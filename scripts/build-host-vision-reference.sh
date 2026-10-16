#!/usr/bin/env bash
# CPU reference for the vision model mode (docs/PORTING.md "Vision"): the same
# llama.cpp pin and patches built for this Mac (CPU backend only, no Metal,
# BLAS or Accelerate) with libmtmd, running ps5/native-compute/ggml-vision.cpp
# with PS5LM_HOST_REFERENCE on the fixed test image. Writes, for the title's
# build, build/native-VARIANT/gen/{model-reference.h,vision-reference.h,
# vision-embd-ref.bin,vision-trace-ref.bin}. Never contacts a console.
#
#   scripts/build-host-vision-reference.sh [vl4b|vl8b]          CPU reference
#   PS5LM_VISION_METAL=1 scripts/build-host-vision-reference.sh  also the Metal stand-in,
#       compared with that reference (a check of the runner, not of the PS5 GPU)
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
llama="$root/third_party/llama.cpp"
variant="${1:-vl4b}"
case "$variant" in
    vl4b) flag=-DPS5LM_MODEL_QWEN3VL4B; model="$root/models/Qwen3VL-4B-Instruct-Q4_K_M.gguf"
          mmproj="$root/models/mmproj-Qwen3VL-4B-Instruct-F16.gguf" ;;
    vl8b) flag=-DPS5LM_MODEL_QWEN3VL8B; model="$root/models/Qwen3VL-8B-Instruct-Q4_K_M.gguf"
          mmproj="$root/models/mmproj-Qwen3VL-8B-Instruct-F16.gguf" ;;
    *) echo "Unknown variant: $variant" >&2; exit 2 ;;
esac
image="$root/testdata/vision/hello-shapes-448.png"
[ "$(git -C "$llama" rev-parse HEAD)" = 552f18f912a32ea86edf82e2b76431cb7131538d ]
for f in "$model" "$mmproj" "$image"; do [ -f "$f" ] || { echo "Missing $f" >&2; exit 2; }; done

build_tree() { # $1 work dir, $2 ON|OFF for Metal
    local work="$1" metal="$2"
    if [ ! -f "$work/.patched" ]; then
        rm -rf "$work/source"
        mkdir -p "$work/source"
        git -C "$llama" archive HEAD | tar -x -C "$work/source"
        for p in "$root"/patches/*.patch "$root"/patches/ggml-vulkan/*.patch; do
            [ -e "$p" ] || continue
            patch --silent -d "$work/source" -p1 < "$p"
        done
        touch "$work/.patched"
    fi
    cmake -S "$work/source" -B "$work/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DGGML_METAL="$metal" -DGGML_METAL_EMBED_LIBRARY="$metal" \
        -DGGML_BLAS=OFF -DGGML_ACCELERATE=OFF \
        -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF -DGGML_LLAMAFILE=OFF -DGGML_BACKEND_DL=OFF \
        -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_TOOLS=OFF \
        -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_APP=OFF -DLLAMA_OPENSSL=OFF \
        -DLLAMA_BUILD_MTMD=ON -DLLAMA_SUBPROCESS=OFF > "$work/cmake.log"
    cmake --build "$work/cmake" --target llama ggml-cpu mtmd -j 8 > "$work/build.log"
    local libs=("$work/cmake/tools/mtmd/libmtmd.a" "$work/cmake/src/libllama.a" "$work/cmake/ggml/src/libggml.a"
                "$work/cmake/ggml/src/libggml-cpu.a")
    [ "$metal" = ON ] && libs+=("$work/cmake/ggml/src/ggml-metal/libggml-metal.a"
                                 "$work/cmake/ggml/src/ggml-blas/libggml-blas.a")
    while IFS= read -r a; do libs+=("$a"); done < <(find "$work/cmake/vendor" -name '*.a' 2>/dev/null | sort)
    libs+=("$work/cmake/ggml/src/libggml-base.a")
    local existing=()
    for l in "${libs[@]}"; do [ -f "$l" ] && existing+=("$l"); done
    local frameworks=()
    [ "$metal" = ON ] && frameworks=(-framework Metal -framework Foundation -framework MetalKit)
    c++ -std=c++17 -O2 -Wall -Wextra -DPS5LM_HOST_REFERENCE "$flag" \
        -I "$work/source/ggml/include" -I "$work/source/include" -I "$work/source/tools/mtmd" \
        -I "$root/ps5/native-compute" "$root/ps5/native-compute/ggml-vision.cpp" \
        "${existing[@]}" ${frameworks[@]+"${frameworks[@]}"} -o "$work/vision-$variant"
}

cpu="$root/build/host-llama-vision"
build_tree "$cpu" OFF
run="$root/test-results/local/vision-reference/$variant-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$run"
"$cpu/vision-$variant" ref "$model" "$mmproj" "$image" "$run" > "$run/output.log" 2> "$run/llama.log"
gen="$root/build/native-$variant/gen"
mkdir -p "$gen"
cp "$run/model-reference.h" "$run/vision-reference.h" "$gen/"
cp "$run/embd.bin" "$gen/vision-embd-ref.bin"
cp "$run/trace.bin" "$gen/vision-trace-ref.bin"
python3 - "$run" "$model" "$mmproj" "$image" <<'PY'
import hashlib, json, pathlib, sys
run = pathlib.Path(sys.argv[1])
def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 24), b''):
            h.update(b)
    return h.hexdigest()
line = [l for l in (run / 'output.log').read_text().splitlines() if l.startswith('PS5LM_VISION_REFERENCE ')][-1]
gaps = [l for l in (run / 'output.log').read_text().splitlines() if l.startswith('PS5LM_VISION_GAPS ')][-1]
files = {pathlib.Path(p).name: {'bytes': pathlib.Path(p).stat().st_size, 'sha256': sha(p)} for p in sys.argv[2:5]}
report = {'files': files, 'llama_commit': '552f18f912a32ea86edf82e2b76431cb7131538d',
          'backend': 'host CPU (no Metal/BLAS/Accelerate)', 'embd_sha256': sha(run / 'embd.bin'),
          'reference': json.loads(line.split(' ', 1)[1]), 'gaps': json.loads(gaps.split(' ', 1)[1])}
(run / 'reference.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
PY
echo "Reference: $gen (evidence $run)"
if [ "${PS5LM_VISION_METAL:-0}" = 1 ]; then
    metal="$root/build/host-llama-vision-metal"
    build_tree "$metal" ON
    mrun="$root/test-results/local/vision-metal-smoke/$variant-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$mrun"
    "$metal/vision-$variant" gpu "$model" "$mmproj" "$image" "$run" "$mrun" > "$mrun/output.log" 2> "$mrun/llama.log"
    grep -E '^PS5LM_VISION_(EMBD_CHECK|REFERENCE)' "$mrun/output.log"
    echo "Metal stand-in: $mrun"
fi
