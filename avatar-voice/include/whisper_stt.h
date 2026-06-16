// whisper.cpp STT wrapper with explicit load/unload for the ModelManager.
#pragma once
#include <string>
#include <vector>
#include "whisper.h"

class WhisperStt {
public:
    explicit WhisperStt(std::string modelPath, int threads = 4)
        : model_(std::move(modelPath)), threads_(threads) {}

    bool load() {
        if (ctx_) return true;
        whisper_context_params p = whisper_context_default_params();
        p.use_gpu = true;
        ctx_ = whisper_init_from_file_with_params(model_.c_str(), p);
        return ctx_ != nullptr;
    }
    void unload() {
        if (ctx_) { whisper_free(ctx_); ctx_ = nullptr; }
    }
    bool ready() const { return ctx_ != nullptr; }

    std::string transcribe(const std::vector<float> &pcm) {
        if (!ctx_) return "";
        whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wp.language = "en";
        wp.n_threads = threads_;
        wp.print_progress = false;
        wp.print_realtime = false;
        wp.print_special = false;
        wp.no_timestamps = true;
        wp.single_segment = false;
        if (whisper_full(ctx_, wp, pcm.data(), (int)pcm.size()) != 0) return "";
        std::string out;
        int n = whisper_full_n_segments(ctx_);
        for (int i = 0; i < n; ++i) out += whisper_full_get_segment_text(ctx_, i);
        // trim leading space whisper adds
        size_t b = out.find_first_not_of(" \t\n");
        return b == std::string::npos ? "" : out.substr(b);
    }

    ~WhisperStt() { unload(); }

private:
    std::string model_;
    int threads_;
    whisper_context *ctx_ = nullptr;
};
