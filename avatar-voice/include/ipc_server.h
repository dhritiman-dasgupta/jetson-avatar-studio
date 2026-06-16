// Tiny line-delimited JSON IPC over a Unix domain socket.
// The PyQt UI connects, receives state/event lines, and sends commands.
#pragma once
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

class IpcServer {
public:
    using LineCb = std::function<void(const std::string &line)>;

    explicit IpcServer(std::string path) : path_(std::move(path)) {}
    ~IpcServer() { stop(); }

    void onLine(LineCb cb) { cb_ = std::move(cb); }

    bool start() {
        ::unlink(path_.c_str());
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
        if (::bind(fd_, (sockaddr *)&addr, sizeof(addr)) < 0) return false;
        if (::listen(fd_, 4) < 0) return false;
        running_ = true;
        accept_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
        if (accept_.joinable()) accept_.join();
        std::lock_guard<std::mutex> lk(mtx_);
        for (int c : clients_) ::close(c);
        clients_.clear();
        ::unlink(path_.c_str());
    }

    // Broadcast one JSON line to all connected clients (and cache by type for replay).
    void broadcast(const std::string &json) {
        std::string line = json + "\n";
        std::lock_guard<std::mutex> lk(mtx_);
        std::string type = jtype(json);
        if (type == "state" || type == "transcript" || type == "reply" || type == "micgain")
            snapshot_[type] = line;        // latest state for new clients
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (::send(*it, line.data(), line.size(), MSG_NOSIGNAL) < 0) {
                ::close(*it); it = clients_.erase(it);
            } else ++it;
        }
    }

private:
    static std::string jtype(const std::string &js) {
        const std::string pat = "\"type\":\"";
        size_t p = js.find(pat);
        if (p == std::string::npos) return "";
        p += pat.size();
        size_t e = js.find('"', p);
        return e == std::string::npos ? "" : js.substr(p, e - p);
    }

    void acceptLoop() {
        while (running_) {
            int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) { if (!running_) break; continue; }
            {   std::lock_guard<std::mutex> lk(mtx_);
                clients_.insert(c);
                for (auto &kv : snapshot_)   // replay current captions to the new client
                    ::send(c, kv.second.data(), kv.second.size(), MSG_NOSIGNAL);
            }
            std::thread([this, c] { clientLoop(c); }).detach();
        }
    }

    void clientLoop(int c) {
        std::string buf;
        char tmp[1024];
        while (running_) {
            ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            buf.append(tmp, n);
            size_t pos;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                if (!line.empty() && cb_) cb_(line);
            }
        }
        { std::lock_guard<std::mutex> lk(mtx_); clients_.erase(c); }
        ::close(c);
    }

    std::string path_;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_;
    std::mutex mtx_;
    std::set<int> clients_;
    std::map<std::string, std::string> snapshot_;   // latest line per caption type
    LineCb cb_;
};
