# earshot

listen to what you hear (all system output, one app like spotify or discord,
or a specific device) and your own mic at the same time, transcribe both live
with whisper.cpp as a readable "them / you" transcript, optionally clean it up
with a local ai, and clip the last n seconds when something happens, like obs's
replay buffer.

it runs on linux (pipewire, x11 or wayland) and windows 10/11 (wasapi). it's
written in c-style c++ and built with g++ and cmake. whisper.cpp is vendored
and linked statically.

| | linux | windows |
|---|---|---|
| binary | ~2.0 mb, links pipewire + libstdc++ | ~3.9 mb static exe, system dlls only |
| per-app capture | pipewire stream target | wasapi process loopback (win10 build 20348+) |
| global hotkeys | x11 grab; wayland via `earshot --send` | `RegisterHotKey` |
| control | fifo in `$XDG_RUNTIME_DIR` | named pipe |

## install

```sh
git clone https://github.com/Lex-Dellimere/earshot && cd earshot && ./install.sh
```

`install.sh` works on linux (debian, ubuntu, fedora, arch, opensuse, alpine,
void) and on windows from git bash or the msys2 shell. it:

1. installs whatever build tools are missing with the system's package manager,
   or msys2's mingw toolchain on windows (installing msys2 with winget if needed)
2. builds earshot with vulkan when there is a gpu, and for the cpu otherwise,
   then copies it to `~/.local/bin` (or `%LOCALAPPDATA%\Programs\earshot`)
3. finds ollama, asks where it is if it can't, or installs it
4. reads the gpus through `earshot --gpus` and picks the models that fit:

   | hardware | whisper | ai |
   |---|---|---|
   | gpu with 7.5 gb or more | large-v3-turbo-q5_0, beam 5 | qwen3.5:9b |
   | gpu with 5 to 7.5 gb | large-v3-turbo-q5_0, beam 5 | qwen3.5:4b |
   | gpu with 3 to 5 gb | small.en-q5_1, beam 5 | qwen3.5:4b |
   | integrated gpu | small.en-q5_1 | qwen3.5:4b |
   | cpu only | small.en-q5_1 with 8+ threads, else base.en | qwen3.5:4b with 12 gb of ram, else off |

5. downloads them and writes a config that uses them

run it again whenever you like; finished steps are skipped. options:
`--yes` answers every question, `--cpu` or `--gpu` forces the build type,
`--no-ai` skips ollama, `--prefix dir` changes where earshot goes, and
`--dry-run` shows what it would do. on alpine, `apk add bash` first.

## build by hand

```sh
cmake -B build && cmake --build build -j       # linux
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake && cmake --build build-win -j   # windows exe
./build/earshot --download base.en             # plus: --download silero (speech detector)
./build/earshot
```

on windows itself, build with msys2 mingw g++ and the same cmake commands.

cmake options (all `-D…=on/off`):

