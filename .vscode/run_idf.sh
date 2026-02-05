#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

ESP_IDF_PATH="${1:-}"
ESP_IDF_CURRENT_SETUP="${2:-}"
BUILD_DIR="${3:-}"
BOARD="${4:-}"
ACTION="${5:-}"
PORT="${6:-}"

if [[ -z "$BUILD_DIR" || -z "$BOARD" || -z "$ACTION" ]]; then
  echo "Usage: run_idf.sh <esp_idf_path> <esp_idf_current_setup> <build_dir> <board> <action> [port]" >&2
  exit 2
fi

if [[ -z "$ESP_IDF_PATH" ]]; then
  ESP_IDF_PATH="$ESP_IDF_CURRENT_SETUP"
fi
if [[ -z "$ESP_IDF_PATH" && -n "${IDF_PATH:-}" ]]; then
  ESP_IDF_PATH="$IDF_PATH"
fi
if [[ -z "$ESP_IDF_PATH" ]]; then
  echo "ESP-IDF path not set. Configure idf.espIdfPath/idf.currentSetup or IDF_PATH." >&2
  exit 1
fi

EXPORT_SH="$ESP_IDF_PATH/export.sh"
if [[ ! -f "$EXPORT_SH" ]]; then
  echo "Cannot find $EXPORT_SH" >&2
  exit 1
fi

if [[ ("$ACTION" == "flash" || "$ACTION" == "monitor") && -z "$PORT" ]]; then
  echo "Serial port not set. Configure idf.port (or pass as arg)." >&2
  exit 1
fi

# shellcheck source=/dev/null
source "$EXPORT_SH" >/dev/null 2>&1

run_idf() {
  if [[ -n "$PORT" ]]; then
    idf.py -B "$BUILD_DIR" "-DBOARD=$BOARD" -p "$PORT" "$ACTION"
  else
    idf.py -B "$BUILD_DIR" "-DBOARD=$BOARD" "$ACTION"
  fi
}

if [[ "$ACTION" == "fullclean" ]]; then
  max_retries=3
  attempt=1
  while [[ $attempt -le $max_retries ]]; do
    set +e
    out="$(run_idf 2>&1)"
    code=$?
    set -e

    [[ -n "$out" ]] && echo "$out"

    if [[ $code -eq 0 ]]; then
      exit 0
    fi

    if grep -q "doesn't seem to be a CMake build directory" <<< "$out"; then
      if [[ -d "$BUILD_DIR" ]]; then
        echo "idf.py fullclean refused non-CMake dir. Removing '$BUILD_DIR' directly..."
        rm -rf "$BUILD_DIR"
        echo "Removed '$BUILD_DIR'."
      else
        echo "'$BUILD_DIR' not found; nothing to clean."
      fi
      exit 0
    fi

    if [[ $attempt -lt $max_retries ]] && grep -Eqi "PermissionError|busy|resource temporarily unavailable|text file busy" <<< "$out"; then
      echo "Retrying fullclean after file-lock error (attempt $attempt/$max_retries)..."
      sleep 2
      attempt=$((attempt + 1))
      continue
    fi

    exit $code
  done
else
  run_idf
fi
