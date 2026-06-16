#!/bin/bash
# Download all runtime models. NeuTTS (q8 GGUF + neucodec) auto-download via HF on first run;
# this pre-fetches whisper + the LLM, and notes the rest.
set -e
WM=~/voice-engines/whisper.cpp/models
mkdir -p "$WM" ~/voice-engines/models

echo "=== whisper tiny.en (STT) ==="
curl -sL -o "$WM/ggml-tiny.en.bin" \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin
# (base.en is more accurate but slower: ggml-base.en.bin)

echo "=== Qwen2.5-0.5B-Instruct Q4 (LLM) ==="
curl -sL -o ~/voice-engines/models/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf

echo "=== NeuTTS (auto-downloads to ~/.cache/huggingface on first worker run) ==="
echo "    backbone: neuphonic/neutts-air-q8-gguf   codec: neuphonic/neucodec  (~3.8GB total)"
echo "=== hotword: hey_jarvis is PRETRAINED inside openwakeword (no download needed) ==="
echo "    it is copied to avatar-voice/hotwords/ from hw-venv/.../openwakeword/resources/models/"
echo "DONE."
