#!/usr/bin/env python3
"""NeuTTS tuning web server for the Jetson — clone a reference voice and tune
generation params (temperature, top_k, top_p, repeat_penalty, max_tokens) live.
Loads NeuTTS once (full GPU) and serves a page reachable from the PC at :8090.

Run:  ~/neutts-venv/bin/python3 ~/avatar-voice/neutts_server.py
"""
import os, io, time, threading
os.environ.setdefault("HF_HUB_DISABLE_PROGRESS_BARS", "1")
os.environ.setdefault("NEU_BACKBONE_DEVICE", "gpu")
os.environ.setdefault("NEU_CODEC_DEVICE", "cuda")
import numpy as np
import soundfile as sf
from flask import Flask, request, send_file, jsonify, Response

REFS = {   # avatar -> (reference wav, reference text)
    "speaker": (os.path.expanduser("~/avatar-voice/ref/reference_api12.wav"),
                  "Kolkata, the capital city of West Bengal, has always been one of the most "
                  "politically active cities in India. In 2026, the political landscape of "
                  "Kolkata, witnessing a major transfer"),
}

app = Flask(__name__)
_lock = threading.Lock()
state = {"ready": False, "loading": "starting"}
tts = None
ref_codes_cache = {}


def load():
    global tts
    from neuttsair.neutts import NeuTTS
    state["loading"] = "loading NeuTTS (backbone=%s codec=%s)..." % (
        os.environ["NEU_BACKBONE_DEVICE"], os.environ["NEU_CODEC_DEVICE"])
    tts = NeuTTS(backbone_repo="neuphonic/neutts-air-q4-gguf",
                 backbone_device=os.environ["NEU_BACKBONE_DEVICE"],
                 codec_repo="neuphonic/neucodec",
                 codec_device=os.environ["NEU_CODEC_DEVICE"])
    state["loading"] = "encoding reference..."
    for name, (wav, _) in REFS.items():
        ref_codes_cache[name] = tts.encode_reference(wav)
    state["loading"] = "warming up..."
    tts.infer("Warming up.", ref_codes_cache["speaker"], REFS["speaker"][1])
    state["ready"] = True
    state["loading"] = "ready"
    print("[server] NeuTTS ready", flush=True)


def synth(text, voice, temperature, top_k, top_p, repeat_penalty, max_tokens):
    """Replicates NeuTTS _infer_ggml with tunable sampling params, then decodes."""
    ref_codes = ref_codes_cache[voice]
    ref_text = REFS[voice][1]
    ref_phones = tts._to_phones(ref_text)
    in_phones = tts._to_phones(text)
    codes_str = "".join(f"<|speech_{idx}|>" for idx in ref_codes)
    prompt = (f"user: Convert the text to speech:<|TEXT_PROMPT_START|>{ref_phones} {in_phones}"
              f"<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>{codes_str}")
    out = tts.backbone(prompt, max_tokens=max_tokens or tts.max_context,
                       temperature=temperature, top_k=top_k, top_p=top_p,
                       repeat_penalty=repeat_penalty, stop=["<|SPEECH_GENERATION_END|>"])
    wav = tts._decode(out["choices"][0]["text"])
    return np.asarray(wav, dtype=np.float32).reshape(-1)


@app.get("/health")
def health():
    return jsonify(state)


@app.post("/synth")
def do_synth():
    if not state["ready"]:
        return jsonify({"error": "model still loading: " + state["loading"]}), 503
    d = request.get_json(force=True)
    text = (d.get("text") or "").strip()
    if not text:
        return jsonify({"error": "empty text"}), 400
    voice = d.get("voice", "speaker")
    try:
        with _lock:
            t = time.time()
            wav = synth(text, voice,
                        float(d.get("temperature", 1.0)), int(d.get("top_k", 50)),
                        float(d.get("top_p", 1.0)), float(d.get("repeat_penalty", 1.0)),
                        int(d.get("max_tokens", 0)))
            dt = time.time() - t
    except Exception as e:
        return jsonify({"error": repr(e)}), 500
    buf = io.BytesIO(); sf.write(buf, wav, 24000, format="WAV"); buf.seek(0)
    resp = send_file(buf, mimetype="audio/wav")
    resp.headers["X-Synth-Seconds"] = f"{dt:.1f}"
    resp.headers["X-Audio-Seconds"] = f"{len(wav)/24000:.1f}"
    return resp


@app.get("/")
def index():
    return Response(PAGE, mimetype="text/html")


