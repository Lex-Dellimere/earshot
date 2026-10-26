#!/usr/bin/env bash
# earshot installer for linux and windows (git bash or msys2)
#
# installs whatever build tools are missing, builds earshot with gpu support when the machine has a
# gpu, finds or installs ollama, then picks the whisper and ai models that fit this machine's gpu,
# downloads them and writes a config that uses them. run it again any time; finished steps are skipped.
set -euo pipefail

usage() {
    cat <<'EOF'
usage: ./install.sh [options]
  -y, --yes       answer yes to every question
      --cpu       build without gpu support
      --gpu       build with gpu support even if no gpu was found
      --no-ai     skip ollama and the ai model
      --prefix D  install earshot into D/bin (default ~/.local on linux)
      --dry-run   show what would happen without changing anything
  -h, --help      show this help
EOF
}

YES=0 FORCE_CPU=0 FORCE_GPU=0 NO_AI=0 DRY=0 PREFIX=""
while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes) YES=1 ;;
        --cpu) FORCE_CPU=1 ;;
        --gpu) FORCE_GPU=1 ;;
        --no-ai) NO_AI=1 ;;
        --prefix) PREFIX="${2:?--prefix needs a folder}"; shift ;;
        --dry-run) DRY=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
    shift
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$ROOT/CMakeLists.txt" ] && [ -f "$ROOT/src/main.cpp" ] || { echo "run install.sh from the earshot source folder" >&2; exit 1; }

if [ -t 1 ]; then BOLD=$'\e[1m' DIM=$'\e[2m' RED=$'\e[31m' GREEN=$'\e[32m' YELLOW=$'\e[33m' RESET=$'\e[0m'
else BOLD="" DIM="" RED="" GREEN="" YELLOW="" RESET=""; fi
step() { printf '\n%s==> %s%s\n' "$BOLD" "$*" "$RESET"; }
info() { printf '    %s\n' "$*"; }
ok() { printf '    %s%s%s\n' "$GREEN" "$*" "$RESET"; }
warn() { printf '    %swarning:%s %s\n' "$YELLOW" "$RESET" "$*" >&2; }
die() { printf '%serror:%s %s\n' "$RED" "$RESET" "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# runs a command, or only shows it with --dry-run
run() {
    printf '    %s$ %s%s\n' "$DIM" "$*" "$RESET"
    [ "$DRY" = 1 ] || "$@"
}

# asks a yes or no question; without a terminal or with --yes the default is taken
ask() {
    local question="$1" default="${2:-y}" reply
    if [ "$YES" = 1 ] || [ ! -t 0 ]; then [ "$default" = y ]; return; fi
    read -r -p "    $question [$( [ "$default" = y ] && echo y/n || echo y/n )] " reply
    reply="${reply:-$default}"
    [[ "$reply" =~ ^[Yy] ]]
}

# ---------------------------------------------------------------- system

OS=linux
case "$(uname -s)" in
    Linux) ;;
    MINGW*|MSYS*|CYGWIN*) OS=windows ;;
    Darwin) die "macos is not supported: earshot captures audio with pipewire on linux and wasapi on windows" ;;
    *) die "unsupported system $(uname -s)" ;;
esac

SUDO=""
if [ "$OS" = linux ] && [ "$(id -u)" != 0 ]; then
    if have sudo; then SUDO=sudo; elif have doas; then SUDO=doas; fi
fi

CPUS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-4}")

ram_gb() {
    if [ -r /proc/meminfo ]; then
        awk '/MemTotal/ { printf "%d", $2 / 1048576 + 0.5 }' /proc/meminfo
    elif have powershell.exe; then
        powershell.exe -NoProfile -Command "[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)" 2>/dev/null | tr -d '\r'
    else
        echo 8
    fi
}

# ---------------------------------------------------------------- build tools on linux

PM=""
detect_pm() {
    for pm in apt-get dnf yum pacman zypper apk xbps-install emerge; do
        if have "$pm"; then PM="$pm"; return; fi
    done
}

