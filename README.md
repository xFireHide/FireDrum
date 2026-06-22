# SomBateria

Software **standalone** de bateria eletrônica que lê os sinais do Arduino
**direto da porta serial** e toca os sons na placa de áudio — **sem MIDI virtual
e sem DAW**. Escrito em C++ com o framework [JUCE](https://juce.com).

Na prática, este programa **funde e substitui** a corrente clássica que todo
mundo monta no Windows:

> **Hairless MIDI → loopMIDI → Addictive Drums (dentro de uma DAW)**

…num único processo, eliminando o MIDI virtual e a DAW pelo caminho.

---

## Índice

- [Por que ele existe](#por-que-ele-existe)
- [O coração: como substituímos o Hairless e o loopMIDI](#o-coração-como-substituímos-o-hairless-e-o-loopmidi)
  - [1. O lugar do Hairless: ler bytes da serial](#1-o-lugar-do-hairless-ler-bytes-da-serial)
  - [2. Decodificar MIDI cru (o que o Hairless faz por dentro)](#2-decodificar-midi-cru-o-que-o-hairless-faz-por-dentro)
  - [3. O lugar do loopMIDI: a fila lock-free](#3-o-lugar-do-loopmidi-a-fila-lock-free)
  - [4. O lugar do Addictive Drums + DAW: o motor de áudio](#4-o-lugar-do-addictive-drums--daw-o-motor-de-áudio)
- [Fluxo completo de um hit](#fluxo-completo-de-um-hit)
- [Modelo de threads e tempo real](#modelo-de-threads-e-tempo-real)
- [Estrutura do projeto](#estrutura-do-projeto)
- [Compilar e rodar](#compilar-e-rodar)
- [Configurar para o seu hardware](#configurar-para-o-seu-hardware)
- [A interface](#a-interface)
- [Troubleshooting](#troubleshooting)

---

## Por que ele existe

A corrente tradicional tem **quatro programas** e duas pontes artificiais
(o cabo MIDI virtual e o host da DAW). Cada elo adiciona latência e um ponto de
falha. Para bateria, latência importa: o ouvido percebe atraso abaixo de ~10 ms
entre bater e ouvir.

| Programa tradicional | O que fazia | Quem assume aqui |
| --- | --- | --- |
| **Hairless MIDI** | Lê a serial do Arduino e converte byte → MIDI | `SerialAudioBridge` (lê serial e decodifica MIDI **internamente**) |
| **loopMIDI** | Cria uma porta MIDI virtual entre apps | **Eliminado** — `LockFreeQueue` (memória compartilhada, mesmo processo) |
| **Addictive Drums** | Sampler que recebe MIDI e toca os WAVs | `DrumSampler` + `SampleBank` |
| **DAW (Reaper, etc.)** | Hospeda o VST e roteia o áudio | **Eliminado** — `AudioEngine` fala direto com o `AudioDeviceManager` |

Resultado: **serial → som dentro de um único processo**, sem o overhead/jitter
do loopback MIDI do SO nem o buffer extra de uma DAW.

---

## O coração: como substituímos o Hairless e o loopMIDI

O ponto central da pergunta. Vamos peça por peça.

### 1. O lugar do Hairless: ler bytes da serial

O Hairless abre a porta serial USB (ex.: `/dev/cu.usbserial-XXXX`) e fica lendo
os bytes que o Arduino manda. Nós fazemos exatamente isso numa **thread
dedicada**, com `termios` (POSIX) configurado em modo RAW, 8N1, leitura não
bloqueante.

- `SerialSource.h` / `SerialSource.cpp` — abstração `ISerialSource`. A
  implementação real é `PosixSerialSource` (macOS/Linux via `termios`). Há também
  uma `SimulatedSerialSource` para desenvolver sem hardware.
- `SerialAudioBridge.h` — a thread (`juce::Thread`) que abre a fonte e fica lendo
  blocos de bytes crus, byte a byte.

```cpp
// SerialAudioBridge::run()  (thread serial)
while (! threadShouldExit())
{
    const int n = source->read (readBuf, sizeof (readBuf));
    for (int i = 0; i < n; ++i)
        handleMidiByte (static_cast<std::uint8_t> (readBuf[i]));
}
```

### 2. Decodificar MIDI cru (o que o Hairless faz por dentro)

**Descoberta importante deste projeto:** o sketch do Arduino, por ter sido feito
para o Hairless, **emite bytes MIDI crus** — não texto `"id:velocity"`. Capturando
a serial vimos:

```
99 24 64   89 24 00   99 2A 51 ...
└┬┘ └┬┘ └┬┘
 │   │   └─ velocity (0x64 = 100)
 │   └───── nota (0x24 = 36 = bumbo)
 └───────── status: 0x9n = Note On no canal n (0x99 = canal 10, o canal de bateria do GM)
```

Então o `SerialAudioBridge` embute um **decodificador MIDI** — a mesma função que
o Hairless faz internamente. É uma máquina de estados que:

- distingue **bytes de status** (bit alto = 1, `0x80–0xFF`) de **bytes de dados**
  (`0x00–0x7F`);
- trata **running status** (o MIDI pode omitir o status repetido);
- ignora mensagens de **tempo real** (`0xF8–0xFF`) intercaladas, sem quebrar o parse;
- converte **Note On (`0x9n`) com velocity > 0** num gatilho de bateria
  (`pieceId = nota MIDI`, `velocity` normalizada para `[0,1]`);
- trata **Note On com velocity 0** e **Note Off (`0x8n`)** como "soltar" — drums
  são one-shot, então isso só vai para o log, não dispara som.

```cpp
void handleMidiByte (std::uint8_t b)
{
    if (b & 0x80) {                 // byte de STATUS
        if (b >= 0xF8) return;      // real-time: ignora, preserva running status
        if (b >= 0xF0) { runningStatus = 0; dataIndex = 0; return; } // system common
        runningStatus = b;          // status de canal (Note On/Off/CC/…)
        dataIndex = 0;
        dataExpected = expectedDataBytes (b);
        return;
    }
    if (runningStatus == 0) return;          // dado órfão
    if (dataIndex < 2) data[dataIndex++] = b;
    if (dataIndex >= dataExpected) {
        dispatchChannelMessage();            // mensagem completa → gatilho/log
        dataIndex = 0;                       // running status: pronto pra próxima
    }
}
```

É isto que torna o app um **substituto direto do Hairless**: ele entende o mesmo
protocolo que seu Arduino já fala, sem reprogramar nada.

### 3. O lugar do loopMIDI: a fila lock-free

No esquema clássico, o loopMIDI é o "cabo" que leva a mensagem do Hairless até o
sampler — só que é uma **porta MIDI virtual do sistema operacional**, com
overhead e jitter.

Aqui as duas pontas vivem no **mesmo processo**, então a "ponte" é só uma
**fila circular lock-free** em memória — `LockFreeQueue` (`LockFreeQueue.h`),
construída sobre `juce::AbstractFifo`, que gerencia os índices de leitura/escrita
de forma atômica (acquire/release). É **Single-Producer / Single-Consumer**:

- **produtor:** a thread serial empurra `DrumMessage` (`queue.push`);
- **consumidor:** a thread de áudio drena (`queue.pop`).

Nenhum mutex, nenhuma alocação, nenhuma syscall. Substitui o cabo MIDI virtual
por uma troca de ponteiros atômica — ordens de magnitude mais rápida e previsível.

> Há **duas** filas: uma de `DrumMessage` (serial → áudio, dispara o som) e uma
> de `MidiLogEvent` (serial → UI, alimenta o monitor estilo Hairless). Cada uma é
> SPSC com um único consumidor.

### 4. O lugar do Addictive Drums + DAW: o motor de áudio

O `DrumSampler` (`DrumSampler.h`) é o sampler polifônico. No callback de áudio ele:

1. drena a fila lock-free e dispara uma **voz** por mensagem (pool de tamanho fixo,
   com *voice-stealing* da voz mais silenciosa quando enche);
2. mixa todas as vozes ativas no buffer de saída, com ganho por voz derivado da
   velocity.

Os sons vêm do `SampleBank` (`SampleBank.h`): buffers `juce::AudioBuffer<float>`
pré-carregados em RAM. Como não exigimos arquivos WAV, há um **kit sintético**
gerado em memória (bumbo/tom graves, caixa/prato agudos), com 3 timbres trocáveis
(`DrumKit.h`).

E o `AudioEngine` (`AudioEngine.h`) é quem dispensa a DAW: implementa
`juce::AudioIODeviceCallback` e fala **direto com o `AudioDeviceManager`** (a placa
de som), sem host de VST no meio.

---

## Fluxo completo de um hit

```
 Você bate no pad
        │
        ▼
 Arduino  ──USB serial (bytes MIDI: 99 24 64)──►  [Thread Serial]
                                                   SerialSource (termios)
                                                   handleMidiByte() decodifica MIDI
                                                        │
                          ┌─────────────────────────────┼──────────────────────────┐
                          ▼ push                                                     ▼ push
                   LockFreeQueue<DrumMessage>                            LockFreeQueue<MidiLogEvent>
                          │ pop (thread de áudio)                                    │ pop (thread de UI, 60 Hz)
                          ▼                                                          ▼
                    DrumSampler.process()                                      Monitor MIDI
                    • dispara voz (nota → sample)                              (console estilo Hairless)
                    • ganho = f(velocity) × volume da peça                      + pads piscam
                    • mixa as vozes ativas
                          │
                          ▼
                 AudioDeviceManager  ──►  Placa de som  ──►  🔊
```

---

## Modelo de threads e tempo real

Três contextos, sem contenção entre eles:

| Thread | Faz | Pode bloquear/alocar? |
| --- | --- | --- |
| **Serial** | I/O da porta, parse MIDI, `push` nas filas | Sim (I/O é aqui, longe do áudio) |
| **Áudio** (callback) | `pop` da fila, síntese, mix | **NÃO** |
| **UI/mensagens** | `pop` do log, desenha, controles | Sim |

**Regras estritas no callback de áudio** (`DrumSampler::process`):

- ❌ sem `new`/`malloc` — o pool de vozes e as filas são pré-alocados; o
  `AudioBuffer` de saída *envelopa* os ponteiros da placa sem copiar;
- ❌ sem mutex/lock — a única comunicação entre threads é via `std::atomic`
  (as filas e os controles do mixer);
- ❌ sem I/O e sem exceções.

A **troca de kit** é feita com *double-buffer*: a UI gera o kit novo num segundo
`SampleBank` e publica o ponteiro com `store-release`; o áudio lê com
`load-acquire`. Trocar de timbre nunca trava o áudio.

---

## Estrutura do projeto

```
sombateria/
├── CMakeLists.txt          # Build (baixa o JUCE via FetchContent)
├── run.sh                  # Configura + compila + executa
├── Source/
│   ├── Main.cpp            # App standalone + UI (pads, mixer, monitor MIDI)
│   ├── AudioEngine.h       # Cola tudo; AudioIODeviceCallback (caminho RT)
│   ├── SerialSource.h/.cpp # ISerialSource: PosixSerialSource (termios) + simulada
│   ├── SerialAudioBridge.h # Thread serial + decodificador MIDI + filas
│   ├── LockFreeQueue.h     # FIFO SPSC lock-free (substitui o loopMIDI)
│   ├── DrumMessage.h       # POD do gatilho (nota + velocity) que vai pro áudio
│   ├── MidiLogEvent.h      # POD do evento de log que vai pra UI
│   ├── DrumSampler.h        # Sampler polifônico (substitui o Addictive Drums)
│   ├── SampleBank.h        # Buffers em RAM + síntese do kit
│   ├── DrumKit.h           # Parâmetros dos kits sintéticos
│   └── HitTelemetry.h      # Telemetria atômica áudio→UI (flash dos pads)
└── README.md
```

---

## Compilar e rodar

**Pré-requisitos:** `cmake` ≥ 3.22 e um compilador C++20 (AppleClang/Clang/GCC).
O JUCE 8 é baixado automaticamente na primeira configuração (precisa de rede).

```bash
./run.sh              # configura (Release), compila e executa
./run.sh --debug      # build Debug
./run.sh --build-only # só compila, não executa
./run.sh --clean      # apaga build/ antes
./run.sh --help
```

Ou manualmente:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

> **Nota (macOS):** o `project()` no CMake habilita as linguagens **C e CXX** — o
> JUCE compila fontes C (juceaide, harfbuzz). Só `CXX` falha com
> `CMAKE_C_COMPILE_OBJECT` ausente.

---

## Configurar para o seu hardware

A porta e o baud estão em `Source/Main.cpp`:

```cpp
const juce::String portPath = "/dev/cu.usbserial-A5069RR4";  // sua porta
auto src = std::make_unique<PosixSerialSource> (portPath, 115200);
```

Para descobrir a porta no macOS/Linux:

```bash
ls /dev/cu.usbserial-* /dev/cu.usbmodem-* /dev/cu.wchusbserial-* 2>/dev/null
```

- `usbserial`  → chip FTDI (FT232R) — driver nativo no macOS
- `usbmodem`   → Arduino Uno oficial (ATmega16U2)
- `wchusbserial` → clone com CH340 (precisa do driver CH340 no macOS)

O **baud** deve bater com o `Serial.begin(...)` do seu sketch (115200 é o comum
para setups de Hairless).

Para usar **WAVs reais** em vez do kit sintético, carregue por nota MIDI no setup:

```cpp
engine.loadPiece (36, juce::File ("/caminho/kick.wav"));   // 36 = bumbo
engine.loadPiece (38, juce::File ("/caminho/snare.wav"));  // 38 = caixa
```

---

## A interface

Tema escuro estilo drum machine, com:

- **Seletor de Kit** — `Acustico`, `808 Eletronico`, `Lo-Fi` (troca ao vivo).
- **Audio / Latencia** — abre o seletor nativo de placa de som e buffer size.
- **Volume** master e **Sensibilidade** (curva de velocity) ajustáveis ao vivo.
- **Pads** que **piscam** no hit (brilho ∝ velocity) e descobrem novas notas
  sozinhos; cada um tem volume + mute (mixer por peça).
- **Monitor MIDI** — console rolando em tempo real, igual ao do Hairless:
  cada Note On (verde) / Note Off (cinza) com timestamp, nota, nome e velocity.

---

## Troubleshooting

**A porta não aparece em `/dev/cu.*`**
- USB do Mac não enumera nada → quase sempre **cabo charge-only** ou **adaptador
  USB-C sem linha de dados**. Teste o adaptador com um pendrive; troque o cabo.
- LED de power aceso **não** garante dados (cabo só de carga acende o LED).

**Aparece `FT232R`/dispositivo, mas sem `/dev/cu.usbserial`**
- Driver não vinculou ainda. No macOS o FTDI é nativo; aguarde alguns segundos
  ou reconecte. Clones CH340 exigem instalar o driver CH340.

**Conecta mas não sai som / não loga nada**
- Confira o **baud** (precisa bater com o sketch) e a **porta** em `Main.cpp`.
- Veja os bytes crus: `python3` lendo a porta em modo raw deve mostrar `99 .. ..`
  (Note On). Se vier tudo `00`, o baud está errado.
- Só **um** processo pode abrir a porta — feche o Arduino IDE / Serial Monitor.

**Som muito baixo**
- O Arduino pode mandar velocities baixos. Use o slider **Sensibilidade** (mais
  baixo = realça hits leves) e/ou o **Volume** master.
```