PAGE = """<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Reference Voice — NeuTTS Tuner</title>
<style>
 body{font-family:system-ui,sans-serif;background:#0b0f14;color:#e6edf3;margin:0;padding:24px;max-width:760px;margin:0 auto}
 h1{font-size:20px;color:#2dd4bf}.sub{color:#8b98a5;font-size:13px;margin-bottom:18px}
 textarea{width:100%;height:90px;background:#111824;color:#e6edf3;border:1px solid #243040;border-radius:8px;padding:10px;font-size:15px;box-sizing:border-box}
 .row{display:flex;align-items:center;gap:12px;margin:12px 0}
 .row label{width:140px;color:#9fb3c8;font-size:14px}
 .row input[type=range]{flex:1}.val{width:54px;text-align:right;color:#2dd4bf;font-variant-numeric:tabular-nums}
 button{background:#2dd4bf;color:#02201d;border:0;border-radius:8px;padding:12px 22px;font-size:16px;font-weight:600;cursor:pointer;margin-top:8px}
 button:disabled{opacity:.5;cursor:default}
 .status{margin:12px 0;color:#8b98a5;font-size:13px;min-height:18px}
 audio{width:100%;margin-top:14px}
 .grid{background:#0e141c;border:1px solid #1c2734;border-radius:10px;padding:14px 16px;margin-top:14px}
 a{color:#2dd4bf}
</style></head><body>
<h1>🎙️ Reference Voice — NeuTTS Tuner</h1>
<div class=sub>Type text, tune the generation params, hear the cloned voice. (NeuTTS is LLM-based — temperature/top-k/top-p control variation & quality; there are no diffusion "steps".)</div>
<textarea id=text placeholder="Type something for the voice to say...">Hello, my name is Sam. This is my cloned voice.</textarea>
<div class=grid>
 <div class=row><label>Temperature</label><input type=range id=temperature min=0.1 max=1.5 step=0.05 value=1.0><span class=val id=vtemperature>1.00</span></div>
 <div class=row><label>Top-K</label><input type=range id=top_k min=0 max=100 step=1 value=50><span class=val id=vtop_k>50</span></div>
 <div class=row><label>Top-P</label><input type=range id=top_p min=0.1 max=1.0 step=0.05 value=1.0><span class=val id=vtop_p>1.00</span></div>
 <div class=row><label>Repeat penalty</label><input type=range id=repeat_penalty min=1.0 max=1.5 step=0.02 value=1.0><span class=val id=vrepeat_penalty>1.00</span></div>
 <div class=row><label>Max tokens (0=auto)</label><input type=range id=max_tokens min=0 max=2048 step=64 value=0><span class=val id=vmax_tokens>0</span></div>
</div>
<button id=gen onclick=go()>Generate ▶</button>
<div class=status id=status></div>
<audio id=player controls></audio>
<div id=dl></div>
<script>
 const ids=['temperature','top_k','top_p','repeat_penalty','max_tokens'];
 ids.forEach(i=>{const el=document.getElementById(i),v=document.getElementById('v'+i);
   const f=()=>v.textContent=(i=='top_k'||i=='max_tokens')?el.value:parseFloat(el.value).toFixed(2);
   el.oninput=f;f();});
 async function poll(){try{const r=await(await fetch('/health')).json();
   if(!r.ready){document.getElementById('status').textContent='⏳ '+r.loading;setTimeout(poll,2000);}
   else document.getElementById('status').textContent='✅ model ready';}catch(e){setTimeout(poll,2000);}}
 poll();
 async function go(){
   const b=document.getElementById('gen'),s=document.getElementById('status');
   const body={text:document.getElementById('text').value,voice:'speaker'};
   ids.forEach(i=>body[i]=parseFloat(document.getElementById(i).value));
   b.disabled=true;s.textContent='🎛️ synthesizing...';
   const t0=Date.now();
   try{const r=await fetch('/synth',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
     if(!r.ok){const e=await r.json();s.textContent='❌ '+(e.error||r.status);b.disabled=false;return;}
     const blob=await r.blob(),url=URL.createObjectURL(blob);
     document.getElementById('player').src=url;document.getElementById('player').play();
     document.getElementById('dl').innerHTML='<a download="reference.wav" href="'+url+'">⬇ download wav</a>';
     s.textContent='✅ '+r.headers.get('X-Audio-Seconds')+'s audio in '+r.headers.get('X-Synth-Seconds')+'s (synth)';
   }catch(e){s.textContent='❌ '+e;}finally{b.disabled=false;}
 }
</script></body></html>"""

if __name__ == "__main__":
    threading.Thread(target=load, daemon=True).start()
    app.run(host="0.0.0.0", port=8090, threaded=True)
