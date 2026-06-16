#!/usr/bin/env python3
"""Wake-word detector worker (openWakeWord ONNX) for the avatar orchestrator.

The C++ orchestrator owns the mic, so it TEES raw 16 kHz mono int16 PCM to this
worker's stdin. We buffer to 1280-sample (80 ms) chunks, run openWakeWord, and
print a line on stdout when the active avatar's wake word fires.

Usage:  hotword_worker.py <model1.onnx> [model2.onnx ...]
Protocol:  stdin  = raw int16 LE audio (binary, continuous)
           stdout = "READY" once loaded, then "WAKE <key> <score>" per detection
           (<key> = the onnx file stem, e.g. speaker). The orchestrator filters
           by the active avatar, so all avatars' wake words load at once here.
CPU-only (CUDA_VISIBLE_DEVICES forced empty) so it never contends with F5/STT/LLM
on the GPU. The hotword is meant to be always-resident / pinned."""
import os
os.environ["CUDA_VISIBLE_DEVICES"] = ""        # keep openWakeWord off the GPU
import sys, time
import numpy as np


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def main():
    if len(sys.argv) < 2:
        log("hotword: no model paths given"); sys.exit(2)
    paths = sys.argv[1:]
    thresh = float(os.environ.get("HOTWORD_THRESHOLD", "0.5"))
    refractory = float(os.environ.get("HOTWORD_REFRACTORY", "2.0"))  # min s between fires per key

    from openwakeword.model import Model
    import openwakeword.utils as oww_utils
    # ensure the two shared models (melspectrogram + embedding) are present
    try:
        oww_utils.download_models()
    except Exception as e:
        log("hotword: download_models warn:", e)

    oww = Model(wakeword_models=paths, inference_framework="onnx")
    keys = list(oww.models.keys())
    log("hotword: loaded", keys, "thresh", thresh)
    print("READY", flush=True)

    BYTES = 1280 * 2                  # 80 ms @ 16 kHz, int16
    buf = b""
    last_fire = {k: 0.0 for k in keys}
    last_score = 0.0
    stdin = sys.stdin.buffer
    while True:
        b = stdin.read(BYTES - len(buf))
        if not b:
            break
        buf += b
        if len(buf) < BYTES:
            continue
        pcm = np.frombuffer(buf[:BYTES], dtype=np.int16)
        buf = buf[BYTES:]
        try:
            scores = oww.predict(pcm)
        except Exception as e:
            log("hotword: predict err:", e); continue
        now = time.monotonic()
        mx = max(scores.values()) if scores else 0.0
        if now - last_score >= 0.15:           # live confidence for the dashboard
            last_score = now
            print(f"SCORE {mx:.3f}", flush=True)
        for k, sc in scores.items():
            if sc >= thresh and (now - last_fire.get(k, 0.0)) >= refractory:
                last_fire[k] = now
                print(f"WAKE {k} {sc:.3f}", flush=True)


if __name__ == "__main__":
    main()
