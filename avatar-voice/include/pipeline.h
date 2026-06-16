// Voice pipeline: VAD utterance -> STT(worker) -> LLM(worker) -> TTS -> idle.
// STT/LLM run as separate processes managed by the ModelManager (offload/onload).
// TTS is a placeholder until the F5-TTS ONNX worker is wired.
#pragma once
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "model_manager.h"
#include "subprocess.h"

class Pipeline {
public:
    using Report = std::function<void(const std::string &kind, const std::string &value)>;
    using Notify = std::function<void()>;

    Pipeline(ModelManager &mm, Subprocess &stt, Subprocess &llm, Subprocess &f5, Report rep)
        : mm_(mm), stt_(stt), llm_(llm), f5_(f5), rep_(std::move(rep)) {}

    void start() {
        running_ = true;
        worker_ = std::thread([this] { loop(); });
        watchdog_ = std::thread([this] { watchdog(); });
    }
    void stop() {
        running_ = false; cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (watchdog_.joinable()) watchdog_.join();
    }

    // Hotword gating: when gated, speech is ignored until a wake word arms a window.
    void setGated(bool g) { gated_ = g; }
    void setOnWake(Notify n) { onWake_ = std::move(n); }

    // Open a listening window (push-to-talk uses this directly; no wake chime).
    void arm() {
        armWindow(wakeWindowMs_);
        if (!busy_) setState("listening");
    }

    // Called by the hotword reader thread when the active avatar's wake word fires.
    void onWake() {
        arm();
        if (onWake_) onWake_();          // wake chime
    }

    void onSpeechStart() { if (!busy_ && armedNow()) setState("listening"); }

    void onUtterance(std::vector<float> pcm) {
        if (!armedNow()) return;         // locked: ignore until the wake word
        { std::lock_guard<std::mutex> lk(m_); if (busy_ || q_.size() > 2) return; q_.push_back(std::move(pcm)); }
        cv_.notify_one();
    }

    void setState(const std::string &s) { state_ = s; rep_("state", s); }

private:
    void loop() {
        while (running_) {
            std::vector<float> pcm;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return !running_ || !q_.empty(); });
                if (!running_) return;
                pcm = std::move(q_.front()); q_.pop_front(); busy_ = true;
            }
            process(pcm);
            busy_ = false;
            if (gated_) { armWindow(followupMs_); setState("listening"); }  // hold for follow-ups
            else setState("idle");
        }
    }

    // Relock to idle when a gated listening window expires with no speech.
    void watchdog() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (gated_ && armed_ && !busy_ && nowMs() >= armedUntil_) {
                armed_ = false;
                setState("idle");
            }
        }
    }

    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void armWindow(long long ms) { armedUntil_ = nowMs() + ms; armed_ = true; }
    bool armedNow() const { return !gated_ || (armed_ && nowMs() < armedUntil_); }

    void process(std::vector<float> &pcm) {
        setState("listening");
        if (!mm_.acquire("stt")) return;
        const std::string path = "/tmp/avatar_utt.f32";
        if (FILE *f = fopen(path.c_str(), "wb")) {
            fwrite(pcm.data(), sizeof(float), pcm.size(), f); fclose(f);
        }
        stt_.writeLine("STT " + path);
        std::string r = stt_.readLine();
        std::string text = (r.rfind("TXT ", 0) == 0) ? r.substr(4) : "";
        if (isJunk(text)) { setState("idle"); return; }   // drop whisper noise/hallucinations
        rep_("transcript", text);

        setState("thinking");
        if (!mm_.acquire("llm")) return;          // arbiter evicts as needed
        llm_.writeLine("GEN " + text);
        std::string rr = llm_.readLine();
        std::string reply = (rr.rfind("RPL ", 0) == 0) ? rr.substr(4) : "";
        rep_("reply", reply);

        setState("speaking");
        if (mm_.acquire("tts") && !reply.empty()) {
            f5_.writeLine("GEN " + reply);         // F5 clones the avatar voice + plays it
            f5_.readLine();                        // blocks until "RPL done" (synth + playback complete)
        }
    }

    // Reject whisper non-speech markers + common silence hallucinations.
    static bool isJunk(const std::string &s) {
        size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos) return true;
        if (s[b] == '(' || s[b] == '[' || s[b] == '*' || s[b] == '{') return true;
        std::string t;
        for (char c : s) t += (char)std::tolower((unsigned char)c);
        size_t lb = t.find_first_not_of(" \t.,!?"), le = t.find_last_not_of(" \t.,!?");
        if (lb == std::string::npos) return true;
        t = t.substr(lb, le - lb + 1);
        if (t.size() < 2) return true;
        static const char *junk[] = {
            "you", "thank you", "thanks for watching", "thanks", "bye", "the", "uh", "um",
            "yeah", "okay", "ok", "so", "and", "hmm", "mm", "ah", "oh", "i", "a", "to"};
        for (auto j : junk) if (t == j) return true;
        return false;
    }

    ModelManager &mm_;
    Subprocess &stt_, &llm_, &f5_;
    Report rep_;
    Notify onWake_;
    std::thread worker_, watchdog_;
    std::atomic<bool> running_{false}, busy_{false};
    std::atomic<bool> gated_{false}, armed_{false};
    std::atomic<long long> armedUntil_{0};
    long long wakeWindowMs_ = 10000;   // listen for ~10s after the wake word
    long long followupMs_ = 8000;      // keep listening ~8s after each reply
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::vector<float>> q_;
    std::string state_ = "idle";
};
