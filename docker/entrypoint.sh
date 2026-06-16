#!/bin/bash
# Start the orchestrator + push-to-talk bridge inside the container.
set -e
mkdir -p "$XDG_RUNTIME_DIR"
# push-to-talk web bridge (port 8091)
/root/neutts-venv/bin/python3 /opt/avatar/avatar-voice/ptt_bridge.py &
# voice orchestrator (mic -> STT -> LLM -> NeuTTS)
exec /opt/avatar/avatar-voice/avatar-voice