- `EARSHOT_STRIP` (on): strips symbols after linking.
- `EARSHOT_LTO` (on for linux, off for windows): link-time optimisation.
- `EARSHOT_NATIVE` (on for linux, off for windows): `-march=native`. turn it off for a binary you'll copy to other machines.
- `EARSHOT_VULKAN` / `EARSHOT_CUDA`: run whisper on the gpu. see [gpu](#gpu).

mingw gcc 14 hits an internal compiler error in ggml's simd code under lto or
avx-512, so windows builds default to an avx2 baseline without lto.

## gpu

the default build runs whisper on the cpu, so it works on every machine. for
gpu speed, build with vulkan. one vulkan binary runs on amd, intel and nvidia,
on linux and windows:

```sh
sudo apt install glslc            # shader compiler, needed only to build
cmake -B build-vk -DEARSHOT_VULKAN=on && cmake --build build-vk -j
```

spir-v headers are fetched automatically if the system doesn't have them. on
windows, use msys2 with `mingw-w64-ucrt-x86_64-vulkan-devel` and
`mingw-w64-ucrt-x86_64-shaderc`.

if no vulkan driver is found at runtime, the same binary falls back to the
cpu. with several gpus, pick one with `gpu_device` in the config. devices are
counted from 0, in the order vulkan lists them; integrated gpus often come
first. the vulkan binary is about 37 mb because ggml embeds its compiled gpu
shaders, which is why it's opt-in.

`EARSHOT_CUDA=on` is nvidia only and needs the cuda toolkit. on the 4070 below
it beats vulkan, but vulkan already gives far more speed than live
transcription needs.

time to encode one 30 s window, the main cost per chunk:

| model | cpu (ryzen, 8 threads) | amd radeon 780m igpu | rtx 4070 laptop |
|---|---|---|---|
| base.en | 386 ms | 58 ms | 6 ms |
| small.en-q5_1 | 1835 ms | 202 ms | 17 ms |

the first run after installing compiles gpu shaders, which can take several
seconds. earshot captures from the start and catches up, so no speech is lost.

## use

```sh
earshot                    # tui: system output as them, default mic as you
earshot -s spotify -m off  # one app, no mic
earshot -m "arctis"        # pick the mic by part of its device name
earshot --list             # sources, marked app or you
earshot --print -s brave   # no tui, transcript on stdout
earshot --send play        # drive a running earshot: clip replay play process clear quit
earshot --file talk.wav    # transcribe a wav file and exit
earshot --set beam=5       # override any config value for one run
```

the left side lists what you can **hear** (system, apps, outputs) and what can
be your **mic**, each with an **off** row. enter or a click picks one; the
current one is marked ● and shows its state, like "idle" when an app is paused.
on narrow terminals the list is behind tab. when the transcript is empty it says
why.

default keys (all configurable in settings):

| tui key | global hotkey | |
|---|---|---|
| `↑↓` `enter` | | pick a source (or click it) |
| `w` | | choose the whisper model from a list (or click "whisper" in the title bar) |
| `a` | | choose the ai model or turn it off (or click "ai") |
| `s` | | settings |
| wheel, `pgup` `pgdn` | | scroll the transcript; `home` goes to the top, `end` follows again |
| `c` | ctrl+alt+c | start a clip that begins `before` seconds ago; press again to stop |
| `r` | ctrl+alt+r | save the last `replay` seconds right now |
| `space` | ctrl+alt+p | play back the last `playback` seconds; press again to stop |
| `i` / `p` | | edit the ai instruction / run the ai now |
| `t` | | timestamps on or off |
| `o` | | open the transcript folder |
| `tab` | | the sources list on narrow terminals |
| `x` / `q` / `?` | | clear / quit / all keys |

**wayland:** bind keys in your compositor to `earshot --send clip|replay|play`:

- gnome: settings → keyboard → custom shortcuts
- kde: system settings → shortcuts → custom
- hyprland: `bind = CTRL ALT, R, exec, earshot --send replay`
- sway: `bindsym Ctrl+Alt+r exec earshot --send replay`

## two channels: them and you

earshot captures two streams at once: **them** is what you hear (`source`) and
**you** is your mic (`mic`, empty for off). each has its own meter in the title
bar. when both are on, every paragraph is named, and the names are
`them_label` and `you_label`.

if your speakers bleed into the mic, a you line that repeats a them line from
the same few seconds is dropped. headphones avoid the problem entirely.

## transcripts

while someone is speaking, a grey guess of the sentence so far updates about
once a second (`live`, `live_every`) and is replaced by the final text when
they pause. turn it off on slow machines if whisper falls behind.

lines from the same speaker with short pauses are joined into paragraphs, in
the tui and in the file. timestamps are off unless you press `t` or set
`timestamps = true`.

- **transcripts** go to `transcript_dir` (default `~/Documents/earshot`) as
  `<date>_<time>_<source>.txt`. the file is created on the first paragraph and
  its path is shown above the transcript. ai output goes next to it as
  `…_ai.txt`.
- **clips and replays** go to `clip_dir` (default `~/Music/earshot`) as 48 khz
  stereo wav, each with a `.txt` transcript of the same window. with the mic on,
  a clip also writes a `-mic.wav`.

## settings

press `s` for the settings screen. every option is listed by section with what
it does. left and right change a value, enter edits text or types a path, and
each change is saved at once. most apply immediately; the few that need a
restart (gpu, speech detection type, global hotkeys) are marked ↻.

the same settings live in a commented text file at
`~/.config/earshot/earshot.conf` on linux and `%APPDATA%\earshot\earshot.conf`
on windows. pass `-c` to use another file. older config files are upgraded on
load.

## whisper models

`model` is a name from the data folder (`~/.local/share/earshot`, or
`%LOCALAPPDATA%\earshot` on windows) or a full path to any ggml model file.
`model_paths` lists more files or folders, separated by `;`. the `w` key and the
settings screen step through every model found, and settings also accept a
typed path.

```sh
earshot --download small.en
earshot --download large-v3-turbo-q5_0
```

## hard audio: music, noise, quiet sources

these run between capture and whisper:

1. **noise suppression for detection** (`denoise`, on). rnnoise cleans a copy of the audio that only the speech detector sees. feeding denoised audio to whisper itself doubled its word errors in tests (whisper is trained on noisy audio and trips over denoiser artefacts), but using it to decide when someone speaks lowered errors a little, most on crowds (45 % to 18 % on the crowd clip).
2. **limiter** (`limiter`, off). pulls bangs and shouts far above the running level down. results were mixed, so it is opt-in.
3. **anti-alias resampling.** a 63-tap windowed-sinc fir takes 48 khz down to 16 khz without aliasing hiss.
4. **centre focus** (`focus`, off by default). an stft mask keeps sound that is the same in both channels. it helps when the noise is wide, like a crowd, but music with a centred lead vocal gets through it and it costs accuracy there.
5. **highpass** (`highpass = 100`). removes bass and rumble.
6. **gain control and normalisation** (`normalize`). a slow agc lifts quiet sources before speech detection, and each chunk is peak-normalised before whisper.
7. **silero vad** (`vad = silero`). a neural speech detector that ignores music between words, unlike an energy gate. use `vad = off` to transcribe everything, e.g. lyrics.
8. **context** (`context`). the previous line is passed as whisper's prompt so sentences carry across chunks. over music, whisper sometimes hands that prompt back as if it had heard it again; when most of a result comes from the prompt, earshot decodes the chunk again without it.
9. **filler filter.** one or two word lines whisper is unsure of (the stray "i" or "oh" it invents from noise) are dropped.

### which model

word error rate (share of words wrong, lower is better) on the jfk sample mixed
with synthetic drums, bass and chords at +6, 0 and −5 db, the same with a sung
lead in the centre, bursts of claps, babble and a −24 db quiet copy. a
music-only clip checked for invented words: no setting produced any.

| model | where | mean wer | time for 3 min of audio |
|---|---|---|---|
| base.en | cpu | 16.8 % | 29 s |
| small.en-q5_1 | cpu | 3.2 % | 129 s |
| small.en-q5_1 | amd 780m igpu | 3.2 % | 22 s |
| large-v3-turbo-q5_0, beam 5 | amd 780m igpu | 1.8 % | 104 s |
| large-v3-turbo-q5_0, beam 5 | rtx 4070 laptop | 2.7 % | 12 s |

so: `base.en` for slow machines, `small.en-q5_1` on a cpu that can spare it or
on an integrated gpu, and `large-v3-turbo-q5_0` with `beam = 5` on a discrete
gpu. with both channels talking at once, whisper does twice the work, so leave
headroom. the test speech is one 22-word sentence, so small differences between
rows are noise; the big ones are not.

before the prompt fix in step 8, small.en-q5_1 on the cpu scored 21.8 % on the
same clips, nearly all of it from repeated lines. `focus` made the larger
models 2 to 7 times worse on the sung-lead clips and only helped on babble, so
it is off by default.

sung lyrics over loud music stay hard for every model. stem separation
(demucs/uvr) would help but needs a separate neural model and runtime, which is
out of scope for a small dependency-free binary.

## ai

the ai is on by default and corrects the transcript **in place**: grey live
text becomes whisper's words, and the ai then fixes misheard words inside those
same paragraphs. it gets numbered lines and returns them numbered, and a
correction is only taken when it changes a few words, so a model that rewrites
or invents is ignored. the transcript file waits for the corrections, up to 30 s.
with `ai_mode = notes` the ai instead follows your instruction (like
"summarise as bullets") and writes the result in a pane under the transcript.

it needs [ollama](https://ollama.com). `install.sh` installs it and pulls the
model that suits the gpu; by hand:

```sh
ollama pull qwen3.5:9b
```

if ollama isn't running, earshot starts it (`llm_start`) and stops it on exit.

models live in numbered slots, `ai1` to `ai9`, and `ai` picks one (0 is off).
the `a` key steps through them while running. a slot can be:

```
ai1 = gemma3:4b                                    # an ollama model
ai2 = llama-3.1-8b-instant @ https://api.groq.com/openai/v1/chat/completions
ai3 = ~/models/mistral-7b-instruct.q4_k_m.gguf     # any gguf file, imported into ollama on first use
```

for hosted endpoints, set `EARSHOT_API_KEY` in your environment. the key goes
to curl through a private temp file, so it never shows in `ps`.

### which ai model

scored on 30 lines: misheard words with known answers (homophones like
by/buy and here/hear, "tour software" for tor, "mike" for mic) and odd lines
from real sessions that should be left alone. each model got earshot's fix
prompt and edit guard. lower "broke" is better; a model that invents is worse
than one that does nothing.

| model | size | wrong words fixed | broke or invented | per batch, rtx 4070 |
|---|---|---|---|---|
| **qwen3.5:9b** | 6.6 gb | **8 of 14** | 2, both arguable (".onion", "right to work") | 1.6 s |
| qwen3:8b | 5.2 gb | 6 of 14 | 1 | 1.3 s |
| qwen2.5:7b | 4.7 gb | 6 of 14 | 6 | 1.1 s |
| gemma3:4b | 3.4 gb | 0 | 0 | 0.8 s |
| llama3.1:8b | 4.9 gb | 1 | 8 ("gemma 3 on alama" became "gemini 3 on alexa") | 1.2 s |

`qwen3.5:9b` is the pick on an 8 gb gpu (`ollama pull qwen3.5:9b`, then put it
in a slot). qwen3 models think before answering, which took over a minute per
batch; earshot turns that off for ollama. `gemma3:4b` stays the default slot
because it fits smaller gpus and never makes things worse. keep whisper on a
different gpu or the cpu if the ai model and whisper together do not fit in
video memory, or ollama slows down by moving layers to the cpu. no ai model can
bring back words whisper never heard.

with the ai off (`ai = 0` or an empty instruction), nothing leaves your machine
and nothing extra runs.

## testing

```sh
cmake -B build -DEARSHOT_TESTS=on && cmake --build build -j && ctest --test-dir build
cmake -B build-asan -DEARSHOT_SANITIZE="address;undefined" -DEARSHOT_TESTS=on
cmake -B build-tsan -DEARSHOT_SANITIZE=thread -DEARSHOT_TESTS=on
```

`--file` runs a wav through the same pipeline as live capture, faster than
real time. clips saved with `c` or `r` make good test cases for audio that
trips whisper up.

## limits

- an app with several audio streams is captured through its first one. earshot re-attaches when the app restarts.
- two-channel capture, settings and model switching are built and cross-compiled for windows but have only been run on linux.
- on windows, app capture needs build 20348 or newer. the windows tui was checked under wine, whose terminal console mangles the layout; real windows terminal or conhost is untested.
