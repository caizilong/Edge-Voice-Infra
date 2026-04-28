#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-phase1"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -DBUILD_STACKFLOW=ON \
  -DBUILD_GATEWAY=ON \
  -DBUILD_PHASE1_SERVICES=ON

cmake --build "${BUILD_DIR}" --target \
  unit_manager \
  rag_ipc_service \
  llm_ipc_service \
  tts_ipc_service \
  -j"$(nproc)"
