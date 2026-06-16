// stt-worker: loads whisper.cpp once, transcribes raw float32 PCM files on request.
// Protocol (stdin lines):  "STT <path.f32>" -> stdout "TXT <text>";  "QUIT" -> exit.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "whisper_stt.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: stt-worker <model.bin>\n"); return 1; }
    WhisperStt stt(argv[1], 4);
    if (!stt.load()) { printf("ERR load\n"); fflush(stdout); return 1; }
    printf("READY\n"); fflush(stdout);

    char *line = nullptr; size_t n = 0;
    while (getline(&line, &n, stdin) > 0) {
        std::string cmd(line);
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
        if (cmd.rfind("STT ", 0) == 0) {
            std::string path = cmd.substr(4);
            std::vector<float> pcm;
            FILE *f = fopen(path.c_str(), "rb");
            if (f) {
                fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                pcm.resize(sz / sizeof(float));
                size_t rd = fread(pcm.data(), sizeof(float), pcm.size(), f);
                (void)rd; fclose(f);
            }
            std::string t = stt.transcribe(pcm);
            for (auto &c : t) if (c == '\n' || c == '\r') c = ' ';
            printf("TXT %s\n", t.c_str()); fflush(stdout);
        } else if (cmd == "QUIT") {
            break;
        }
    }
    free(line);
    return 0;
}
