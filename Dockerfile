# Jetson Avatar Studio — full stack on an L4T base (JetPack 6 / CUDA 12.6, arm64).
# Build ON a Jetson Orin (arm64 + nvidia container runtime). Image is large (torch + builds).
#   docker build -t jetson-avatar-studio .        (or docker/build.sh)
# Models (NeuTTS q8 + neucodec, ~3.8GB) download to ~/.cache/huggingface on first run by default;
# set BAKE_MODELS=1 to pre-download into the image (much larger image).
FROM nvcr.io/nvidia/l4t-jetpack:r36.4.0

ENV DEBIAN_FRONTEND=noninteractive PATH=/usr/local/cuda/bin:$PATH \
    LD_LIBRARY_PATH=/usr/local/cuda/lib64:/usr/lib/aarch64-linux-gnu \
    HF_HUB_DISABLE_PROGRESS_BARS=1
WORKDIR /opt/avatar

# --- system deps ---
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git pkg-config python3-venv python3-pip \
      libasound2-dev alsa-utils espeak-ng sox curl \
      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
      gstreamer1.0-plugins-good gstreamer1.0-plugins-bad libcairo2-dev \
 && rm -rf /var/lib/apt/lists/*

# --- copy source ---
COPY avatar-voice/ /opt/avatar/avatar-voice/
COPY avatar-cpp/   /opt/avatar/avatar-cpp/
COPY setup/        /opt/avatar/setup/
COPY systemd/      /opt/avatar/systemd/

# --- whisper.cpp + llama.cpp (CUDA sm_87) ---
ENV HOME=/root
RUN bash /opt/avatar/setup/build_engines.sh

# --- python venvs (neutts-venv: torch jetson + NeuTTS + llama-cpp CUDA; hw-venv: openwakeword) ---
# NOTE: jetson-ai-lab wheels + llama-cpp CUDA build need the nvidia runtime available at build time.
RUN sed -i 's#~#/root#g' /opt/avatar/setup/setup_venvs.sh && bash /opt/avatar/setup/setup_venvs.sh \
 && ln -s /opt/avatar/avatar-voice /root/avatar-voice

# --- C++ orchestrator + kiosk ---
RUN cd /opt/avatar/avatar-voice && make \
 && g++ -O2 /opt/avatar/avatar-cpp/avatar_ui.cpp -o /opt/avatar/avatar-cpp/avatar_ui \
      $(pkg-config --cflags --libs gstreamer-1.0 cairo)

# --- models ---
RUN bash /opt/avatar/setup/download_models.sh \
 && cp /root/avatar-voice/hw-venv/lib/python3.10/site-packages/openwakeword/resources/models/hey_jarvis_v0.1.onnx \
       /opt/avatar/avatar-voice/hotwords/ || true
ARG BAKE_MODELS=0
RUN if [ "$BAKE_MODELS" = "1" ]; then \
      NEU_BACKBONE_DEVICE=cpu NEU_CODEC_DEVICE=cpu /root/neutts-venv/bin/python3 \
      -c "from neuttsair.neutts import NeuTTS; NeuTTS(backbone_repo='neuphonic/neutts-air-q8-gguf',backbone_device='cpu',codec_repo='neuphonic/neucodec',codec_device='cpu')"; fi

# Run with: --runtime nvidia, mic/speaker (--device /dev/snd) and a tmpfs for the socket.
ENV XDG_RUNTIME_DIR=/run/user/0 AVATAR_MIC=plughw:0,0 AVATAR_MIC_GAIN=4 \
    NEU_BACKBONE_DEVICE=gpu NEU_CODEC_DEVICE=cpu NEU_STREAM_CHUNK=40
EXPOSE 8090 8091
CMD ["/opt/avatar/docker/entrypoint.sh"]
