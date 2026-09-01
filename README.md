# Jetson Avatar Studio — on-device talking avatar (voice + cloned TTS)

A complete on-device **talking avatar** for the **Jetson Orin Nano Super** (8GB, JetPack 6 / L4T r36.4, CUDA 12.6):
hear → understand → reply in a **cloned voice**, with a round-display kiosk UI and on-screen captions.

```
mic ─▶ trigger (hotword "hey jarvis" OR push-to-talk) ─▶ STT (whisper.cpp) ─▶ LLM (llama.cpp / Qwen) ─▶ NeuTTS cloned voice (streamed) ─▶ speaker
                                                                         └──────────────── captions broadcast over a Unix socket to the kiosk + web pages
```

Everything runs **on the device**. No cloud STT, no cloud LLM, no cloud TTS — the mic audio
never leaves the board.

## Why this is the interesting part

An 8 GB Jetson is not enough memory for this pipeline done naively, and the engineering is all in
what that forces:

- **The whole stack has to be resident at once.** Speech recognition, the language model and the
  cloned-voice TTS are all pinned in GPU memory, because reloading a model (~4 s) costs more than
  living with the pressure — **measured ~8 s/turn all-resident against ~12 s when evicting**.
- **It still does not fit.** q8 NeuTTS with its codec on the GPU swap-thrashes into an OOM reboot.
  The codec runs on **CPU** instead. That split is the difference between working and not.
- **The floor is honest.** The ~8 s "thinking" pause is the Orin's own STT + LLM inference floor,
  not a bug to tune away. The only way past it is moving STT and the LLM to a faster machine and
  keeping TTS on the Jetson.
- **Speech starts before generation finishes.** TTS streams in chunks piped straight to `aplay`,
  so audio begins ~1–2 s in rather than after the full utterance is synthesised.

Two ways to talk to it: say **"hey jarvis"**, or hold push-to-talk from any browser on the LAN.

---

## Components

