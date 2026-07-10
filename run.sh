#!/usr/bin/env bash
#
# run.sh — configura, compila e executa o FireKit (JUCE standalone).
#
# Uso:
#   ./run.sh              build (Release) e roda
#   ./run.sh --debug      build em Debug e roda
#   ./run.sh --build-only compila sem executar
#   ./run.sh --clean      apaga o diretório de build antes
#   ./run.sh --jobs N     paralelismo do compilador (default: nº de CPUs)
#   ./run.sh --help       esta ajuda
#
# Pré-requisitos: cmake (>=3.22) e um compilador C++20. O JUCE é baixado
# automaticamente via FetchContent na primeira configuração (precisa de rede).
#
set -Eeuo pipefail
shopt -s inherit_errexit 2>/dev/null || true

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly SCRIPT_DIR
readonly BUILD_DIR="${SCRIPT_DIR}/build"
readonly TARGET="FireKit"
readonly PRODUCT_NAME="FireKit"

# Defaults configuráveis por flag.
BUILD_TYPE="Release"
BUILD_ONLY=0
DO_CLEAN=0
JOBS=""

log()  { printf '\033[1;36m[run]\033[0m %s\n' "$*" >&2; }
err()  { printf '\033[1;31m[erro]\033[0m %s\n' "$*" >&2; }

usage() {
  sed -n '2,/^set -/{/^set -/d;s/^# \{0,1\}//;p;}' "${BASH_SOURCE[0]}"
}

detect_jobs() {
  if command -v nproc >/dev/null 2>&1; then nproc
  elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.ncpu
  else printf '4'; fi
}

parse_args() {
  while (( $# )); do
    case "$1" in
      --debug)      BUILD_TYPE="Debug" ;;
      --release)    BUILD_TYPE="Release" ;;
      --build-only) BUILD_ONLY=1 ;;
      --clean)      DO_CLEAN=1 ;;
      --jobs)       shift; JOBS="${1:?--jobs requer um número}" ;;
      -h|--help)    usage; exit 0 ;;
      *)            err "argumento desconhecido: $1"; usage; exit 2 ;;
    esac
    shift
  done
  [[ -n "$JOBS" ]] || JOBS="$(detect_jobs)"
}

check_prereqs() {
  command -v cmake >/dev/null 2>&1 || { err "cmake não encontrado no PATH"; exit 1; }
}

# Localiza o binário/app gerado, lidando com layouts macOS (.app) e Linux.
locate_binary() {
  local app
  # macOS: bundle .app
  app="$(find "${BUILD_DIR}" -type d -name "${PRODUCT_NAME}.app" -print -quit 2>/dev/null || true)"
  if [[ -n "$app" ]]; then
    printf '%s/Contents/MacOS/%s' "$app" "$PRODUCT_NAME"
    return 0
  fi
  # Linux/outros: executável solto dentro de *_artefacts
  app="$(find "${BUILD_DIR}" -type f \( -name "$TARGET" -o -name "$PRODUCT_NAME" \) -perm -u+x -print -quit 2>/dev/null || true)"
  [[ -n "$app" ]] && { printf '%s' "$app"; return 0; }
  return 1
}

main() {
  parse_args "$@"
  check_prereqs

  if (( DO_CLEAN )); then
    log "limpando ${BUILD_DIR}"
    rm -rf -- "${BUILD_DIR}"
  fi

  log "configurando (CMAKE_BUILD_TYPE=${BUILD_TYPE})"
  cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

  log "compilando ${TARGET} com ${JOBS} jobs"
  cmake --build "${BUILD_DIR}" --target "${TARGET}" --config "${BUILD_TYPE}" --parallel "${JOBS}"

  local bin
  if ! bin="$(locate_binary)"; then
    err "binário não localizado em ${BUILD_DIR} após o build"
    exit 1
  fi
  log "binário: ${bin}"

  if (( BUILD_ONLY )); then
    log "--build-only: pulando execução"
    exit 0
  fi

  log "executando…"
  exec "${bin}"
}

main "$@"
