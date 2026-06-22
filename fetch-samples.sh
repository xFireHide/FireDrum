#!/usr/bin/env bash
#
# fetch-samples.sh — baixa uma BIBLIOTECA de samples de bateria reais (licença
# livre) para ./samples/library/<categoria>/, com muitas opções por peça.
#
# IMPORTANTE: cada arquivo vai para a categoria CERTA. Pastas-fonte "puras"
# (só bumbos, só caixas...) vão inteiras; pastas que são KITS MISTURADOS
# (hh, hh27 = closedhh+crash+kick+snare+ride+...) são roteadas arquivo a
# arquivo pela palavra-chave do nome, para não contaminar categorias.
#
# Fonte: TidalCycles "Dirt-Samples" (uso livre). Solte WAVs seus em
# samples/library/<categoria>/ e eles aparecem na UI.
#
set -Eeuo pipefail
shopt -s inherit_errexit 2>/dev/null || true
shopt -s nocasematch

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly LIB="${SCRIPT_DIR}/samples/library"
readonly RAW="https://raw.githubusercontent.com/tidalcycles/Dirt-Samples/master"
readonly API="https://api.github.com/repos/tidalcycles/Dirt-Samples/contents"

ok=0; skip=0

list_wavs() { # lista nomes .wav de uma pasta do repo
  curl -fsSL --max-time 25 "${API}/$1" 2>/dev/null \
    | grep '"name"' | sed 's/.*: "//; s/".*//' | grep -iE '\.wav$' || true
}

download_to() { # categoria, folder, nome_remoto, prefixo
  local cat="$1" folder="$2" name="$3" prefix="$4"
  local dest="${LIB}/${cat}"; mkdir -p "${dest}"
  local out="${dest}/${prefix}_${name}"
  if curl -fsSL --max-time 30 -o "${out}" "${RAW}/${folder}/${name}" \
     && [[ "$(head -c 4 "${out}" | tr -d '\0')" == "RIFF" ]]; then
    ok=$((ok+1))
  else
    rm -f "${out}"; skip=$((skip+1))
  fi
}

# Pasta PURA -> uma categoria (até <max> arquivos).
pull_pure() {
  local cat="$1" folder="$2" max="$3" prefix="$4" name
  while IFS= read -r name; do
    [[ -z "$name" ]] && continue
    download_to "$cat" "$folder" "$name" "$prefix"
  done < <(list_wavs "$folder" | head -n "$max")
  printf '  %-8s <- %-9s (pura)\n' "$cat" "$folder"
}

# Categoria de um arquivo de KIT misturado, pela palavra-chave do nome.
kit_category() {
  case "$1" in
    *closedhh*|*opendhh*|*openhh*|*closedhat*|*openhat*|*closehat*) echo hihat ;;
    *crash*)         echo crash ;;
    *kick*)          echo kick ;;
    *snare*)         echo snare ;;
    *hit*)           echo tom ;;
    *ride*)          echo ride ;;
    *perc*|*rerc*)   echo perc ;;
    *cowbell*)       echo cowbell ;;
    *clap*)          echo clap ;;
    *)               echo "" ;;
  esac
}

# Pasta de KIT misturado -> roteia cada arquivo para a categoria certa.
pull_kit() {
  local folder="$1" prefix="$2" name cat
  while IFS= read -r name; do
    [[ -z "$name" ]] && continue
    cat="$(kit_category "$name")"
    [[ -z "$cat" ]] && continue
    download_to "$cat" "$folder" "$name" "$prefix"
  done < <(list_wavs "$folder")
  printf '  (kit %-6s roteado por categoria)\n' "$folder"
}

pull_file() { # destino_relativo, origem_no_repo
  local out="${LIB}/$1"; mkdir -p "$(dirname -- "$out")"
  if curl -fsSL --max-time 30 -o "$out" "${RAW}/$2" \
     && [[ "$(head -c 4 "$out" | tr -d '\0')" == "RIFF" ]]; then
    ok=$((ok+1)); else rm -f "$out"; skip=$((skip+1)); fi
}

printf 'Baixando biblioteca para %s ...\n' "${LIB}"

# --- Pastas PURAS ---
pull_pure kick    bd       10 bd
pull_pure kick    808bd     8 808
pull_pure snare   sn       12 sn
pull_pure snare   808sd     8 808
pull_pure tom     808lt     4 808lo
pull_pure tom     808mt     4 808mid
pull_pure tom     808ht     4 808hi
pull_pure hihat   linnhats  6 linn
pull_pure hihat   ho        6 open
pull_pure crash   metal    10 metal
pull_pure crash   808cy     8 808
pull_pure ride    808cy     4 ride808
pull_pure clap    cp        2 cp
pull_pure clap    realclaps 4 real
pull_pure perc    perc      6 perc

# --- Kits MISTURADOS (roteados por categoria) ---
pull_kit hh    hh3
pull_kit hh27  hh27

# --- Curados do kit acústico Gretsch ---
pull_file kick/gretsch.wav             gretsch/013_kick.wav
pull_file snare/gretsch.wav            gretsch/020_snare.wav
pull_file snare/gretsch_hard.wav       gretsch/022_snarehard.wav
pull_file snare/gretsch_slack.wav      gretsch/023_snareslack.wav
pull_file tom/gretsch_hi.wav           gretsch/012_hitom.wav
pull_file tom/gretsch_lo.wav           gretsch/015_lotom.wav
pull_file tom/gretsch_brush_hi.wav     gretsch/000_brushhitom.wav
pull_file tom/gretsch_brush_lo.wav     gretsch/001_brushlotom.wav
pull_file hihat/gretsch_closed.wav     gretsch/004_closedhat.wav
pull_file hihat/gretsch_closed_hard.wav gretsch/005_closedhathard.wav
pull_file hihat/gretsch_open.wav       gretsch/017_openhat.wav
pull_file crash/gretsch_grab.wav       gretsch/007_cymbalgrab.wav
pull_file ride/gretsch.wav             gretsch/019_ridecymbal.wav
pull_file ride/gretsch_bell.wav        gretsch/018_ridebell.wav
pull_file ride/gretsch_rub.wav         gretsch/008_cymbalrub.wav
pull_file cowbell/gretsch.wav          gretsch/006_cowbell.wav

# Normaliza extensões .WAV -> .wav (FS case-insensitive: renomeia via temp).
find "${LIB}" -type f -name '*.WAV' -print0 2>/dev/null | while IFS= read -r -d '' f; do
  base="${f%.WAV}"; mv -f "$f" "${base}.tmp" && mv -f "${base}.tmp" "${base}.wav"
done

printf '\nBiblioteca em %s\n' "${LIB}"
if [[ -d "${LIB}" ]]; then
  for d in "${LIB}"/*/; do
    [[ -d "$d" ]] || continue
    printf '  %-10s %s sons\n' "$(basename -- "$d")" "$(find "$d" -iname '*.wav' | wc -l | tr -d ' ')"
  done
fi
printf 'Total: %d baixados, %d pulados.\n' "${ok}" "${skip}"
