// Light ALSA mono 16 kHz capture + playback (no PulseAudio).
#pragma once
#include <alsa/asoundlib.h>
#include <cstdint>
#include <string>
#include <vector>

class AlsaCapture {
public:
    bool open(const char *dev = "default", unsigned rate = 16000) {
        rate_ = rate;
        if (snd_pcm_open(&h_, dev, SND_PCM_STREAM_CAPTURE, 0) < 0) return false;
        return cfg();
    }
    // Read `frames` mono samples; returns frames read (or <0 on error, with recover).
    int read(int16_t *buf, int frames) {
        int n = snd_pcm_readi(h_, buf, frames);
        if (n == -EPIPE) { snd_pcm_prepare(h_); return 0; }
        return n;
    }
    void close() { if (h_) { snd_pcm_close(h_); h_ = nullptr; } }
    ~AlsaCapture() { close(); }

private:
    bool cfg() {
        snd_pcm_hw_params_t *p;
        snd_pcm_hw_params_alloca(&p);
        snd_pcm_hw_params_any(h_, p);
        snd_pcm_hw_params_set_access(h_, p, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(h_, p, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(h_, p, 1);
        snd_pcm_hw_params_set_rate_near(h_, p, &rate_, 0);
        return snd_pcm_hw_params(h_, p) >= 0;
    }
    snd_pcm_t *h_ = nullptr;
    unsigned rate_ = 16000;
};

class AlsaPlayback {
public:
    bool open(const char *dev = "default", unsigned rate = 24000) {
        rate_ = rate;
        if (snd_pcm_open(&h_, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) return false;
        snd_pcm_set_params(h_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           1, rate_, 1, 200000);
        return true;
    }
    void play(const int16_t *buf, int frames) {
        int n = snd_pcm_writei(h_, buf, frames);
        if (n == -EPIPE) snd_pcm_prepare(h_);
        snd_pcm_drain(h_);
    }
    void close() { if (h_) { snd_pcm_close(h_); h_ = nullptr; } }
    ~AlsaPlayback() { close(); }

private:
    snd_pcm_t *h_ = nullptr;
    unsigned rate_ = 24000;
};