# package names for each manager: base build tools, then what the vulkan build adds
packages() {
    local want="$1"
    case "$PM:$want" in
        apt-get:base) echo "build-essential cmake git pkg-config libpipewire-0.3-dev libx11-dev curl ca-certificates" ;;
        apt-get:vulkan) echo "libvulkan-dev glslc" ;;
        apt-get:mesa) echo "mesa-vulkan-drivers" ;;
        dnf:base|yum:base) echo "gcc-c++ make cmake git pkgconf-pkg-config pipewire-devel libX11-devel curl" ;;
        dnf:vulkan|yum:vulkan) echo "vulkan-loader-devel vulkan-headers glslc" ;;
        dnf:mesa|yum:mesa) echo "mesa-vulkan-drivers" ;;
        pacman:base) echo "base-devel cmake git pkgconf libpipewire libx11 curl" ;;
        pacman:vulkan) echo "vulkan-icd-loader vulkan-headers shaderc" ;;
        pacman:mesa) echo "vulkan-radeon vulkan-intel" ;;
        zypper:base) echo "gcc-c++ make cmake git pkgconf-pkg-config pipewire-devel libX11-devel curl" ;;
        zypper:vulkan) echo "vulkan-devel shaderc" ;;
        zypper:mesa) echo "Mesa-vulkan-device-select libvulkan_radeon libvulkan_intel" ;;
        apk:base) echo "build-base cmake git pkgconf pipewire-dev libx11-dev curl linux-headers" ;;
        apk:vulkan) echo "vulkan-loader-dev vulkan-headers shaderc" ;;
        apk:mesa) echo "mesa-vulkan-ati mesa-vulkan-intel" ;;
        xbps-install:base) echo "base-devel cmake git pkg-config pipewire-devel libX11-devel curl" ;;
        xbps-install:vulkan) echo "Vulkan-Headers vulkan-loader-devel shaderc" ;;
        xbps-install:mesa) echo "mesa-vulkan-radeon mesa-vulkan-intel" ;;
    esac
}

pm_install() {
    [ $# -gt 0 ] || return 0
    case "$PM" in
        apt-get) run $SUDO apt-get update -qq && run $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$@" ;;
        dnf|yum) run $SUDO "$PM" install -y -q "$@" ;;
        pacman) run $SUDO pacman -S --needed --noconfirm "$@" ;;
        zypper) run $SUDO zypper --non-interactive install --no-recommends "$@" ;;
        apk) run $SUDO apk add --no-cache "$@" ;;
        xbps-install) run $SUDO xbps-install -Sy "$@" ;;
        *) return 1 ;;
    esac
}

has_c_compiler() { have g++ || have c++ || have clang++; }
has_pc() { have pkg-config && pkg-config --exists "$1" 2>/dev/null; }

linux_missing_base() {
    have cmake || return 0
    has_c_compiler || return 0
    have git || return 0
    have curl || return 0
    has_pc libpipewire-0.3 || return 0
    has_pc x11 || return 0
    return 1
}

linux_missing_vulkan() {
    has_pc vulkan || return 0
    have glslc || return 0
    return 1
}

# a gpu shows up as a drm card from amd, nvidia or intel
linux_has_gpu() {
    local v
    for v in /sys/class/drm/card*/device/vendor; do
        [ -r "$v" ] || continue
        case "$(cat "$v")" in 0x1002|0x10de|0x8086) return 0 ;; esac
    done
    have lspci && lspci 2>/dev/null | grep -qiE 'vga|3d controller|display' && return 0
    return 1
}

linux_has_vulkan_driver() {
    local d
    for d in /usr/share/vulkan/icd.d /etc/vulkan/icd.d /usr/local/share/vulkan/icd.d; do
        compgen -G "$d/*.json" >/dev/null && return 0
    done
    return 1
}

setup_linux() {
    detect_pm
    step "build tools"
    if linux_missing_base; then
        [ -n "$PM" ] || die "no supported package manager found; install cmake, a c++ compiler, git, curl, pkg-config and the pipewire and x11 development files, then run this again"
        [ "$PM" = emerge ] && die "on gentoo, emerge dev-build/cmake dev-vcs/git media-video/pipewire x11-libs/libX11 net-misc/curl, then run this again"
        [ -n "$SUDO" ] || [ "$(id -u)" = 0 ] || die "installing packages needs root, and neither sudo nor doas was found"
        info "missing build tools; installing with $PM: $(packages base)"
        ask "install them now?" y || die "build tools are needed"
        # shellcheck disable=SC2046
        pm_install $(packages base)
    else
        ok "cmake, compiler, git, curl, pipewire and x11 headers are all here"
    fi

    GPU_BUILD=0
    if [ "$FORCE_CPU" = 0 ] && { [ "$FORCE_GPU" = 1 ] || linux_has_gpu; }; then
        step "gpu support"
        if linux_missing_vulkan; then
            info "a gpu was found; the vulkan build needs: $(packages vulkan)"
            if ask "install them for gpu speed?" y; then
                # shellcheck disable=SC2046
                pm_install $(packages vulkan) && GPU_BUILD=1 || warn "could not install them, building for the cpu"
            fi
        else
            GPU_BUILD=1
            ok "vulkan headers and glslc are here"
        fi
        if [ "$GPU_BUILD" = 1 ] && ! linux_has_vulkan_driver; then
            info "no vulkan driver is installed; amd and intel gpus use mesa's ($(packages mesa)), nvidia's comes with its driver"
            # shellcheck disable=SC2046
            ask "install the mesa vulkan drivers?" y && { pm_install $(packages mesa) || warn "could not install mesa vulkan drivers"; }
        fi
    fi

    if ! { have pw-cli && pw-cli info 0 >/dev/null 2>&1; } && ! { have pactl && pactl info 2>/dev/null | grep -qi pipewire; }; then
        warn "pipewire does not seem to be running; earshot captures audio through pipewire"
    fi
}

