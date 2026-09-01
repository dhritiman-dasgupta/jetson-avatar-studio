#!/usr/bin/env python3
"""F5-TTS-ONNX worker: clones the avatar voice (reference clip) to speak text.
Loads the 3 ONNX models (Transformer on CUDA), enrolls the reference audio+text once,
then synthesizes on demand. Protocol: stdin 'GEN <text>' -> synth -> play -> 'RPL done'.
Test:  python3 f5_worker.py "Hello, I am your avatar."   (writes/plays /tmp/f5_out.wav)"""
import os, re, sys, time, wave, audioop
import numpy as np
import onnxruntime
import jieba
from pypinyin import lazy_pinyin, Style

H = os.path.expanduser("~/avatar-voice/f5-onnx")
VOCAB = os.path.join(H, "vocab.txt")
M_A = os.path.join(H, "F5_Preprocess.onnx")
M_B = os.path.join(H, "F5_Transformer.onnx")
M_C = os.path.join(H, "F5_Decode.onnx")
# No reference audio ships with this repo -- supply your own clean ~10s mono clip.
REF_AUDIO = os.path.expanduser(os.environ.get("F5_REF_AUDIO", "~/avatar-voice/ref/ref10.wav"))
# ref_text MUST match the reference audio exactly or F5 output degrades (growly/robotic).
REF_TEXT = os.environ.get("F5_REF_TEXT", "")
SR, HOP = 24000, 256


def log(*a):
    print(*a, file=sys.stderr, flush=True)


provs = onnxruntime.get_available_providers()
use_cuda = "CUDAExecutionProvider" in provs
prov = ["CUDAExecutionProvider"] if use_cuda else ["CPUExecutionProvider"]
popts = [{"device_id": 0, "cudnn_conv_algo_search": "DEFAULT",
          "do_copy_in_default_stream": "1",
          "gpu_mem_limit": str(2600 * 1024 * 1024)}] if use_cuda else None
log("F5 providers:", provs, "-> using", prov)

so = onnxruntime.SessionOptions()
so.log_severity_level = 4
sA = onnxruntime.InferenceSession(M_A, sess_options=so, providers=["CPUExecutionProvider"])
sB = onnxruntime.InferenceSession(M_B, sess_options=so, providers=prov, provider_options=popts)
sC = onnxruntime.InferenceSession(M_C, sess_options=so, providers=["CPUExecutionProvider"])
NFE = int(os.environ.get("F5_NFE", "16"))   # 16 = good speed/quality balance
log("NFE =", NFE, "| transformer provider:", sB.get_providers()[0])

inA = [i.name for i in sA.get_inputs()];  outA = [o.name for o in sA.get_outputs()]
inB = [i.name for i in sB.get_inputs()];  outB = [o.name for o in sB.get_outputs()]
inC = [i.name for i in sC.get_inputs()];  outC = [o.name for o in sC.get_outputs()]

vocab = {}
with open(VOCAB, encoding="utf-8") as f:
    for i, ch in enumerate(f):
        vocab[ch[:-1]] = i


def convert_char_to_pinyin(text_list, polyphone=True):
    if jieba.dt.initialized is False:
        jieba.default_logger.setLevel(50)
        jieba.initialize()
    final = []
    ct = str.maketrans({";": ",", "“": '"', "”": '"', "‘": "'", "’": "'"})

    def is_zh(c):
        return "㄀" <= c <= "鿿"

    for text in text_list:
        cl = []
        text = text.translate(ct)
        for seg in jieba.cut(text):
            bl = len(bytes(seg, "UTF-8"))
            if bl == len(seg):
                if cl and bl > 1 and cl[-1] not in " :'\"":
                    cl.append(" ")
                cl.extend(seg)
            elif polyphone and bl == 3 * len(seg):
                sp = lazy_pinyin(seg, style=Style.TONE3, tone_sandhi=True)
                for i, c in enumerate(seg):
                    if is_zh(c):
                        cl.append(" ")
                    cl.append(sp[i])
            else:
                for c in seg:
                    if ord(c) < 256:
                        cl.extend(c)
                    elif is_zh(c):
                        cl.append(" ")
                        cl.extend(lazy_pinyin(c, style=Style.TONE3, tone_sandhi=True))
                    else:
                        cl.append(c)
        final.append(cl)
    return final


def load_ref(path):
    w = wave.open(path, "rb"); fr = w.getframerate()
    d = w.readframes(w.getnframes()); w.close()
    if fr != SR:
        d, _ = audioop.ratecv(d, 2, 1, fr, SR, None)
    a = np.frombuffer(d, dtype=np.int16).astype(np.float32) / 32768.0
    return a.astype(np.float16)   # this ONNX export expects fp16 audio input


refaudio = load_ref(REF_AUDIO)
audio_len = len(refaudio)
refaudio = refaudio.reshape(1, 1, -1)
ZH = r"。，、；：？！"


def synth(gen_text):
    rtl = len(REF_TEXT.encode()) + 3 * len(re.findall(ZH, REF_TEXT))
    gtl = len(gen_text.encode()) + 3 * len(re.findall(ZH, gen_text))
    ral = audio_len // HOP + 1
    max_dur = np.array(ral + int(ral / rtl * gtl), dtype=np.int64)      # scalar
    text = convert_char_to_pinyin([REF_TEXT + " " + gen_text])
    ids = np.array([[vocab.get(c, 0) for c in text[0]]], dtype=np.int32)  # [1, N]
    # preprocess -> 7 outputs
    noise, rope_cos, rope_sin, cat_mel, cat_mel_drop, qk_empty, ref_sig_len = sA.run(
        outA, {inA[0]: refaudio, inA[1]: ids, inA[2]: max_dur})
    # NFE denoising loop on the transformer (time_step scalar)
    for step in range(NFE):
        ts = np.array(step, dtype=np.int32)
        noise = sB.run(outB, {inB[0]: noise, inB[1]: rope_cos, inB[2]: rope_sin,
                              inB[3]: cat_mel, inB[4]: cat_mel_drop, inB[5]: qk_empty,
                              inB[6]: ts})[0]
    sig = sC.run(outC, {inC[0]: noise, inC[1]: ref_sig_len})[0]
    return sig.reshape(-1).astype(np.float32)


def write_play(sig):
    import soundfile as sf
    sf.write("/tmp/f5_out.wav", sig, SR)
    os.system("aplay -q -D plughw:0,0 /tmp/f5_out.wav >/dev/null 2>&1")


if len(sys.argv) > 1 and sys.argv[1] != "--worker":
    txt = " ".join(sys.argv[1:])
    log("warming up...")
    synth("warm up.")                       # first run compiles CUDA kernels
    t = time.time(); sig = synth(txt); dt = time.time() - t
    write_play(sig)
    log(f"synth {len(sig)/SR:.1f}s audio in {dt:.1f}s")
    print("DONE", flush=True)
else:
    synth("warm up.")
    print("READY", flush=True)
    for line in sys.stdin:
        line = line.strip()
        if line.startswith("GEN "):
            write_play(synth(line[4:]))
            print("RPL done", flush=True)
        elif line == "QUIT":
            break
