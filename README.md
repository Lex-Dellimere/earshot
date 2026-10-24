# earshot

Listen to what you hear (all system output, one app like Spotify or Discord,
or a specific device) and your own mic at the same time, transcribe both live
with whisper.cpp as a readable "Them / You" transcript, optionally clean it up
with a local AI, and clip the last N seconds when something happens, like OBS's
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
- `EARSHOT_VULKAN` / `EARSHOT_CUDA`: run whisper on the GPU. See [GPU](#gpu).

MinGW GCC 14 hits an internal compiler error in ggml's SIMD code under LTO or
AVX-512, so Windows builds default to an AVX2 baseline without LTO.

## GPU

The default build runs whisper on the CPU, so it works on every machine. For
GPU speed, build with Vulkan. One Vulkan binary runs on AMD, Intel and NVIDIA,
on Linux and Windows:

```sh
sudo apt install glslc            # shader compiler, needed only to build
cmake -B build-vk -DEARSHOT_VULKAN=ON && cmake --build build-vk -j
```

SPIR-V headers are fetched automatically if the system doesn't have them. On
Windows, use MSYS2 with `mingw-w64-x86_64-vulkan-devel` and
`mingw-w64-x86_64-shaderc`.

If no Vulkan driver is found at runtime, the same binary falls back to the
CPU. With several GPUs, pick one with `gpu_device` in the config. Devices are
counted from 0, in the order Vulkan lists them; integrated GPUs often come
first. The Vulkan binary is about 37 MB because ggml embeds its compiled GPU
shaders, which is why it's opt-in.

`EARSHOT_CUDA=ON` is NVIDIA only and needs the CUDA toolkit. On the 4070 below
it beats Vulkan, but Vulkan already gives far more speed than live
transcription needs.

Time to encode one 30 s window, the main cost per chunk:

| model | CPU (Ryzen, 8 threads) | AMD Radeon 780M iGPU | RTX 4070 laptop |
|---|---|---|---|
| base.en | 386 ms | 58 ms | 6 ms |
| small.en-q5_1 | 1835 ms | 202 ms | 17 ms |

The first run after installing compiles GPU shaders, which can take several
seconds. earshot captures from the start and catches up, so no speech is lost.

## Use

```sh
earshot                    # tui: system output as Them, default mic as You
earshot -s spotify -m off  # one app, no mic
earshot -m "Arctis"        # pick the mic by part of its device name
earshot --list             # sources, marked app or you
earshot --print -s brave   # no tui, transcript on stdout
earshot --send play        # drive a running earshot: clip replay play process clear quit
earshot --file talk.wav    # transcribe a wav file and exit
earshot --set beam=5       # override any config value for one run
```

Default keys (all configurable in settings):

| tui key | global hotkey | |
|---|---|---|
| `↑↓` `Enter` | | in the sources list: Enter on a mic or input sets You, on anything else sets Them, on the active one turns that channel off |
| `Tab` | | the sources list on narrow terminals |
| `s` | | settings |
| `c` | ctrl+alt+c | start a clip that begins `before` seconds ago; press again to stop |
| `r` | ctrl+alt+r | save the last `replay` seconds right now |
| `space` | ctrl+alt+p | play back the last `playback` seconds; press again to stop |
| `a` | | next AI model slot, then off |
| `w` | | next whisper model; it loads in the background and swaps in without a gap |
| `i` / `p` | | edit the AI instruction / run it now |
| `t` | | timestamps on or off |
| `o` | | open the transcript folder |
| `x` / `q` / `?` | | clear / quit / all keys |

**Wayland:** bind keys in your compositor to `earshot --send clip|replay|play`:

- GNOME: Settings → Keyboard → Custom Shortcuts
- KDE: System Settings → Shortcuts → Custom
- Hyprland: `bind = CTRL ALT, R, exec, earshot --send replay`
- Sway: `bindsym Ctrl+Alt+r exec earshot --send replay`

## Two channels: Them and You

earshot captures two streams at once: **Them** is what you hear (`source`) and
**You** is your mic (`mic`, empty for off). Each has its own meter in the title
bar. When both are on, every paragraph is named, and the names are
`them_label` and `you_label`.

If your speakers bleed into the mic, a You line that repeats a Them line from
the same few seconds is dropped. Headphones avoid the problem entirely.

## Transcripts

Lines from the same speaker with short pauses are joined into paragraphs, in
the TUI and in the file. Timestamps are off unless you press `t` or set
`timestamps = true`.

- **Transcripts** go to `transcript_dir` (default `~/Documents/earshot`) as
  `<date>_<time>_<source>.txt`. The file is created on the first paragraph and
  its path is shown above the transcript. AI output goes next to it as
  `…_ai.txt`.
- **Clips and replays** go to `clip_dir` (default `~/Music/earshot`) as 48 kHz
  stereo WAV, each with a `.txt` transcript of the same window. With the mic on,
  a clip also writes a `-mic.wav`.

## Settings

Press `s` for the settings screen. Every option is listed by section with what
it does. Left and right change a value, Enter edits text or types a path, and
each change is saved at once. Most apply immediately; the few that need a
restart (GPU, speech detection type, global hotkeys) are marked ↻.

The same settings live in a commented text file at
`~/.config/earshot/earshot.conf` on Linux and `%APPDATA%\earshot\earshot.conf`
on Windows. Pass `-c` to use another file. Older config files are upgraded on
load.

## Whisper models

`model` is a name from the data folder (`~/.local/share/earshot`, or
`%LOCALAPPDATA%\earshot` on Windows) or a full path to any ggml model file.
`model_paths` lists more files or folders, separated by `;`. The `w` key and the
settings screen step through every model found, and settings also accept a
typed path.

```sh
earshot --download small.en
earshot --download large-v3-turbo-q5_0
```

## Hard audio: music, noise, quiet sources

These run between capture and whisper:

1. **Anti-alias resampling.** A 63-tap windowed-sinc FIR takes 48 kHz down to 16 kHz without aliasing hiss.
2. **Centre focus** (`focus`, off by default). An STFT mask keeps sound that is the same in both channels. It helps when the noise is wide, like a crowd, but music with a centred lead vocal gets through it and it costs accuracy there.
3. **Highpass** (`highpass = 100`). Removes bass and rumble.
4. **Gain control and normalisation** (`normalize`). A slow AGC lifts quiet sources before speech detection, and each chunk is peak-normalised before whisper.
5. **Silero VAD** (`vad = silero`). A neural speech detector that ignores music between words, unlike an energy gate. Use `vad = off` to transcribe everything, e.g. lyrics.
6. **Context** (`context`). The previous line is passed as whisper's prompt so sentences carry across chunks. Over music, whisper sometimes hands that prompt back as if it had heard it again; when most of a result comes from the prompt, earshot decodes the chunk again without it.
7. **Filler filter.** One or two word lines whisper is unsure of (the stray "I" or "Oh" it invents from noise) are dropped.

### Which model

Word error rate (share of words wrong, lower is better) on the JFK sample mixed
with synthetic drums, bass and chords at +6, 0 and −5 dB, the same with a sung
lead in the centre, bursts of claps, babble and a −24 dB quiet copy. A
music-only clip checked for invented words: no setting produced any.

| model | where | mean WER | time for 3 min of audio |
|---|---|---|---|
| base.en | CPU | 16.8 % | 29 s |
| small.en-q5_1 | CPU | 3.2 % | 129 s |
| small.en-q5_1 | AMD 780M iGPU | 3.2 % | 22 s |
| large-v3-turbo-q5_0, beam 5 | AMD 780M iGPU | 1.8 % | 104 s |
| large-v3-turbo-q5_0, beam 5 | RTX 4070 laptop | 2.7 % | 12 s |

So: `base.en` for slow machines, `small.en-q5_1` on a CPU that can spare it or
on an integrated GPU, and `large-v3-turbo-q5_0` with `beam = 5` on a discrete
GPU. With both channels talking at once, whisper does twice the work, so leave
headroom. The test speech is one 22-word sentence, so small differences between
rows are noise; the big ones are not.

Before the prompt fix in step 6, small.en-q5_1 on the CPU scored 21.8 % on the
same clips, nearly all of it from repeated lines. `focus` made the larger
models 2 to 7 times worse on the sung-lead clips and only helped on babble, so
it is off by default.

Sung lyrics over loud music stay hard for every model. Stem separation
(Demucs/UVR) would help but needs a separate neural model and runtime, which is
out of scope for a small dependency-free binary.

## AI

The AI is on by default with the instruction "fix words that were clearly
misheard and add punctuation", and its output appears in the AI pane. It needs
[Ollama](https://ollama.com) with a model pulled:

```sh
ollama pull gemma3:4b
```

If Ollama isn't running, earshot starts it (`llm_start`) and stops it on exit.

Models live in numbered slots, `ai1` to `ai9`, and `ai` picks one (0 is off).
The `a` key steps through them while running. A slot can be:

```
ai1 = gemma3:4b                                    # an ollama model
ai2 = llama-3.1-8b-instant @ https://api.groq.com/openai/v1/chat/completions
ai3 = ~/models/Mistral-7B-Instruct.Q4_K_M.gguf     # any gguf file, imported into ollama on first use
```

For hosted endpoints, set `EARSHOT_API_KEY` in your environment. The key goes
to curl through a private temp file, so it never shows in `ps`.

Small local models are cautious editors rather than fixers. In testing,
gemma3:4b kept the speaker names and changed the least; qwen2.5:3b and
llama3.2:3b sometimes invented words. None of them could recover a word
whisper misheard badly, so accuracy comes mostly from the whisper model.

With the AI off (`ai = 0` or an empty instruction), nothing leaves your machine
and nothing extra runs.

## Testing

```sh
cmake -B build -DEARSHOT_TESTS=ON && cmake --build build -j && ctest --test-dir build
cmake -B build-asan -DEARSHOT_SANITIZE="address;undefined" -DEARSHOT_TESTS=ON
cmake -B build-tsan -DEARSHOT_SANITIZE=thread -DEARSHOT_TESTS=ON
```

`--file` runs a WAV through the same pipeline as live capture, faster than
real time. Clips saved with `c` or `r` make good test cases for audio that
trips whisper up.

## Limits

- An app with several audio streams is captured through its first one. earshot re-attaches when the app restarts.
- Two-channel capture, settings and model switching are built and cross-compiled for Windows but have only been run on Linux.
- On Windows, app capture needs build 20348 or newer. The Windows TUI was checked under Wine, whose terminal console mangles the layout; real Windows Terminal or conhost is untested.
