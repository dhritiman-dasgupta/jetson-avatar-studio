---
name: project-voice-ai-replica
description: "Laptop WSL replica of the Jetson voice stack — built 2026-06-11 without the Jetson, fully working; paths, ports, gotchas"
metadata: 
  node_type: memory
  type: project
  originSessionId: 14cf6637-ccd3-4379-985f-9e0951580f10
---

# Voice-AI laptop replica (WSL `avatar` distro, E: drive) — 2026-06-11

Built WITHOUT the Jetson (it was off/unavailable). Architecture = the **June-4-era browser-driven loop** (kiosk browser does hotword-SSE → record → STT → LLM → TTS → play), NOT the later orchestrator/:8010 design. Related: [[project_voice_ai]], [[project_wsl_docker_gpu]].

**Staging repo (git): `E:\voice-ai-replica`** — services/, avatar-studio/, systemd/, deploy.sh, setup-venvs.sh, smoke.sh, status.sh, warm.sh, debug1.sh. Edit here → copy into WSL → rebuild/restart. Sources: E:\avatar-studio (full May base) + E:\avatar-studio-jetson\src (June-4 overlay) + E:\voice-ai-jetson (stt-main.py, hotword main.py). REWRITTEN from scratch: services/llm/main.py (ollama proxy), services/tts/main.py (Piper), src/lib/audio-devices.ts, src/lib/phrases.ts, api/device-id route.

**Inside WSL `avatar`** (user jetson, systemd on): code at /opt/voice-ai/{services,venvs,models,config,data}, studio at /home/jetson/avatar-studio. Units: voice-tts :8001 (Piper en_US-lessac-medium), voice-hotword :8002 (phrase_mode, custom_phrase "arijit", mic = WSLg pulse via PULSE_SERVER=unix:/mnt/wslg/PulseServer), voice-llm :8003 (qwen3:0.6b, keep_alive -1, **0.37s warm on GPU**), voice-stt :8004 (faster-whisper base CPU int8, ~1.5s), avatar-studio :3001, ollama :11434. All boot-enabled. Config /opt/voice-ai/config/audio.json.

**Device id `dev_4d8cf6bfdc83`** (MAC-derived via /api/device-id). Pairing QR verified rendering; claim from phone app pending.

## Gotchas (hard-won)
- **WSL kills the distro when the last wsl.exe client exits** → services die between commands. FIX: keeper `wsl -d avatar --exec sleep infinity` + Startup-folder `wsl-avatar-keeper.vbs` (no admin for schtasks). nohup does NOT survive session exit — use `systemd-run --unit=...` for detached procs.
- **ollama keep_alive must be a JSON number** (or "5m"); string "-1" → 400 from /api/generate.
- **piper-tts ≥1.3: use `voice.synthesize_wav(text, wav_file)`**; old synthesize(text, wf) leaves the wave file empty ("# channels not specified").
- **ollama GPU discovery times out on first boot in WSL** (watchdog "context deadline exceeded" → falls back CPU). `systemctl restart ollama` after CUDA is warm → CUDA0 detected; first model load on GPU ~65s (JIT), then fine. GTX 1650 = compute 7.5, model 100% GPU 693MB.
- **June src on May components TS breaks fixed**: Float32Array<ArrayBuffer> in display-screen VAD; ListeningOverlay needs {mode?,label?,userText?,replyText?}; PlaybackPlayer needs onReady (fire on onPlaying); pairing-screen line ~67 called async getOrCreateDeviceId() IN RENDER → React error 482 ("async Client Component") + page stuck on "Loading…" — must resolve into state.
- next dev binds localhost-only — use `-H 0.0.0.0`; WSL localhost relay flaky for fresh ports, retry or use 127.0.0.1.
- git-bash (Bash tool) mangles /mnt/e paths + $vars across wsl.exe; drive WSL via PowerShell tool + script files on /mnt/e.
- WSLg PulseAudio works for service mic capture (pactl 15.99, hotword listener opens "pulse" device fine).

## Cloud enrollment + assets (2026-06-11 later)
- **Device CLAIMED via API** (no phone needed): `POST /device/claim {user_id, device_id, device_name}` is UNAUTHENTICATED. dev_4d8cf6bfdc83 → user_d927e149 "Laptop WSL Replica", token `sync_918c9a7ae2436cc25ce902a5a2e22d26`. Full API spec in `E:\voice-ai-replica\avatar-studio\openapi.json`.
- **Voice-clone reference audio was NOT lost — it lives in S3** (`avatar_voice_link` in /sync/avatars): arijit.wav 132s, reference.wav 44s → saved to `E:\voice-ai-replica\voices\` + `/opt/voice-ai/data/voices/`. Usable for future F5/XTTS enrollment.
- **S3 bucket has NO CORS headers** → kiosk fetch()-based video cache fails from any origin. FIX: Next route `/api/media?url=` (same-origin streaming proxy, host-whitelisted) + `proxied()` wrapper in cache.ts. Avatar idle video verified playing on the laptop kiosk.

## 2026-06-12 — Sparklers studios vendored in; trained wake word + F5 clones LIVE
- **voice-hotword :8002 = sparklers-wakeword-studio** (Arijit1080 GitHub, cloned to /home/jetson/, also vendored in repo under vendor/) + `web/kiosk_compat.py` (kiosk /health, /hotword/mute via MUTED flag filtered in /events, /hotword/active persists /opt/voice-ai/config/wakeword.json + auto-arms on boot; untyped trigger lines added to /events for kiosk EventSource.onmessage). **"arijit" model TRAINED (AUC 1.000, models/arijit.joblib, ~4 min on x86)**, armed + listening on WSLg pulse mic, p50 4ms/chunk, real triggers observed.
- **voice-tts :8001 = sparklers-voiceclone-studio** + `web/kiosk_compat.py` (kiosk /synthesize {text,voice_id,use_clone,nfe_step} → F5 speak() w/ Piper fallback; /voice/default; startup AUTO-ENROLL of /opt/voice-ai/data/voices/*.wav = "F5 auto-trains"). Clones arijit+speaker enrolled @5.5s ref w/ real whisper transcripts; default voice = **arijit F5 clone**.
- **App /dashboard page** (avatar-studio): full studio UI ported in-app — service health, live score bars (SSE :8002/events typed), trigger feed, listen start/stop+threshold, train w/ progress, record-my-voice×5, model arm/delete, voice clone picker. /settings links to it.
- **GTX 1650 F5 reality: rtf ~2.9 @nfe4 (7.7s/reply), ~5.5 @nfe8 (15s)** — no tensor cores; default TTS_F5_NFE=4 (drop-in voice-tts.service.d/ref.conf with TTS_F5_REF_SEC=5.5). First-load ~10min (HF download+cuDNN autotune). GPU ~0.9-1.7GB during synth.
- **GOTCHAS:** PyPI f5-tts needs cached_path+soxr+pypinyin extras and `setuptools<81` (pkg_resources removed → librosa 0.9.1 breaks; error masked as transformers "Could not import module 'pipeline'"). FIRST auto-enroll ran while transformers was broken → silent FALLBACK ref_text (placeholder) = bad clones; re-enrolled after fix (delete via /api/voice/delete then restart). ollama installer needs zstd. sparklers /api/train/start = Form fields. tegrastats absence lands in status.last_error (filtered in compat /health).

## Not done / next
- Live spoken wake→reply loop end-to-end (detector fires; kiosk loop needs user voice test).
- F5 is slow on 1650 — options: nfe=4 (current), shorter refs, or Piper default for hot path.
- Whisper STT still CPU (1.5s) — CUDA ct2 possible later.
