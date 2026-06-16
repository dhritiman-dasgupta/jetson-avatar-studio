#!/bin/bash
# Python venvs for the avatar stack — the exact, hard-won recipe (JetPack 6 / CUDA 12.6).
set -e
cd ~
JLAB="https://pypi.jetson-ai-lab.io/jp6/cu126"   # Jetson CUDA wheels (torch etc.)

echo "===== neutts-venv (NeuTTS Air TTS, full-GPU) ====="
python3 -m venv ~/neutts-venv
P=~/neutts-venv/bin/pip
$P install -q --upgrade pip
# numpy<2: jetson torch is compiled against numpy 1.x (neucodec declares >=2 but 1.26.4 works fine)
$P install -q 'numpy<2'
# CUDA torch + matching torchaudio from jetson-ai-lab (versions MUST match or libtorchaudio undefined-symbol)
$P install -q 'torch==2.8.0' 'torchaudio==2.8.0' --index-url "$JLAB"
# NeuTTS Air is NOT on PyPI -> git, --no-deps (we pin deps ourselves)
$P install -q --no-deps git+https://github.com/neuphonic/neutts-air.git
# NeuTTS pinned deps: transformers 4.56.1 (5.x breaks HubertModel), resemble-perth (watermarker, required at infer)
$P install -q neucodec soundfile phonemizer librosa 'transformers==4.56.1' 'resemble-perth==1.0.1' flask
# llama-cpp-python REBUILT with CUDA (sm_87) so the GGUF backbone runs on GPU  (~20 min)
CUDACXX=/usr/local/cuda/bin/nvcc CMAKE_ARGS='-DGGML_CUDA=on -DCMAKE_CUDA_ARCHITECTURES=87' FORCE_CMAKE=1 \
  $P install --no-cache-dir --force-reinstall --no-binary llama-cpp-python llama-cpp-python
# numpy may get bumped by the rebuild — pin back
$P install -q 'numpy<2'
~/neutts-venv/bin/python3 -c 'from neuttsair.neutts import NeuTTS; import torch,llama_cpp; print("neutts-venv OK, cuda", torch.cuda.is_available())'

echo "===== hw-venv (openWakeWord hotword, CPU) ====="
python3 -m venv ~/avatar-voice/hw-venv
~/avatar-voice/hw-venv/bin/pip install -q 'numpy<2' openwakeword onnxruntime
~/avatar-voice/hw-venv/bin/python3 -c 'from openwakeword.model import Model; print("hw-venv OK")'

echo "DONE. espeak-ng must be apt-installed (see install_deps.sh)."
