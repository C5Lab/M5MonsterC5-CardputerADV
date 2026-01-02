#!/usr/bin/env bash
set -euo pipefail

IMAGE="${IDF_DOCKER_IMAGE:-espressif/idf:v6.0-beta1}"
USE_DOCKER="${USE_DOCKER:-1}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# go two levels up: .github/scripts -> repo root
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
APP_DIR="${REPO_ROOT}/"
CACHE_DIR="${IDF_CACHE_DIR:-${REPO_ROOT}/.espressif}"

usage() {
  cat <<'EOF'
Usage: container_build.sh [--no-docker]

Builds ESP32S3 firmware using esp-idf release/v6.0.
Defaults to running inside Docker (espressif/idf:v6.0-beta1).

Options:
  --no-docker   Run idf.py directly (for GitHub Actions container job or host with esp-idf installed)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-docker)
      USE_DOCKER=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

build_native() {
  cd "$APP_DIR"
  export IDF_TARGET=esp32s3
  if [ -f /opt/esp/idf/export.sh ]; then
    # shellcheck disable=SC1091
    . /opt/esp/idf/export.sh >/dev/null 2>&1
  fi
  local extra_flags="${IDF_PY_FLAGS:-}"
  if [[ "${IDF_TARGET}" == "esp32s3" && "${extra_flags}" != *"--preview"* ]]; then
    extra_flags="--preview ${extra_flags}"
  fi
  idf.py ${extra_flags} build
}

if [[ "$USE_DOCKER" == "0" || "$USE_DOCKER" == "false" ]]; then
  build_native
  exit 0
fi

mkdir -p "$CACHE_DIR"

docker run --rm \
  --workdir /project \
  --env IDF_TARGET=esp32s3 \
  --env IDF_PY_FLAGS="${IDF_PY_FLAGS:-}" \
  --volume "${APP_DIR}:/project" \
  --volume "${CACHE_DIR}:/root/.espressif" \
  "$IMAGE" \
  bash -c '. /opt/esp/idf/export.sh >/dev/null 2>&1; \
           extra="${IDF_PY_FLAGS:-}"; \
           [ "$IDF_TARGET" = "esp32s3" ] && case "$extra" in (*--preview*) ;; (*) extra="--preview $extra";; esac; \
           idf.py $extra build'
