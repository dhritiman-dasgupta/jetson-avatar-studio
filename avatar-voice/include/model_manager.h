// ModelManager: automatic offload/onload of models with a GPU VRAM budget.
// - pinned models (hotword) are never unloaded
// - GPU models are evicted (drain-then-switch) to fit the budget before loading
// - an idle reaper unloads models unused past their TTL
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class ModelState { Unloaded, Loading, Ready };

struct Model {
    std::string name;
    bool pinned = false;          // hotword: always resident
    bool usesGpu = false;
    size_t vramMB = 0;
    int idleTtlMs = 20000;
    ModelState state = ModelState::Unloaded;
    std::chrono::steady_clock::time_point lastUsed{};
    std::function<bool()> doLoad = [] { return true; };   // real impl plugs in here
    std::function<void()> doUnload = [] {};
};

class ModelManager {
public:
    using EventCb = std::function<void(const std::string &name, const std::string &state)>;

    explicit ModelManager(size_t vramBudgetMB, EventCb cb = nullptr)
        : vramBudget_(vramBudgetMB), onEvent_(std::move(cb)) {}

    ~ModelManager() { stop(); }

    void registerModel(std::shared_ptr<Model> m) {
        std::lock_guard<std::mutex> lk(mtx_);
        models_[m->name] = std::move(m);
    }

    // Ensure a model is loaded (evicting GPU models as needed) and mark it used.
    bool acquire(const std::string &name) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = models_.find(name);
        if (it == models_.end()) return false;
        auto &m = it->second;
        if (m->state == ModelState::Ready) { m->lastUsed = clock_::now(); return true; }
        if (m->usesGpu) evictGpuForLocked(m->vramMB, name);
        m->state = ModelState::Loading;
        emit(m->name, "loading");
        bool ok = m->doLoad();
        m->state = ok ? ModelState::Ready : ModelState::Unloaded;
        m->lastUsed = clock_::now();
        emit(m->name, ok ? "loaded" : "error");
        return ok;
    }

    void touch(const std::string &name) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = models_.find(name);
        if (it != models_.end()) it->second->lastUsed = clock_::now();
    }

    void start() {
        running_ = true;
        reaper_ = std::thread([this] {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                reapIdle();
            }
        });
    }

    void stop() {
        running_ = false;
        if (reaper_.joinable()) reaper_.join();
    }

private:
    using clock_ = std::chrono::steady_clock;

    void emit(const std::string &n, const std::string &s) { if (onEvent_) onEvent_(n, s); }

    size_t gpuUsedLocked() const {
        size_t u = 0;
        for (auto &kv : models_)
            if (kv.second->usesGpu && kv.second->state == ModelState::Ready) u += kv.second->vramMB;
        return u;
    }

    // Unload Ready, non-pinned GPU models (oldest first) until `needMB` fits the budget.
    void evictGpuForLocked(size_t needMB, const std::string &keep) {
        if (gpuUsedLocked() + needMB <= vramBudget_) return;
        std::vector<Model *> cand;
        for (auto &kv : models_) {
            auto *m = kv.second.get();
            if (m->usesGpu && !m->pinned && m->state == ModelState::Ready && m->name != keep)
                cand.push_back(m);
        }
        std::sort(cand.begin(), cand.end(),
                  [](Model *a, Model *b) { return a->lastUsed < b->lastUsed; });
        for (auto *m : cand) {
            if (gpuUsedLocked() + needMB <= vramBudget_) break;
            m->doUnload();
            m->state = ModelState::Unloaded;
            emit(m->name, "unloaded");
        }
    }

    void reapIdle() {
        std::lock_guard<std::mutex> lk(mtx_);
        auto now = clock_::now();
        for (auto &kv : models_) {
            auto *m = kv.second.get();
            if (m->pinned || m->state != ModelState::Ready) continue;
            auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - m->lastUsed).count();
            if (idle > m->idleTtlMs) {
                m->doUnload();
                m->state = ModelState::Unloaded;
                emit(m->name, "unloaded");
            }
        }
    }

    size_t vramBudget_;
    EventCb onEvent_;
    std::map<std::string, std::shared_ptr<Model>> models_;
    std::mutex mtx_;
    std::thread reaper_;
    std::atomic<bool> running_{false};
};
