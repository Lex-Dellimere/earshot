# earshot

Listen to one audio source on Linux (all system output, the mic, a single app like
Spotify or Brave, or a specific device), transcribe it live with whisper.cpp,
optionally rewrite the transcript with an instruction ("remove repeats and
summarise"), and clip the last N seconds the moment something funny happens,
like OBS's replay buffer.

C-style C++ built with g++ and CMake. Runtime deps: PipeWire, libstdc++, libgomp, and
the `curl` binary (only for the LLM step). whisper.cpp is vendored and linked
statically. The stripped binary is about 2.5 MB.

## Build

```sh
cmake -B build && cmake --build build -j
./download-model.sh base.en        # or tiny.en / small.en / large-v3-turbo
./build/earshot
```

GPU: `cmake -B build -DEARSHOT_CUDA=ON` (needs `nvidia-cuda-toolkit`) or
`-DEARSHOT_VULKAN=ON` (needs `glslc`). CPU is fine for `base.en`/`small.en`.

## Use

```
earshot                         # TUI, listening to all system output
earshot -s spotify              # one app
earshot --list                  # see sources
earshot --print -s brave        # no TUI, transcript on stdout
```

| key | |
|---|---|
| ↑↓ / Enter | pick source |
| c | start a clip that begins `-b` seconds ago (default 10); press again to stop |
| r | save the last `-r` seconds right now (default 30) |
| i | edit the instruction (saved to `~/.config/earshot/instruction.txt`) |
| p | run the instruction now instead of waiting `-e` seconds |
| x | clear · PgUp/PgDn scroll · q quit |

Clips go to `~/Music/earshot/` as 48 kHz stereo WAV with a `.txt` transcript of
the same window. Two signals do the same without focusing the terminal, so you
can bind them to global hotkeys in your desktop:

```sh
pkill -USR1 earshot   # save replay
pkill -USR2 earshot   # start/stop clip
```

## Instructions (LLM step)

Any OpenAI-compatible `/chat/completions` endpoint works. Free options:

- **Ollama (local, default):** `ollama pull llama3.2`. earshot uses
  `http://localhost:11434/v1/chat/completions`.
- **Groq (free tier):** `EARSHOT_LLM_URL=https://api.groq.com/openai/v1/chat/completions
  EARSHOT_LLM_MODEL=llama-3.1-8b-instant EARSHOT_API_KEY=... earshot`

Only new transcript lines are sent each time, along with recent output as context so
repeats get dropped. With no instruction, nothing leaves your machine.

## Limits

- An app with several streams (e.g. Brave tabs) is captured through its first
  stream; earshot re-attaches automatically when that app starts a new one.
- Whisper on music gives lyric-ish guesses; it is tuned for speech.
- Speech is split on pauses (energy VAD), so lines arrive 1–8 s after they're said.