# ---------------------------------------------------------------- build tools on windows

MSYS=""
setup_windows() {
    step "build tools (msys2)"
    if [ -n "${MSYSTEM:-}" ] && have cygpath; then MSYS="$(cygpath -u "$(cygpath -w /)")"; fi
    [ -n "$MSYS" ] && [ -x "$MSYS/usr/bin/pacman" ] || MSYS=""
    if [ -z "$MSYS" ]; then
        for d in /c/msys64 /c/tools/msys64 "$HOME/msys64"; do
            [ -x "$d/usr/bin/pacman" ] && { MSYS="$d"; break; }
        done
    fi
    if [ -z "$MSYS" ]; then
        info "earshot builds with the mingw compiler from msys2, which is not installed"
        have winget || die "install msys2 from https://www.msys2.org, then run this again"
        ask "install msys2 with winget?" y || die "msys2 is needed to build earshot"
        run winget install -e --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements
        MSYS=/c/msys64
        [ "$DRY" = 1 ] || [ -x "$MSYS/usr/bin/pacman" ] || die "msys2 did not end up in c:\\msys64; run this again from the msys2 ucrt64 shell"
    fi
    ok "msys2 is at $MSYS"
    local pkgs="mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf git"
    GPU_BUILD=0
    if [ "$FORCE_CPU" = 0 ]; then
        pkgs="$pkgs mingw-w64-ucrt-x86_64-vulkan-devel mingw-w64-ucrt-x86_64-shaderc"
        GPU_BUILD=1
    fi
    info "making sure these are installed: $pkgs"
    run "$MSYS/usr/bin/bash.exe" -lc "pacman -S --needed --noconfirm $pkgs"
    export PATH="$MSYS/ucrt64/bin:$MSYS/usr/bin:$PATH"
}

# ---------------------------------------------------------------- build and install earshot

BIN=""
build_earshot() {
    local dir="$ROOT/build" args=(-DCMAKE_BUILD_TYPE=Release)
    if [ "$GPU_BUILD" = 1 ]; then dir="$ROOT/build-vk"; args+=(-DEARSHOT_VULKAN=ON); fi
    [ "$OS" = windows ] && args+=(-G Ninja)
    # a whisper.cpp checkout next to the source saves cloning it again
    [ -f "$ROOT/.deps/whisper.cpp/CMakeLists.txt" ] && args+=("-DFETCHCONTENT_SOURCE_DIR_WHISPER=$ROOT/.deps/whisper.cpp")
    step "building earshot ($( [ "$GPU_BUILD" = 1 ] && echo "gpu, vulkan" || echo cpu ))"
    info "the first build compiles whisper.cpp and can take several minutes"
    if ! run cmake -S "$ROOT" -B "$dir" "${args[@]}" || ! run cmake --build "$dir" -j "$CPUS"; then
        if [ "$GPU_BUILD" = 1 ]; then
            warn "the gpu build failed, building for the cpu instead"
            GPU_BUILD=0
            build_earshot
            return
        fi
        die "the build failed; the messages above say why"
    fi

    step "installing"
    local exe=earshot dest
    [ "$OS" = windows ] && exe=earshot.exe
    if [ -n "$PREFIX" ]; then dest="$PREFIX/bin"
    elif [ "$OS" = windows ]; then dest="$(cygpath -u "${LOCALAPPDATA:-$USERPROFILE/AppData/Local}")/Programs/earshot"
    else dest="$HOME/.local/bin"
    fi
    run mkdir -p "$dest"
    run cp "$dir/$exe" "$dest/$exe"
    BIN="$dest/$exe"
    [ "$DRY" = 1 ] && BIN="$dir/$exe"
    ok "earshot is at $dest/$exe"
    case ":$PATH:" in
        *":$dest:"*) ;;
        *) if [ "$OS" = windows ]; then
               info "to run it from anywhere, add $(cygpath -w "$dest") to your path"
               if have powershell.exe && ask "add it to your user path now?" y; then
                   run powershell.exe -NoProfile -Command "\$p=[Environment]::GetEnvironmentVariable('Path','User'); if (\$p -notlike '*$(cygpath -w "$dest")*') { [Environment]::SetEnvironmentVariable('Path', (\$p.TrimEnd(';') + ';$(cygpath -w "$dest")'), 'User') }"
               fi
           else
               info "$dest is not on your path; add this to your shell profile: export PATH=\"$dest:\$PATH\""
           fi ;;
    esac
}

