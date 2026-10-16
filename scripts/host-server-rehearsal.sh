#!/usr/bin/env bash
# Rehearse the title's server mode on this Mac before a console run: the same
# llama.cpp pin and patches (patches/server included, hooks on), CPU backend,
# and ps5/native-compute/ggml-server.cpp with PS5LM_HOST_REHEARSAL. It serves
# on 127.0.0.1:8081 for WINDOW seconds (or until POST /ps5lm/shutdown) and
# prints the same PS5LM_SERVER_SUMMARY line. Never contacts a console.
#
#   scripts/host-server-rehearsal.sh [build]          build only
#   scripts/host-server-rehearsal.sh run [WINDOW]     build, then serve (default 60 s)
#   PS5LM_MODEL_VARIANT=dense ...                     Qwen3.5 0.8B instead of Granite
#   (also moe3b, olmoe, qwen3moe: the MoE climb; vl4b, vl8b: vision, with the mmproj)
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
llama="$root/third_party/llama.cpp"
work="$root/build/host-llama-server"
action="${1:-build}"
window="${2:-60}"
variant="${PS5LM_MODEL_VARIANT:-moe}"
case "$variant" in
    moe) model="$root/models/granite-3.1-1b-a400m-instruct-Q4_K_M.gguf"; vflags=(-DPS5LM_MODEL_MOE) ;;
    dense) model="$root/models/Qwen3.5-0.8B-Q4_K_M.gguf"; vflags=() ;;
    moe3b) model="$root/models/granite-3.1-3b-a800m-instruct-Q4_K_M.gguf"; vflags=(-DPS5LM_MODEL_GRANITE3B) ;;
    olmoe) model="$root/models/OLMoE-1B-7B-0125-Instruct-Q4_K_M.gguf"; vflags=(-DPS5LM_MODEL_OLMOE) ;;
    qwen3moe) model="$root/models/Qwen_Qwen3-30B-A3B-Instruct-2507-IQ2_M.gguf"; vflags=(-DPS5LM_MODEL_QWEN3MOE) ;;
    # Vision (docs/PORTING.md "Vision"): the text model and its mmproj.
    vl4b) model="$root/models/Qwen3VL-4B-Instruct-Q4_K_M.gguf"; vflags=(-DPS5LM_MODEL_QWEN3VL4B
          "-DPS5LM_HOST_MMPROJ_PATH=\"$root/models/mmproj-Qwen3VL-4B-Instruct-F16.gguf\"") ;;
    vl8b) model="$root/models/Qwen3VL-8B-Instruct-Q4_K_M.gguf"; vflags=(-DPS5LM_MODEL_QWEN3VL8B
          "-DPS5LM_HOST_MMPROJ_PATH=\"$root/models/mmproj-Qwen3VL-8B-Instruct-F16.gguf\"") ;;
    *) echo "Unknown PS5LM_MODEL_VARIANT: $variant" >&2; exit 2 ;;
esac
[ "$(git -C "$llama" rev-parse HEAD)" = 552f18f912a32ea86edf82e2b76431cb7131538d ]
[ -f "$model" ] || { echo "Missing model: $model" >&2; exit 2; }
rm -rf "$work/source"
mkdir -p "$work/source"
git -C "$llama" archive HEAD | tar -x -C "$work/source"
for p in "$root"/patches/*.patch "$root"/patches/ggml-vulkan/*.patch "$root"/patches/server/*.patch; do
    patch --silent -d "$work/source" -p1 < "$p"
done
cmake -S "$work/source" -B "$work/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DGGML_METAL=OFF -DGGML_BLAS=OFF -DGGML_ACCELERATE=OFF \
    -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF -DGGML_LLAMAFILE=OFF -DGGML_BACKEND_DL=OFF \
    -DLLAMA_BUILD_COMMON=ON -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_TOOLS=ON \
    -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_APP=OFF \
    -DLLAMA_BUILD_UI=OFF -DLLAMA_USE_PREBUILT_UI=OFF -DLLAMA_SUBPROCESS=OFF -DLLAMA_OPENSSL=OFF \
    -DCMAKE_CXX_FLAGS=-DPS5LM_SERVER_HOOKS > "$work/cmake.log"
cmake --build "$work/cmake" --target llama-server-impl ggml-cpu -j 8 > "$work/build.log"
c="$work/cmake"
libs=()
for l in tools/server/libllama-server-impl.a tools/server/libserver-context.a common/libllama-common.a \
        common/libllama-common-base.a tools/mtmd/libmtmd.a tools/ui/libllama-ui.a \
        vendor/cpp-httplib/libcpp-httplib.a vendor/hash/libvendor-hash.a src/libllama.a \
        ggml/src/libggml.a ggml/src/libggml-cpu.a ggml/src/libggml-base.a; do
    libs+=("$c/$l")
done
c++ -std=c++17 -O2 -Wall -Wextra -DPS5LM_HOST_REHEARSAL ${vflags[@]+"${vflags[@]}"} \
    -DPS5LM_HOST_MODEL_PATH="\"$model\"" -DPS5LM_SERVER_HOST='"127.0.0.1"' \
    -DPS5LM_SERVER_WINDOW_S="${window}u" \
    -I "$work/source/ggml/include" -I "$work/source/include" -I "$work/source/common" -I "$work/source/tools/mtmd" \
    -I "$root/ps5/native-compute" "$root/ps5/native-compute/ggml-server.cpp" \
    "${libs[@]}" -framework Foundation -o "$work/server-rehearsal-$variant"
echo "Built $work/server-rehearsal-$variant"
[ "$action" = run ] || exit 0
exec "$work/server-rehearsal-$variant"
