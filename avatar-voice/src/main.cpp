// AI Avatar voice orchestrator (C++): VAD -> STT(worker) -> LLM(worker) -> TTS.
// Light: orchestrator links only ALSA + pthread; heavy models live in spawned workers
// that the ModelManager loads (spawn) / unloads (kill) for automatic GPU offload.
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json_min.h"
#include "model_manager.h"
#include "ipc_server.h"
#include "subprocess.h"
#include "audio_alsa.h"
#include "vad.h"
#include "pipeline.h"

static std::atomic<bool> g_run{true};
static void onSig(int) { g_run = false; }

static std::string home() { const char *h = std::getenv("HOME"); return h ? h : "/home/jetson"; }
static std::string sockPath() {
    const char *xdg = std::getenv("XDG_RUNTIME_DIR");
    return (xdg && *xdg ? std::string(xdg) : "/tmp") + "/avatar-voice.sock";
}

// 8GB Orin: keep all resident. Measured all-resident (~8s/turn) BEATS evict (~12s) —
// the STT/LLM reload costs ~4s, more than the mild swap pressure. No per-turn reload.
static constexpr size_t VRAM_BUDGET_MB = 6000;

// List trained wake-word models in ~/avatar-voice/hotwords/ (returns onnx paths + stems).
static std::vector<std::pair<std::string, std::string>> listHotwords(const std::string &dir) {
    std::vector<std::pair<std::string, std::string>> out;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 5 && n.substr(n.size() - 5) == ".onnx")
                out.push_back({dir + "/" + n, n.substr(0, n.size() - 5)});
        }
        closedir(d);
    }
    return out;
}