# ---------------------------------------------------------------- pick models for this machine

WHISPER="" BEAM=1 GPU_ON=false GPU_DEVICE=0 AI_MODEL="" HARDWARE=""
choose_models() {
    step "choosing models for this machine"
    local best=-1 best_mb=0 igpu=-1 n kind mb name ram
    if [ "$GPU_BUILD" = 1 ] && [ -x "$BIN" ]; then
        while IFS=$'\t' read -r n kind mb name; do
            [ -n "$n" ] || continue
            info "gpu $n: $name ($kind, $(((mb + 512) / 1024)) gb)"
            if [ "$kind" = gpu ] && [ "$mb" -gt "$best_mb" ]; then best=$n best_mb=$mb; fi
            if [ "$kind" = igpu ] && [ "$igpu" -lt 0 ]; then igpu=$n; fi
        done < <("$BIN" --gpus 2>/dev/null | tr -d '\r')
    fi
    ram=$(ram_gb)
    if [ "$best" -ge 0 ] && [ "$best_mb" -ge 3000 ]; then
        GPU_ON=true GPU_DEVICE=$best BEAM=5
        HARDWARE="dedicated gpu with $(((best_mb + 512) / 1024)) gb"
        # whisper large-v3-turbo needs about 0.9 gb beside the ai model, measured on an rtx 4070
        if [ "$best_mb" -ge 7500 ]; then WHISPER=large-v3-turbo-q5_0 AI_MODEL=qwen3.5:9b
        elif [ "$best_mb" -ge 5000 ]; then WHISPER=large-v3-turbo-q5_0 AI_MODEL=qwen3.5:4b
        else WHISPER=small.en-q5_1 AI_MODEL=qwen3.5:4b
        fi
    elif [ "$igpu" -ge 0 ] || [ "$best" -ge 0 ]; then
        GPU_ON=true GPU_DEVICE=$(( best >= 0 ? best : igpu )) BEAM=5
        HARDWARE="integrated or small gpu"
        WHISPER=small.en-q5_1 AI_MODEL=qwen3.5:4b
    else
        HARDWARE="cpu only, $CPUS threads, $ram gb of memory"
        if [ "$CPUS" -ge 8 ]; then WHISPER=small.en-q5_1; else WHISPER=base.en; fi
        if [ "$ram" -ge 12 ]; then AI_MODEL=qwen3.5:4b; else AI_MODEL=""; fi
    fi
    [ "$NO_AI" = 1 ] && AI_MODEL=""
    ok "$HARDWARE"
    info "whisper: $WHISPER, beam $BEAM, $( [ "$GPU_ON" = true ] && echo "on gpu $GPU_DEVICE" || echo "on the cpu" )"
    info "ai: ${AI_MODEL:-off}"
}

# ---------------------------------------------------------------- ollama

OLLAMA="" OLLAMA_CUSTOM=0
find_ollama() {
    if have ollama; then OLLAMA="$(command -v ollama)"; return 0; fi
    local c
    local candidates=("$HOME/.local/bin/ollama" /usr/local/bin/ollama /usr/bin/ollama /opt/ollama/bin/ollama /snap/bin/ollama)
    if [ "$OS" = windows ]; then
        local lad
        lad="$(cygpath -u "${LOCALAPPDATA:-$USERPROFILE/AppData/Local}")"
        candidates=("$lad/Programs/Ollama/ollama.exe" "/c/Program Files/Ollama/ollama.exe")
    fi
    for c in "${candidates[@]}"; do
        if [ -x "$c" ]; then OLLAMA="$c" OLLAMA_CUSTOM=1; return 0; fi
    done
    return 1
}

install_ollama() {
    if [ "$OS" = windows ]; then
        if have winget; then
            run winget install -e --id Ollama.Ollama --accept-package-agreements --accept-source-agreements
        else
            local setup="${TEMP:-/tmp}/OllamaSetup.exe"
            run curl -fL -o "$setup" https://ollama.com/download/OllamaSetup.exe
            run "$setup"
        fi
    else
        info "using ollama's official installer, which may ask for your password"
        if [ "$DRY" = 1 ]; then info "$ curl -fsSL https://ollama.com/install.sh | sh"; else curl -fsSL https://ollama.com/install.sh | sh; fi
    fi
    find_ollama || [ "$DRY" = 1 ]
}

