// Energy-based VAD with hangover. Feeds an utterance buffer to STT once speech ends.
// Clean interface so Silero-ONNX can replace the energy core later.
#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

class Vad {
public:
    struct Config {
        int sampleRate = 16000;
        int frameMs = 20;            // 320 samples @16k
        float startRms = 0.018f;     // speech-onset threshold (normalized)
        float endRms = 0.010f;       // below this counts as silence
        int hangoverMs = 600;        // silence to end an utterance
        int minSpeechMs = 250;       // discard blips
        int maxUtteranceMs = 12000;  // hard cap
    };

    // Called with a completed utterance (float32 mono 16k, [-1,1]).
    using OnUtterance = std::function<void(const std::vector<float> &)>;
    using OnStart = std::function<void()>;

    explicit Vad(Config c, OnUtterance cb, OnStart onStart = nullptr)
        : cfg_(c), cb_(std::move(cb)), onStart_(std::move(onStart)) {
        frame_ = cfg_.sampleRate * cfg_.frameMs / 1000;
    }

    int frameSamples() const { return frame_; }

    // Push one frame of int16 mono samples (size == frameSamples()).
    void push(const int16_t *s, int n) {
        float rms = 0.f;
        for (int i = 0; i < n; ++i) { float v = s[i] / 32768.f; rms += v * v; }
        rms = std::sqrt(rms / std::max(1, n));

        if (!inSpeech_) {
            if (rms >= cfg_.startRms) {
                inSpeech_ = true; silentMs_ = 0; buf_.clear(); append(s, n);
                if (onStart_) onStart_();
            }
        } else {
            append(s, n);
            silentMs_ = (rms < cfg_.endRms) ? silentMs_ + cfg_.frameMs : 0;
            int durMs = (int)(buf_.size() * 1000 / cfg_.sampleRate);
            if (silentMs_ >= cfg_.hangoverMs || durMs >= cfg_.maxUtteranceMs) {
                int speechMs = durMs - silentMs_;
                inSpeech_ = false;
                if (speechMs >= cfg_.minSpeechMs && cb_) cb_(buf_);
                buf_.clear();
            }
        }
    }

private:
    void append(const int16_t *s, int n) {
        for (int i = 0; i < n; ++i) buf_.push_back(s[i] / 32768.f);
    }
    Config cfg_;
    OnUtterance cb_;
    OnStart onStart_;
    int frame_;
    bool inSpeech_ = false;
    int silentMs_ = 0;
    std::vector<float> buf_;
};