int main() {
    std::signal(SIGINT, onSig);
    std::signal(SIGTERM, onSig);
    std::signal(SIGPIPE, SIG_IGN);

    const std::string STT_WORKER = home() + "/avatar-voice/stt-worker";
    const std::string LLM_WORKER = home() + "/avatar-voice/llm-worker";
    const std::string WHISPER_MODEL = home() + "/voice-engines/whisper.cpp/models/ggml-tiny.en.bin";  // faster than base
    const std::string LLM_MODEL = home() + "/voice-engines/models/qwen2.5-0.5b-instruct-q4_k_m.gguf";
    const std::string F5_WORKER = home() + "/avatar-voice/f5_worker.py";
    const std::string NEUTTS_WORKER = home() + "/avatar-voice/neutts_worker.py";
    const std::string NEUTTS_PY = home() + "/neutts-venv/bin/python3";   // isolated full-GPU venv
    const std::string HOTWORD_WORKER = home() + "/avatar-voice/hotword_worker.py";
    const std::string HOTWORD_DIR = home() + "/avatar-voice/hotwords";

    IpcServer ipc(sockPath());
    Subprocess sttProc, llmProc, f5Proc, hwProc;

    ModelManager mm(VRAM_BUDGET_MB, [&](const std::string &n, const std::string &s) {
        ipc.broadcast(jsonmin::obj({{"type", "model"}, {"name", n}, {"state", s}}));
    });

    auto sttM = std::make_shared<Model>();
    sttM->name = "stt"; sttM->usesGpu = true; sttM->vramMB = 400; sttM->pinned = true;  // tiny.en, resident
    sttM->doLoad = [&] { if (!sttProc.start({STT_WORKER, WHISPER_MODEL})) return false; return sttProc.readLine() == "READY"; };
    sttM->doUnload = [&] { sttProc.kill(); };

    auto llmM = std::make_shared<Model>();
    llmM->name = "llm"; llmM->usesGpu = true; llmM->vramMB = 800; llmM->pinned = true;  // resident
    llmM->doLoad = [&] { if (!llmProc.start({LLM_WORKER, LLM_MODEL})) return false; return llmProc.readLine() == "READY"; };
    llmM->doUnload = [&] { llmProc.kill(); };

    auto ttsM = std::make_shared<Model>();   // NeuTTS Air worker (cloned voice, full-GPU)
    // PINNED: ~90s load+warm is too slow to load/unload per-utterance; stays resident.
    ttsM->name = "tts"; ttsM->usesGpu = true; ttsM->vramMB = 1500; ttsM->pinned = true;  // codec on CPU
    ttsM->doLoad = [&] { if (!f5Proc.start({NEUTTS_PY, NEUTTS_WORKER, "--worker"})) return false; return f5Proc.readLine() == "READY"; };
    ttsM->doUnload = [&] { f5Proc.kill(); };

    mm.registerModel(sttM);
    mm.registerModel(llmM);
    mm.registerModel(ttsM);
    mm.start();

    Pipeline pipe(mm, sttProc, llmProc, f5Proc, [&](const std::string &kind, const std::string &value) {
        ipc.broadcast(jsonmin::obj({{"type", kind}, {"value", value}}));
    });
    pipe.start();

    // Pre-warm the pinned NeuTTS worker at startup (~90s load+warm) so the first
    // reply isn't slow. acquire() holds the MM mutex during load -> the box is
    // "busy" for ~90s after boot, then fully ready; subsequent replies are fast.
    std::thread([&] {   // pre-warm all (pinned) so there are no per-turn reloads
        std::cerr << "[warm] loading STT+LLM...\n"; mm.acquire("stt"); mm.acquire("llm");
        std::cerr << "[warm] pre-warming NeuTTS...\n"; bool ok = mm.acquire("tts");
        std::cerr << (ok ? "[warm] all models ready\n" : "[warm] TTS LOAD FAILED\n"); }).detach();

    const char *tingEnv = std::getenv("AVATAR_TING");   // set to "0" to disable
    const bool ting = !(tingEnv && std::string(tingEnv) == "0");
    auto tingPlay = [ting] {
        if (ting)
            std::system("aplay -q -D plughw:0,0 /home/jetson/avatar-voice/ting.wav >/dev/null 2>&1 &");
    };

    // Push-to-talk (always available, coexists with hotword): mic is buffered only
    // between ptt_start/ptt_stop IPC commands; otherwise hotword+gated-VAD run.
    std::mutex pttMtx;
    std::vector<float> pttBuf;
    std::atomic<bool> pttActive{false};

    // Hotword: load every trained wake word; gate the pipeline on the ACTIVE avatar's.
    // No models present -> always-on VAD (graceful fallback until training lands).
    std::mutex awMtx;
    std::string activeWake;             // "" = accept any trained wake word
    auto models = listHotwords(HOTWORD_DIR);
    std::thread hwReader;
    bool gated = false;
    if (!models.empty()) {
        // isolated venv python (openwakeword + numpy<2 + CPU onnxruntime) keeps the
        // hotword stack from clobbering F5's onnxruntime-gpu in the --user site.
        const std::string HW_PY = home() + "/avatar-voice/hw-venv/bin/python3";
        std::vector<std::string> argv = {HW_PY, HOTWORD_WORKER};
        std::string names;
        for (auto &m : models) { argv.push_back(m.first); names += m.second + " "; }
        if (hwProc.start(argv) && hwProc.readLine() == "READY") {
            gated = true;
            std::cerr << "hotword armed: " << names << "(gated; say a wake word)\n";
            pipe.setGated(true);
            pipe.setOnWake(tingPlay);   // chime on wake, not on every VAD onset
            hwReader = std::thread([&] {
                while (g_run && hwProc.alive()) {
                    std::string line = hwProc.readLine();
                    if (line.empty()) break;
                    if (line.rfind("SCORE ", 0) == 0) {   // live wake-word confidence
                        ipc.broadcast(jsonmin::obj({{"type", "hwscore"}, {"value", line.substr(6)}}));
                        continue;
                    }
                    if (line.rfind("WAKE ", 0) != 0) continue;
                    std::string rest = line.substr(5);
                    size_t sp = rest.find(' ');
                    std::string key = sp == std::string::npos ? rest : rest.substr(0, sp);
                    std::string want; { std::lock_guard<std::mutex> lk(awMtx); want = activeWake; }
                    if (want.empty() || key == want) {
                        std::cerr << "[hotword] WAKE " << key << "\n";
                        pipe.onWake();
                    }
                }
            });
        } else {
            std::cerr << "WARN: hotword worker failed to start - falling back to always-on VAD\n";
        }
    } else {
        std::cerr << "no hotword models in " << HOTWORD_DIR << " - always-on VAD\n";
    }

    Vad::Config vcfg;
    vcfg.startRms = 0.030f;     // onset (post-gain) — above ambient noise
    vcfg.endRms = 0.018f;
    vcfg.hangoverMs = 700;      // capture full sentences across short pauses
    vcfg.minSpeechMs = 350;     // discard brief noise blips
    Vad vad(vcfg,
            [&](const std::vector<float> &pcm) { pipe.onUtterance(pcm); },
            [&] {   // speech onset: chime here only when NOT gated (else chime is on wake)
                if (!gated) tingPlay();
                pipe.onSpeechStart();
            });

    ipc.onLine([&](const std::string &line) {
        auto m = jsonmin::parseFlat(line);
        const std::string &t = m["type"];
        if (t == "set_avatar") {
            std::cerr << "[avatar] " << m["name"] << " voice=" << m["voice_url"] << "\n";
            // voice_url -> reference audio for the F5-TTS clone (used by the TTS worker later)
            { std::lock_guard<std::mutex> lk(awMtx); activeWake = m["name"]; }  // gate on this avatar's wake word
            ipc.broadcast(jsonmin::obj({{"type", "info"}, {"value", "avatar set: " + m["name"]}}));
        } else if (t == "ptt_start") {       // hold-to-talk pressed: start buffering mic
            { std::lock_guard<std::mutex> lk(pttMtx); pttBuf.clear(); }
            pttActive = true;
            tingPlay();
            pipe.arm();                       // open the listening window (bypasses hotword gate)
        } else if (t == "ptt_stop") {        // released: transcribe what was captured
            pttActive = false;
            std::vector<float> u;
            { std::lock_guard<std::mutex> lk(pttMtx); u.swap(pttBuf); }
            if (u.size() >= 1600) { pipe.arm(); pipe.onUtterance(std::move(u)); }  // ignore <0.1s
            else pipe.setState("idle");
        } else if (t == "ping") {
            ipc.broadcast(jsonmin::obj({{"type", "pong"}, {"value", ""}}));
        }
    });

    if (!ipc.start()) { std::cerr << "IPC bind failed at " << sockPath() << "\n"; return 1; }
    std::cerr << "avatar-voice up. socket=" << sockPath() << "\n";
    pipe.setState("idle");

    // mic capture -> VAD (USB PnP mic = card 0; override with AVATAR_MIC)
    const char *micEnv = std::getenv("AVATAR_MIC");
    const std::string micDev = micEnv && *micEnv ? micEnv : "plughw:0,0";
    AlsaCapture cap;
    std::thread capThread;
    const char *gainEnv = std::getenv("AVATAR_MIC_GAIN");
    const float micGain = gainEnv && *gainEnv ? (float)atof(gainEnv) : 4.0f;
    if (cap.open(micDev.c_str(), 16000)) {
        std::cerr << "mic open ok (" << micDev << ") gain=" << micGain << "\n";
        ipc.broadcast(jsonmin::obj({{"type", "micgain"}, {"value", std::to_string(micGain)}}));
        capThread = std::thread([&] {
            std::vector<int16_t> buf(vad.frameSamples());
            int lvlCtr = 0;
            while (g_run) {
                int n = cap.read(buf.data(), vad.frameSamples());
                if (n > 0) {
                    for (int i = 0; i < n; ++i) {        // boost the quiet USB mic
                        int v = (int)(buf[i] * micGain);
                        buf[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
                    }
                    if (pttActive) {                     // push-to-talk held: buffer only
                        std::lock_guard<std::mutex> lk(pttMtx);
                        for (int i = 0; i < n; ++i) pttBuf.push_back(buf[i] / 32768.f);
                    } else {                             // else: hotword detection + (gated) VAD
                        vad.push(buf.data(), n);
                        if (gated) hwProc.writeRaw(buf.data(), (size_t)n * sizeof(int16_t));
                    }
                    if (++lvlCtr >= 10) {                // ~5/s post-gain mic level for dashboard
                        lvlCtr = 0;
                        double s = 0; for (int i = 0; i < n; ++i) { double v = buf[i] / 32768.0; s += v * v; }
                        ipc.broadcast(jsonmin::obj({{"type", "miclevel"},
                            {"value", std::to_string(std::sqrt(s / std::max(1, n)))}}));
                    }
                }
            }
        });
    } else {
        std::cerr << "WARN: no mic ('default' capture failed) - pipeline idle until audio device present\n";
    }

    while (g_run) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (capThread.joinable()) capThread.join();
    hwProc.kill();
    if (hwReader.joinable()) hwReader.join();
    pipe.stop();
    mm.stop();
    ipc.stop();
    sttProc.kill();
    llmProc.kill();
    f5Proc.kill();
    std::cerr << "avatar-voice stopped.\n";
    return 0;
}
