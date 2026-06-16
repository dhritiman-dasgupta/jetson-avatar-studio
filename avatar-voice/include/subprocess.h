// Minimal child-process with stdin/stdout pipes (for the STT/LLM model workers).
#pragma once
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

class Subprocess {
public:
    bool start(const std::vector<std::string> &argv) {
        if (argv.empty()) return false;
        int inP[2], outP[2];
        if (pipe(inP) || pipe(outP)) return false;
        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {                       // child
            dup2(inP[0], STDIN_FILENO);
            dup2(outP[1], STDOUT_FILENO);
            close(inP[0]); close(inP[1]); close(outP[0]); close(outP[1]);
            std::vector<char *> a;
            for (auto &s : argv) a.push_back(const_cast<char *>(s.c_str()));
            a.push_back(nullptr);
            execv(a[0], a.data());
            _exit(127);
        }
        close(inP[0]); close(outP[1]);
        wfd_ = inP[1];
        rf_ = fdopen(outP[0], "r");
        return rf_ != nullptr;
    }

    void writeLine(const std::string &s) {
        if (wfd_ < 0) return;
        std::string l = s + "\n";
        ssize_t w = ::write(wfd_, l.data(), l.size());
        (void)w;
    }

    // Write raw bytes to the child's stdin (e.g. PCM frames to the hotword worker).
    void writeRaw(const void *p, size_t n) {
        if (wfd_ < 0) return;
        const char *c = (const char *)p;
        while (n > 0) {
            ssize_t w = ::write(wfd_, c, n);
            if (w <= 0) break;
            c += w; n -= (size_t)w;
        }
    }

    std::string readLine() {
        if (!rf_) return "";
        char *line = nullptr; size_t n = 0;
        ssize_t r = getline(&line, &n, rf_);
        std::string out;
        if (r > 0) { out.assign(line, r); if (!out.empty() && out.back() == '\n') out.pop_back(); }
        free(line);
        return out;
    }

    bool alive() {
        if (pid_ <= 0) return false;
        int st; return waitpid(pid_, &st, WNOHANG) == 0;
    }

    void kill() {
        if (pid_ > 0) { ::kill(pid_, SIGTERM); int st; waitpid(pid_, &st, 0); pid_ = -1; }
        if (rf_) { fclose(rf_); rf_ = nullptr; }
        if (wfd_ >= 0) { close(wfd_); wfd_ = -1; }
    }

    ~Subprocess() { kill(); }

private:
    pid_t pid_ = -1;
    int wfd_ = -1;
    FILE *rf_ = nullptr;
};
