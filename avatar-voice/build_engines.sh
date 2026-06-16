#!/bin/bash
# Build whisper.cpp + llama.cpp with CUDA (Orin sm_87) and fetch small models.
set -e
export PATH=/usr/local/cuda/bin:$PATH
ARCH=87
cd ~/voice-engines

echo "=== whisper.cpp ==="
cd ~/voice-engines/whisper.cpp
cmake -B build -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=$ARCH -DCMAKE_BUILD_TYPE=Release \
      -DWHISPER_BUILD_EXAMPLES=OFF -DWHISPER_BUILD_TESTS=OFF -DBUILD_SHARED_LIBS=ON
cmake --build build -j3
[ -f models/ggml-base.en.bin ] || bash ./models/download-ggml-model.sh base.en

echo "=== llama.cpp ==="
cd ~/voice-engines/llama.cpp
cmake -B build -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=$ARCH -DCMAKE_BUILD_TYPE=Release \
      -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_SERVER=OFF -DBUILD_SHARED_LIBS=ON
cmake --build build -j3

echo "=== llm model (Qwen2.5-0.5B-Instruct Q4) ==="
mkdir -p ~/voice-engines/models
cd ~/voice-engines/models
[ -f qwen2.5-0.5b-instruct-q4_k_m.gguf ] || \
  wget -q --show-progress https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf

echo "=== artifacts ==="
find ~/voice-engines -name 'libwhisper*' -o -name 'libllama*' 2>/dev/null
ls -lh ~/voice-engines/whisper.cpp/models/ggml-base.en.bin ~/voice-engines/models/*.gguf 2>/dev/null
echo BUILD_ENGINES_DONE
