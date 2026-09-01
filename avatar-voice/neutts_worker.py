#!/usr/bin/env python3
"""NeuTTS Air worker: clones the avatar voice and speaks LLM replies.
Drop-in replacement for f5_worker.py — same orchestrator protocol:
  stdin "GEN <text>" -> synth -> aplay -> stdout "RPL done"
  stdin "QUIT"       -> exit
Runs full-GPU (backbone + neucodec on CUDA). Loads once, warms up, then prints READY.
Reference = a voice-clone clip you supply (NEU_REF_AUDIO + NEU_REF_TEXT). No reference
audio ships with this repo; both variables are required. Test:  neutts_worker.py "Hello, I am your avatar." """
import os, sys, time, subprocess
import numpy as np
os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "1")
os.environ.setdefault("TRANSFORMERS_NO_ADVISORY_WARNINGS", "1")
# Keep STDOUT clean for the orchestrator protocol (READY / RPL done) — route all
# library/load noise to STDERR until we're ready, else doLoad misreads the first line.
_PROTO = sys.stdout
sys.stdout = sys.stderr
import soundfile as sf

def log(*a): print(*a, file=sys.stderr, flush=True)
def emit(s): print(s, file=_PROTO, flush=True)   # protocol channel (real stdout)

BB_DEV = os.environ.get("NEU_BACKBONE_DEVICE", "gpu")
CO_DEV = os.environ.get("NEU_CODEC_DEVICE", "cuda")
# Voice-clone reference. NO reference audio ships with this repo -- supply your own.
# NEU_REF_TEXT must be an accurate transcript of NEU_REF_AUDIO, or cloning degrades badly.
REF_AUDIO = os.path.expanduser(os.environ.get("NEU_REF_AUDIO", "~/avatar-voice/ref/reference.wav"))
REF_TEXT = os.environ.get("NEU_REF_TEXT", "")
OUT = "/tmp/neutts_out.wav"
SPK = os.environ.get("AVATAR_SPK", "plughw:0,0")

from neuttsair.neutts import NeuTTS

BB_REPO = os.environ.get("NEU_BACKBONE_REPO", "neuphonic/neutts-air-q8-gguf")  # q8 > q4 quality
log(f"NeuTTS loading: repo={BB_REPO} backbone={BB_DEV} codec={CO_DEV}")
tts = NeuTTS(backbone_repo=BB_REPO, backbone_device=BB_DEV,
             codec_repo="neuphonic/neucodec", codec_device=CO_DEV)
# bigger streaming chunk = smoother playback, fewer boundary artifacts (default 25 ~0.5s)
CHUNK = int(os.environ.get("NEU_STREAM_CHUNK", "150"))
tts.streaming_frames_per_chunk = CHUNK
tts.streaming_stride_samples = CHUNK * tts.hop_length
log(f"stream chunk = {CHUNK} frames (~{CHUNK*tts.hop_length/24000:.2f}s)")
if not os.path.isfile(REF_AUDIO) or not REF_TEXT.strip():
    log(f"FATAL: need a voice-clone reference. No sample ships with this repo.\n"
        f"  set NEU_REF_AUDIO to a clean ~10s mono wav of the voice to clone "
        f"(currently: {REF_AUDIO}, exists={os.path.isfile(REF_AUDIO)})\n"
        f"  set NEU_REF_TEXT to its exact transcript "
        f"(currently: {'set' if REF_TEXT.strip() else 'EMPTY'})")
    sys.exit(2)
ref_codes = tts.encode_reference(REF_AUDIO)
log("encoded reference, warming up...")
_ = tts.infer("Warming up.", ref_codes, REF_TEXT)   # compiles CUDA kernels (one-time)


def speak(text):
    # STREAMING: pipe each generated chunk straight to aplay (raw 24k PCM) so
    # playback starts within ~1s instead of waiting for the whole synth.
    p = subprocess.Popen(["aplay", "-q", "-D", SPK, "-f", "S16_LE", "-r", "24000",
                          "-c", "1", "-t", "raw"], stdin=subprocess.PIPE)
    chunks = []
    try:
        for ch in tts.infer_stream(text, ref_codes, REF_TEXT):
            a = np.clip(np.asarray(ch, dtype=np.float32).reshape(-1), -1.0, 1.0)
            chunks.append(a)
            try:
                p.stdin.write((a * 32767.0).astype("<i2").tobytes())
            except BrokenPipeError:
                break
    finally:
        try: p.stdin.close()
        except Exception: pass
        p.wait()
    if chunks:
        sf.write(OUT, np.concatenate(chunks), 24000)   # keep a file copy for fetching


if len(sys.argv) > 1 and sys.argv[1] != "--worker":
    t = time.time(); speak(" ".join(sys.argv[1:]))
    log(f"spoke in {time.time()-t:.1f}s")
    emit("DONE")
else:
    emit("READY")
    for line in sys.stdin:
        line = line.strip()
        if line.startswith("GEN "):
            speak(line[4:])
            emit("RPL done")
        elif line == "QUIT":
            break
