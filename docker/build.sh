#!/bin/bash
# Build the image ON a Jetson Orin (arm64 + nvidia container runtime).
set -e
cd "$(dirname "$0")/.."
# BAKE_MODELS=1 pre-downloads NeuTTS into the image (bigger but offline-ready).
docker build --build-arg BAKE_MODELS="${BAKE_MODELS:-0}" -t jetson-avatar-studio:latest .
echo "built jetson-avatar-studio:latest"
