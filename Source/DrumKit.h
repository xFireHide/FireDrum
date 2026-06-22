#pragma once

namespace bateria
{

/**
    Parâmetros de síntese de um kit. Um único motor de síntese
    (SampleBank::generateDrumSound) lê esses valores e produz timbres
    diferentes por kit, sem precisar de arquivos WAV.
*/
struct KitParams
{
    const char* name;        // nome exibido na UI
    float tuning;            // multiplica as frequências (1.0 = afinação padrão)
    float decayScale;        // multiplica os decaimentos (1 = natural)
    float noiseScale;        // ruído de caixa/pratos (1 = natural, 0 = só tom)
    float saturation;        // drive de saturação tanh (1 = limpo)
    float brightness;        // multiplica os cutoffs dos filtros (1 = natural)
    float srCrush;           // sample&hold: 1 = limpo, 0.25 = 1/4 da taxa (lo-fi)
};

/// Kits disponíveis. Acrescente novos aqui e eles aparecem no menu da UI.
inline constexpr KitParams kKits[] = {
    //  name              tuning decay  noise  sat   bright srCrush
    { "Acustico",         1.00f, 1.00f, 1.00f, 1.20f, 1.00f, 1.00f },
    { "Rock",             0.95f, 1.30f, 1.10f, 1.70f, 1.15f, 1.00f },
    { "Punch (Metal)",    0.90f, 1.05f, 1.15f, 2.20f, 1.30f, 1.00f },
    { "808 Eletronico",   0.82f, 2.30f, 0.18f, 1.50f, 0.85f, 1.00f },
    { "Jazz",             1.12f, 0.78f, 0.95f, 1.10f, 1.20f, 1.00f },
    { "Lo-Fi",            0.90f, 0.72f, 1.20f, 1.30f, 0.70f, 0.28f },
};

inline constexpr int kNumKits = static_cast<int> (sizeof (kKits) / sizeof (kKits[0]));

} // namespace bateria
