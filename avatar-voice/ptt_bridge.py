#!/usr/bin/env python3
"""Push-to-talk web bridge for the avatar (port 8091, no GPU).
Serves a 'Hold to Talk' page and forwards press/release to the orchestrator's
Unix socket as ptt_start / ptt_stop. Also mirrors state/transcript/reply so the
page shows what you said and the avatar's reply.
Run:  python3 ptt_bridge.py   (needs the avatarvoice orchestrator running with AVATAR_PTT=1)
"""
import os, socket, threading, json, time
from flask import Flask, jsonify, Response

SOCK_PATH = (os.environ.get("XDG_RUNTIME_DIR") or "/run/user/1000") + "/avatar-voice.sock"
app = Flask(__name__)
state = {"state": "idle", "transcript": "", "reply": "", "connected": False,
         "hwscore": "0", "micgain": "", "miclevel": "0"}
_sock = None
_lock = threading.Lock()


def reader():
    global _sock
    while True:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(SOCK_PATH)
            _sock = s
            state["connected"] = True
            buf = b""
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    try:
                        m = json.loads(line.decode())
                    except Exception:
                        continue
                    t = m.get("type")
                    if t in ("state", "transcript", "reply", "hwscore", "micgain", "miclevel"):
                        state[t] = m.get("value", "")
        except Exception:
            pass
        _sock = None
        state["connected"] = False
        time.sleep(2)


def send(cmd):
    with _lock:
        if _sock:
            try:
                _sock.sendall((json.dumps(cmd) + "\n").encode())
                return True
            except Exception:
                return False
    return False


@app.post("/ptt/start")
def ptt_start():
    return ("", 204) if send({"type": "ptt_start"}) else ("orchestrator offline", 503)


@app.post("/ptt/stop")
def ptt_stop():
    return ("", 204) if send({"type": "ptt_stop"}) else ("orchestrator offline", 503)


@app.get("/status")
def status():
    return jsonify(state)


@app.get("/")
def index():
    return Response(PAGE, mimetype="text/html")


PAGE = """<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1,user-scalable=no">
<title>Avatar — Hold to Talk</title>
<style>
 html,body{height:100%;margin:0}
 body{font-family:system-ui,sans-serif;background:#0b0f14;color:#e6edf3;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:22px;padding:24px;box-sizing:border-box}
 h1{font-size:18px;color:#2dd4bf;margin:0}
 #btn{width:230px;height:230px;border-radius:50%;border:none;font-size:22px;font-weight:700;color:#02201d;background:#2dd4bf;cursor:pointer;user-select:none;-webkit-user-select:none;touch-action:none;box-shadow:0 0 0 0 rgba(45,212,191,.5);transition:transform .08s,box-shadow .2s}
 #btn.live{background:#f87171;color:#2a0a0a;transform:scale(1.06);box-shadow:0 0 0 18px rgba(248,113,113,.18)}
 .badge{font-size:14px;font-weight:700;letter-spacing:.5px;min-height:20px}
 .cap{max-width:520px;text-align:center;line-height:1.4}
 .you{color:#9fb3c8}.rep{color:#2dd4bf;font-size:18px;margin-top:6px}
 .hint{color:#5b6b7a;font-size:12px}.off{color:#f87171}
 .meters{width:100%;max-width:430px;display:flex;flex-direction:column;gap:11px;margin-top:8px}
 .meter{font-size:12px;color:#9fb3c8}
 .meter .lab{display:flex;justify-content:space-between;margin-bottom:3px}
 .meter .lab span:last-child{color:#2dd4bf;font-variant-numeric:tabular-nums}
 .bar{height:11px;background:#111824;border:1px solid #243040;border-radius:6px;overflow:hidden;position:relative}
 .fill{height:100%;width:0;background:#2dd4bf;transition:width .12s}
 .thr::after{content:'';position:absolute;top:-1px;left:50%;width:2px;height:13px;background:#f5c542}
</style></head><body>
<h1>🎙️ Avatar — Push to Talk</h1>
<button id=btn>HOLD<br>TO TALK</button>
<div class=badge id=badge>idle</div>
<div class=cap><div class=you id=you></div><div class=rep id=rep></div></div>
<div class=meters>
 <div class=meter><div class=lab><span>Wake-word conf — say "hey speaker" (fires at 0.50 ▮)</span><span id=hwval>0.00</span></div><div class="bar thr"><div id=hwbar class=fill></div></div></div>
 <div class=meter><div class=lab><span>Mic level &nbsp;·&nbsp; gain <b id=gain>—</b></span><span id=mlval>0.00</span></div><div class=bar><div id=mlbar class=fill></div></div></div>
</div>
<div class=hint id=hint>Hold the button (or hold <b>Space</b>) and speak. Release to send. Or say <b>"hey speaker"</b>.</div>
<script>
 const btn=document.getElementById('btn');let down=false;
 async function post(p){try{await fetch(p,{method:'POST'})}catch(e){}}
 function start(){if(down)return;down=true;btn.classList.add('live');btn.innerHTML='LISTENING…';post('/ptt/start');}
 function stop(){if(!down)return;down=false;btn.classList.remove('live');btn.innerHTML='HOLD<br>TO TALK';post('/ptt/stop');}
 btn.addEventListener('mousedown',start);btn.addEventListener('touchstart',e=>{e.preventDefault();start()},{passive:false});
 window.addEventListener('mouseup',stop);btn.addEventListener('touchend',e=>{e.preventDefault();stop()},{passive:false});
 document.addEventListener('keydown',e=>{if(e.code=='Space'&&!e.repeat){e.preventDefault();start();}});
 document.addEventListener('keyup',e=>{if(e.code=='Space'){e.preventDefault();stop();}});
 const cmap={idle:'#8b98a5',listening:'#2dd4bf',thinking:'#f5c542',speaking:'#34d399'};
 async function poll(){try{const s=await(await fetch('/status')).json();
   const b=document.getElementById('badge');
   b.textContent=s.connected?s.state.toUpperCase():'ORCHESTRATOR OFFLINE';
   b.style.color=s.connected?(cmap[s.state]||'#8b98a5'):'#f87171';
   document.getElementById('you').textContent=s.transcript?('You: '+s.transcript):'';
   document.getElementById('rep').textContent=s.reply||'';
   const hw=parseFloat(s.hwscore||0);
   document.getElementById('hwval').textContent=hw.toFixed(2);
   const hb=document.getElementById('hwbar');hb.style.width=Math.min(100,hw*100)+'%';
   hb.style.background=hw>=0.5?'#34d399':'#2dd4bf';
   document.getElementById('gain').textContent=s.micgain?parseFloat(s.micgain).toFixed(1)+'×':'—';
   const ml=parseFloat(s.miclevel||0);
   document.getElementById('mlval').textContent=ml.toFixed(2);
   document.getElementById('mlbar').style.width=Math.min(100,ml*400)+'%';
 }catch(e){}setTimeout(poll,400);}
 poll();
</script></body></html>"""

if __name__ == "__main__":
    threading.Thread(target=reader, daemon=True).start()
    app.run(host="0.0.0.0", port=8091, threaded=True)
