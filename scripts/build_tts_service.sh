#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-tts"

mkdir -p "${BUILD_DIR}"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DBUILD_STACKFLOW=ON \
  -DBUILD_GATEWAY=OFF \
  -DBUILD_PHASE1_SERVICES=ON

TARGETS=(tts_ipc_service)
if [[ -f "${ROOT_DIR}/third-party/SummerTTS/include/SynthesizerTrn.h" ]]; then
  TARGETS+=(edge_tts_service)
fi

cmake --build "${BUILD_DIR}" --target "${TARGETS[@]}" -j"$(nproc)"

echo "build done:"
echo "  ${BUILD_DIR}/services/tts-service/tts_ipc_service"
if [[ -f "${ROOT_DIR}/third-party/SummerTTS/include/SynthesizerTrn.h" ]]; then
  echo "  ${BUILD_DIR}/services/tts-service/edge_tts_service"
fi
