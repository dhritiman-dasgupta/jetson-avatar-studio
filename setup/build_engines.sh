#!/bin/bash
# Build whisper.cpp (STT) and llama.cpp (LLM) with CUDA (sm_87) for the worker binaries.
# Libs land in build/ subdirs; the orchestrator Makefile links them. (A copy of the
# original is in avatar-voice/build_engines.sh.)
set -e
mkdir -p ~/voice-engines && cd ~/voice-engines

if [ ! -d whisper.cpp ]; then git clone https://github.com/ggerganov/whisper.cpp; fi
cd whisper.cpp && cmake -B build -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=87 -DBUILD_SHARED_LIBS=ON \
  && cmake --build build -j"$(nproc)" --config Release
cd ~/voice-engines

if [ ! -d llama.cpp ]; then git clone https://github.com/ggerganov/llama.cpp; fi
cd llama.cpp && cmake -B build -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=87 -DBUILD_SHARED_LIBS=ON -DLLAMA_BUILD_TOOLS=OFF \
  && cmake --build build -j"$(nproc)" --config Release || true   # the example `llama` tool may fail to link; libs build fine
echo "engines built (libs in whisper.cpp/build & llama.cpp/build)."
