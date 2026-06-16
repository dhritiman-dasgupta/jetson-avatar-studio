HOME_DIR := $(HOME)
WHISPER  := $(HOME_DIR)/voice-engines/whisper.cpp
LLAMA    := $(HOME_DIR)/voice-engines/llama.cpp

CXX      := g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -Iinclude
PTHREAD  := -lpthread

# --- whisper worker deps ---
WH_INC   := -I$(WHISPER)/include -I$(WHISPER)/ggml/include
WH_LIB   := -L$(WHISPER)/build/src -L$(WHISPER)/build/ggml/src -lwhisper
WH_RPATH := -Wl,-rpath,$(WHISPER)/build/src -Wl,-rpath,$(WHISPER)/build/ggml/src

# --- llama worker deps ---
LL_INC   := -I$(LLAMA)/include -I$(LLAMA)/ggml/include
LL_LIB   := -L$(LLAMA)/build/bin -L$(LLAMA)/build/src -L$(LLAMA)/build/ggml/src -lllama
LL_RPATH := -Wl,-rpath,$(LLAMA)/build/bin -Wl,-rpath,$(LLAMA)/build/src -Wl,-rpath,$(LLAMA)/build/ggml/src

all: avatar-voice stt-worker llm-worker

avatar-voice: src/main.cpp include/*.h
	$(CXX) $(CXXFLAGS) src/main.cpp -o avatar-voice $(PTHREAD) -lasound

stt-worker: src/stt_worker.cpp include/whisper_stt.h
	$(CXX) $(CXXFLAGS) $(WH_INC) src/stt_worker.cpp -o stt-worker $(WH_LIB) $(WH_RPATH)

llm-worker: src/llm_worker.cpp include/llama_llm.h
	$(CXX) $(CXXFLAGS) $(LL_INC) src/llm_worker.cpp -o llm-worker $(LL_LIB) $(LL_RPATH)

clean:
	rm -f avatar-voice stt-worker llm-worker