| Path | What it is |
|---|---|
| `avatar-voice/` | **C++ voice orchestrator** (the daemon). Owns the mic, runs the state machine, manages models (load/evict), IPC over a Unix socket. |
| `avatar-voice/src/main.cpp` | Orchestrator entry: mic capture, VAD/hotword/PTT triggers, pipeline wiring, IPC. |
| `avatar-voice/include/*.h` | Header-only modules: `model_manager` (load/evict + VRAM budget), `pipeline` (STT→LLM→TTS state machine), `ipc_server` (Unix-socket JSON lines + caption replay), `vad`, `audio_alsa`, `subprocess`, `json_min`, `whisper_stt`, `llama_llm`. |
| `avatar-voice/Makefile` | Builds `avatar-voice` (orchestrator), `stt-worker` (whisper.cpp), `llm-worker` (llama.cpp). |
| `avatar-voice/neutts_worker.py` | **NeuTTS Air** cloned-voice TTS worker (streaming). Protocol: stdin `GEN <text>` → streams chunks to `aplay` → stdout `RPL done`. Runs in `neutts-venv`. |
| `avatar-voice/hotword_worker.py` | **openWakeWord** detector (runs in `hw-venv`, CPU). Emits `WAKE <key> <score>` + live `SCORE <score>`. |
| `avatar-voice/ptt_bridge.py` | Flask **push-to-talk web page** (`:8091`) + bridge to the orchestrator socket. Shows state, captions, **wake-word confidence + mic gain/level bars**. |
| `avatar-voice/neutts_server.py` | Flask **NeuTTS tuning page** (`:8090`) — type text, tune temperature/top-k/top-p, hear the cloned reference voice. (GPU; don't run alongside the orchestrator — both load NeuTTS = OOM on 8GB.) |
| `avatar-voice/hotwords/` | Active wake-word ONNX (`hey_jarvis_v0.1.onnx`). `hotwords_disabled/` holds others (`hey_speaker`, `hey_kiki`). |
| `avatar-voice/ref/` | Voice-clone reference clips (not included - supply your own). |
| `avatar-voice/f5_worker.py` | Legacy F5-TTS-ONNX worker (replaced by NeuTTS; kept for reference). |
| `avatar-cpp/avatar_ui.cpp` | **Kiosk UI** — GStreamer (NVDEC) plays the avatar video + a cairo overlay (cyan ring, telemetry, **live captions** from the socket). Build: `g++ -O2 avatar_ui.cpp -o avatar_ui $(pkg-config --cflags --libs gstreamer-1.0 cairo)`. |
| `avatar-studio/kiosk.sh` | Boot kiosk launcher (Weston + avatar_ui) from getty autologin on tty1. |
| `systemd/*.service` | `avatarvoice` (orchestrator), `ptt-bridge`, `neutts-tuner` — user services. |
| `context/*.md` | **Dev context / build journal** — the hard-won gotchas and decisions. Read these first. |

---

## Architecture notes (the important bits)

- **Triggers (both active):** say **"hey jarvis"** (openWakeWord, pretrained, gated VAD collects the utterance) **OR** hold the push-to-talk button (`:8091`, or Spacebar). Set `AVATAR_PTT` is no longer used — both coexist.
- **Models (all GPU-resident, pinned):** NeuTTS (q8 GGUF backbone, codec on CPU), whisper `tiny.en`, Qwen2.5-0.5B-Instruct Q4. Kept resident because **reload (~4s) costs more than the mild swap** — measured all-resident ~8s/turn vs evict ~12s.
- **NeuTTS streaming:** `infer_stream` yields ~chunks piped raw to `aplay` so playback starts ~1–2s in. Chunk size = `NEU_STREAM_CHUNK` frames (40 ≈ 0.8s; bigger = smoother but later first audio).
- **8GB memory reality:** q8 NeuTTS + GPU codec **does not fit** (swap-thrashes → OOM reboot). Codec runs on **CPU**. The ~8s "thinking" is the Orin's STT+LLM inference floor — the only way past it is **offloading STT/LLM to a faster machine** and keeping TTS on the Jetson.
- **IPC:** orchestrator broadcasts JSON lines (`state`/`transcript`/`reply`/`hwscore`/`micgain`/`miclevel`) over `$XDG_RUNTIME_DIR/avatar-voice.sock`; new clients get the latest cached state on connect. The kiosk + both web pages are clients.

---

## Build and run

Requires: Jetson Orin (8GB+), JetPack 6 / L4T r36.4, CUDA 12.6 toolkit, internet.

```bash
# 1. system deps + engines + models + venvs  (see setup/ scripts; ~30-60 min, downloads several GB)
bash setup/install_deps.sh        # apt: build tools, alsa, espeak-ng, gstreamer, weston, python3-venv
bash setup/build_engines.sh       # clone+build whisper.cpp & llama.cpp with CUDA (sm_87)
bash setup/setup_venvs.sh         # neutts-venv (torch jetson-ai-lab + neutts + llama-cpp-python CUDA) + hw-venv (openwakeword)
bash setup/download_models.sh     # whisper tiny.en, Qwen 0.5B, NeuTTS q8 + neucodec, hey_jarvis

# 2. build the C++ orchestrator + kiosk
cd avatar-voice && make && cd ..
g++ -O2 avatar-cpp/avatar_ui.cpp -o avatar-cpp/avatar_ui $(pkg-config --cflags --libs gstreamer-1.0 cairo)

# 3. install + start services
cp systemd/*.service ~/.config/systemd/user/
loginctl enable-linger $USER          # so services survive logout/reboot
systemctl --user daemon-reload
systemctl --user enable --now avatarvoice ptt-bridge

# 4. open the push-to-talk page from any PC on the LAN
#    http://<jetson-ip>:8091/      (hold to talk, or say "hey jarvis")
```

> The Jetson's IP jumps via DHCP — resolve with `ping -4 jetson-desktop.local`. (mDNS can collide if another device shares the hostname; verify the SSH host key.)

## Docker

`Dockerfile` builds the whole stack on an L4T base image. See `docker/` for build/run/push helpers. Run with `--runtime nvidia` and device mounts for audio + the socket. The image is large (torch + models); models can be baked in or downloaded on first run.

```bash
docker/build.sh           # build the image
docker/push.sh            # tag + push to your free registry (edit REGISTRY first; needs docker login)
docker/run.sh             # run with GPU + audio + the runtime socket dir mounted
```
