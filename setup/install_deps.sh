#!/bin/bash
# System packages for the avatar stack (JetPack 6 / Ubuntu 22.04 arm64).
set -e
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git pkg-config \
  python3-venv python3-pip \
  libasound2-dev alsa-utils \
  espeak-ng \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  libcairo2-dev sox \
  weston
# CUDA 12.6 toolkit (nvcc) is required for the GPU builds — install from the L4T r36.4 repo if missing:
#   sudo apt-get install -y cuda-toolkit-12-6
echo "deps installed. nvcc: $(/usr/local/cuda/bin/nvcc --version 2>/dev/null | grep -o 'release [0-9.]*' || echo MISSING)"