setup_ollama() {
    step "ollama (runs the ai that fixes misheard words)"
    if find_ollama; then
        ok "found $OLLAMA"
    else
        info "ollama was not found"
        local reply=""
        if [ "$YES" = 0 ] && [ -t 0 ]; then
            read -r -p "    path to ollama, enter to install it, or 'skip' for no ai: " reply
        fi
        if [ "$reply" = skip ]; then
            AI_MODEL=""; warn "skipping the ai"; return
        elif [ -n "$reply" ]; then
            reply="${reply/#\~/$HOME}"
            [ "$OS" = windows ] && have cygpath && reply="$(cygpath -u "$reply")"
            [ -d "$reply" ] && reply="$reply/ollama$( [ "$OS" = windows ] && echo .exe )"
            [ -x "$reply" ] || die "no ollama program at $reply"
            OLLAMA="$reply" OLLAMA_CUSTOM=1
        else
            install_ollama || { warn "ollama did not install; earshot will run without the ai"; AI_MODEL=""; return; }
        fi
    fi
    [ "$DRY" = 1 ] && return
    # pulling needs the server; start one for the duration of the install if none is running
    if ! "$OLLAMA" list >/dev/null 2>&1; then
        info "starting ollama"
        "$OLLAMA" serve >/dev/null 2>&1 &
        OLLAMA_PID=$!
        for _ in $(seq 30); do "$OLLAMA" list >/dev/null 2>&1 && break; sleep 1; done
    fi
    "$OLLAMA" list >/dev/null 2>&1 || { warn "ollama would not start; earshot will run without the ai"; AI_MODEL=""; return; }
    local pulled
    pulled="$("$OLLAMA" list 2>/dev/null | awk 'NR > 1 { print $1 }')" || true
    if grep -qx "$AI_MODEL" <<<"$pulled"; then
        ok "$AI_MODEL is already downloaded"
    else
        info "downloading $AI_MODEL"
        run "$OLLAMA" pull "$AI_MODEL" || { warn "could not download $AI_MODEL"; AI_MODEL=""; }
    fi
}
OLLAMA_PID=""
cleanup() { [ -n "$OLLAMA_PID" ] && kill "$OLLAMA_PID" 2>/dev/null || true; }
trap cleanup EXIT

# ---------------------------------------------------------------- whisper models and config

download_models() {
    step "whisper models"
    run "$BIN" --download "$WHISPER"
    run "$BIN" --download silero
}

write_config() {
    step "config"
    local config sets=() slot="" free="" n value
    config="$("$BIN" --help 2>/dev/null | sed -n 's/.*config file (default \(.*\))/\1/p' | tr -d '\r')" || true
    [ "$OS" = windows ] && [ -n "$config" ] && config="$(cygpath -u "$config")"
    sets+=(--set "model=$WHISPER" --set "beam=$BEAM" --set "gpu=$GPU_ON" --set "gpu_device=$GPU_DEVICE" --set focus=false)
    if [ -n "$AI_MODEL" ]; then
        # reuse the slot that already holds the model, otherwise take the first empty one
        for n in 1 2 3 4 5 6 7 8 9; do
            value="$(sed -n "s/^ai$n *= *//p" "$config" 2>/dev/null | head -1 | tr -d '\r')" || true
            if [ "$value" = "$AI_MODEL" ]; then slot=$n; break; fi
            [ -z "$value" ] && [ -z "$free" ] && free=$n
        done
        slot="${slot:-${free:-1}}"
        sets+=(--set "ai$slot=$AI_MODEL" --set "ai=$slot")
        [ "$OLLAMA_CUSTOM" = 1 ] && sets+=(--set "llm_start=$OLLAMA serve")
    else
        sets+=(--set ai=0)
    fi
    run "$BIN" "${sets[@]}" --save
}

# ---------------------------------------------------------------- main

printf '%searshot installer%s (%s)\n' "$BOLD" "$RESET" "$OS"
[ "$DRY" = 1 ] && info "dry run: nothing will be changed"
if [ "$OS" = linux ]; then setup_linux; else setup_windows; fi
build_earshot
choose_models
[ -n "$AI_MODEL" ] && setup_ollama
download_models
write_config

step "done"
ok "run: earshot"
info "keys: w whisper model, a ai model, s settings, ? all keys"
[ -n "$AI_MODEL" ] || info "the ai is off; run this again without --no-ai, or set it up later with the a key"
