// llm-worker: loads llama.cpp once, answers prompts on request.
// Protocol (stdin lines):  "GEN <text>" -> stdout "RPL <reply>";  "QUIT" -> exit.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "llama_llm.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: llm-worker <model.gguf>\n"); return 1; }
    const std::string sys =
        "You are a warm, concise avatar companion. Reply in one short, friendly sentence.";
    LlamaLlm llm(argv[1], sys, 2048, 80);
    if (!llm.load()) { printf("ERR load\n"); fflush(stdout); return 1; }
    printf("READY\n"); fflush(stdout);

    char *line = nullptr; size_t n = 0;
    while (getline(&line, &n, stdin) > 0) {
        std::string cmd(line);
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
        if (cmd.rfind("GEN ", 0) == 0) {
            std::string reply = llm.generate(cmd.substr(4));
            for (auto &c : reply) if (c == '\n' || c == '\r') c = ' ';
            printf("RPL %s\n", reply.c_str()); fflush(stdout);
        } else if (cmd == "QUIT") {
            break;
        }
    }
    free(line);
    return 0;
}
