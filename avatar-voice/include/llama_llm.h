// llama.cpp LLM wrapper (current API) with load/unload for the ModelManager.
// Single-turn ChatML prompts (Qwen2.5), short replies for the avatar.
#pragma once
#include <string>
#include <vector>
#include "llama.h"

class LlamaLlm {
public:
    LlamaLlm(std::string modelPath, std::string sys, int nCtx = 2048, int nPredict = 80)
        : model_(std::move(modelPath)), sys_(std::move(sys)), nCtx_(nCtx), nPredict_(nPredict) {}

    bool load() {
        if (ctx_) return true;
        llama_backend_init();
        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = 99;                       // offload all layers to the Orin GPU
        lm_ = llama_model_load_from_file(model_.c_str(), mp);
        if (!lm_) return false;
        llama_context_params cp = llama_context_default_params();
        cp.n_ctx = nCtx_;
        cp.n_batch = nCtx_;
        ctx_ = llama_init_from_model(lm_, cp);
        if (!ctx_) { llama_model_free(lm_); lm_ = nullptr; return false; }
        vocab_ = llama_model_get_vocab(lm_);
        llama_sampler_chain_params sp = llama_sampler_chain_default_params();
        smpl_ = llama_sampler_chain_init(sp);
        llama_sampler_chain_add(smpl_, llama_sampler_init_temp(0.7f));
        llama_sampler_chain_add(smpl_, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
        return true;
    }

    void unload() {
        if (smpl_) { llama_sampler_free(smpl_); smpl_ = nullptr; }
        if (ctx_)  { llama_free(ctx_); ctx_ = nullptr; }
        if (lm_)   { llama_model_free(lm_); lm_ = nullptr; }
    }
    bool ready() const { return ctx_ != nullptr; }

    std::string generate(const std::string &user) {
        if (!ctx_) return "";
        llama_memory_clear(llama_get_memory(ctx_), true);     // fresh single turn
        std::string prompt = "<|im_start|>system\n" + sys_ +
                             "<|im_end|>\n<|im_start|>user\n" + user +
                             "<|im_end|>\n<|im_start|>assistant\n";
        std::vector<llama_token> toks = tokenize(prompt, true);
        llama_batch batch = llama_batch_get_one(toks.data(), (int)toks.size());
        std::string out;
        for (int i = 0; i < nPredict_; ++i) {
            if (llama_decode(ctx_, batch) != 0) break;
            llama_token id = llama_sampler_sample(smpl_, ctx_, -1);
            if (llama_vocab_is_eog(vocab_, id)) break;
            out += piece(id);
            cur_ = id;
            batch = llama_batch_get_one(&cur_, 1);
        }
        return trim(out);
    }

    ~LlamaLlm() { unload(); }

private:
    std::vector<llama_token> tokenize(const std::string &s, bool addSpecial) {
        int n = -llama_tokenize(vocab_, s.c_str(), (int)s.size(), nullptr, 0, addSpecial, true);
        std::vector<llama_token> t(n);
        llama_tokenize(vocab_, s.c_str(), (int)s.size(), t.data(), n, addSpecial, true);
        return t;
    }
    std::string piece(llama_token id) {
        char buf[256];
        int n = llama_token_to_piece(vocab_, id, buf, sizeof(buf), 0, true);
        return n > 0 ? std::string(buf, n) : "";
    }
    static std::string trim(std::string s) {
        size_t b = s.find_first_not_of(" \t\n");
        size_t e = s.find_last_not_of(" \t\n");
        return b == std::string::npos ? "" : s.substr(b, e - b + 1);
    }

    std::string model_, sys_;
    int nCtx_, nPredict_;
    llama_model *lm_ = nullptr;
    llama_context *ctx_ = nullptr;
    const llama_vocab *vocab_ = nullptr;
    llama_sampler *smpl_ = nullptr;
    llama_token cur_ = 0;
};
