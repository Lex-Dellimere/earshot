# earshot

Listen to one audio source (all system output, the mic, one app like Spotify or
Brave, or a specific device), transcribe it live with whisper.cpp, optionally
rewrite the transcript with an AI instruction such as "remove repeats and
summarise", and clip the last N seconds when something happens, like OBS's
replay buffer.

It runs on Linux (PipeWire, X11 or Wayland) and Windows 10/11 (WASAPI). It's
written in C-style C++ and built with g++ and CMake. whisper.cpp is vendored
and linked statically.

| | Linux | Windows |
|---|---|---|
| binary | ~2.0 MB, links PipeWire + libstdc++ | ~3.9 MB static exe, system DLLs only |
| per-app capture | PipeWire stream target | WASAPI process loopback (Win10 build 20348+) |
| global hotkeys | X11 grab; Wayland via `earshot --send` | `RegisterHotKey` |
| control | FIFO in `$XDG_RUNTIME_DIR` | named pipe |

## Build

```sh
cmake -B build && cmake --build build -j       # linux
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake && cmake --build build-win -j   # windows exe
./build/earshot --download base.en             # plus: --download silero (speech detector)
./build/earshot
```

On Windows itself, build with MSYS2 MinGW g++ and the same CMake commands.

CMake options (all `-D…=ON/OFF`):

- `EARSHOT_STRIP` (on): strips symbols after linking.
- `EARSHOT_LTO` (on for Linux, off for Windows): link-time optimisation.
- `EARSHOT_NATIVE` (on for Linux, off for Windows): `-march=native`. Turn it off for a binary you'll copy to other machines.
- `EARSHOT_CUDA` / `EARSHOT_VULKAN`: run whisper on the GPU. CUDA needs `nvcc`; Vulkan needs `glslc`.

MinGW GCC 14 hits an internal compiler error in ggml's SIMD code under LTO or
AVX-512, so Windows builds default to an AVX2 baseline without LTO.

## Use

```sh
earshot                    # tui on all system output
earshot -s spotify         # one app
earshot --list             # sources
earshot --print -s brave   # no tui, transcript on stdout
earshot --send play        # drive a running earshot: clip replay play process clear quit
```

Default keys (all configurable):

| tui key | global hotkey | |
|---|---|---|
| `c` | ctrl+alt+c | start a clip that begins `before` seconds ago; press again to stop |
| `r` | ctrl+alt+r | save the last `replay` seconds right now |
| `space` | ctrl+alt+p | play back the last `playback` seconds; press again to stop |
| `i` | | edit the AI instruction (saved to the config) |
| `p` | | run the instruction now |
| `x` / `q` | | clear / quit |

**Wayland:** bind keys in your compositor to `earshot --send clip|replay|play`:

- GNOME: Settings → Keyboard → Custom Shortcuts
- KDE: System Settings → Shortcuts → Custom
- Hyprland: `bind = CTRL ALT, R, exec, earshot --send replay`
- Sway: `bindsym Ctrl+Alt+r exec earshot --send replay`

**Output** goes to `out_dir` (default `~/Music/earshot`):

- clips and replays as 48 kHz stereo WAV, each with a `.txt` transcript of the same window
- the session transcript in `transcripts/<date>.txt`
- AI output in `transcripts/<date>-processed.txt`

## Config

The config is written with comments on first run, at
`~/.config/earshot/earshot.conf` on Linux and `%APPDATA%\earshot\earshot.conf`
on Windows. Pass `-c` to use another file. Every option is in that file:
model, language, VAD, filters, clip lengths, AI endpoint, keys and hotkeys.

## Hard audio: music, noise, quiet sources

These run between capture and whisper:

1. **Anti-alias resampling.** A 63-tap windowed-sinc FIR takes 48 kHz down to 16 kHz without aliasing hiss.
2. **Centre focus** (`focus`). An STFT mask keeps sound that is the same in both channels. Voices are usually mixed to the centre and instruments panned, so this works as a cheap vocal isolation. It has no effect on mono audio.
3. **Highpass** (`highpass = 100`). Removes bass and rumble.
4. **Gain control and normalisation** (`normalize`). A slow AGC lifts quiet sources before speech detection, and each chunk is peak-normalised before whisper.
5. **Silero VAD** (`vad = silero`). A neural speech detector that ignores music between words, unlike an energy gate. Use `vad = off` to transcribe everything, e.g. lyrics.
6. **Context** (`context`). The previous line is passed as whisper's prompt so sentences carry across chunks.

Results on a test clip of speech under loud panned chords, bass and hi-hats at
about 0 dB SNR:

- before: "And so am I, fellow Americans! AS NOT! Watch your country can do it!"
- now: "and so my fellow Americans ask not what your country can do for you…"

Speech at −30 dB went from no output to word-perfect.

For more accuracy use a bigger model: `--download small.en-q5_1`, then set
`model = small.en-q5_1`. True stem separation (Demucs/UVR) needs a separate
neural model and runtime, which is out of scope for a small dependency-free
binary.

## AI step

Any OpenAI-compatible `/chat/completions` endpoint works. The default is local
Ollama with `gemma3:4b`. In testing it stayed closest to what was actually said;
llama3.2-3B and qwen2.5-3B made up facts. If the endpoint is down, earshot runs
`llm_start` (default `ollama serve`) and stops it on exit.

```sh
ollama pull gemma3:4b
```

For a hosted free tier (Groq):

```
llm_url = https://api.groq.com/openai/v1/chat/completions
llm_model = llama-3.1-8b-instant
```

and set `EARSHOT_API_KEY` in your environment. The key is passed to curl
through a private temp file, so it never shows in `ps`.

With no instruction set, nothing leaves your machine.

## Limits

- An app with several audio streams is captured through its first one. earshot re-attaches when the app restarts.
- On Windows, app capture needs build 20348 or newer. The Windows TUI was checked under Wine, whose terminal console mangles the layout; real Windows Terminal or conhost is untested.
