#!/usr/bin/env bash
# Fetch a whisper.cpp ggml model from Hugging Face.
# usage: ./download-model.sh [tiny.en|base.en|small.en|medium.en|large-v3-turbo|...]
set -euo pipefail
name="${1:-base.en}"
dir="${XDG_DATA_HOME:-$HOME/.local/share}/earshot"
mkdir -p "$dir"
url="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$name.bin"
echo "downloading $url -> $dir/ggml-$name.bin"
curl -fL --progress-bar -o "$dir/ggml-$name.bin.part" "$url"
mv "$dir/ggml-$name.bin.part" "$dir/ggml-$name.bin"
echo "done. use: earshot -m $dir/ggml-$name.bin"
