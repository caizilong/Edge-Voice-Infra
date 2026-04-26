#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE_DIR="${ROOT_DIR}/services/tts-service"
BUILD_DIR="${SERVICE_DIR}/build"

mkdir -p "${BUILD_DIR}"
cmake -S "${SERVICE_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo "build done: ${BUILD_DIR}/tts_service"