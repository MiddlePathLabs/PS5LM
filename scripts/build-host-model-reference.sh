#!/usr/bin/env bash
# CPU reference for the native model workload: the same llama.cpp pin and
# patches built for this Mac (CPU backend only, no Metal, BLAS or Accelerate),
# running ps5/native-compute/ggml-model.cpp with PS5LM_HOST_REFERENCE.
# Writes build/native-model/gen/model-reference.h. Never contacts a console.
#
#   scripts/build-host-model-reference.sh [MODEL.gguf]
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
llama="$root/third_party/llama.cpp"
work="$root/build/host-llama"
model="${1:-$root/models/Qwen3.5-0.8B-Q4_K_M.gguf}"
[ "$(git -C "$llama" rev-parse HEAD)" = 552f18f912a32ea86edf82e2b76431cb7131538d ]
[ -f "$model" ] || { echo "Missing model: $model" >&2; exit 2; }
rm -rf "$work/source"
mkdir -p "$work/source"
git -C "$llama" archive HEAD | tar -x -C "$work/source"
for p in "$root"/patches/*.patch "$root"/patches/ggml-vulkan/*.patch; do
    [ -e "$p" ] || continue
    patch --silent -d "$work/source" -p1 < "$p"
done
cmake -S "$work/source" -B "$work/cmake" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DGGML_METAL=OFF -DGGML_BLAS=OFF -DGGML_ACCELERATE=OFF \
    -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF -DGGML_LLAMAFILE=OFF -DGGML_BACKEND_DL=OFF \
    -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_TOOLS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_APP=OFF -DLLAMA_OPENSSL=OFF \
    > "$work/cmake.log"
cmake --build "$work/cmake" --target llama ggml-cpu -j 8 > "$work/build.log"
mkdir -p "$root/build/native-model/gen"
c++ -std=c++17 -O2 -Wall -Wextra -DPS5LM_HOST_REFERENCE \
    -I "$work/source/ggml/include" -I "$work/source/include" -I "$root/ps5/native-compute" \
    "$root/ps5/native-compute/ggml-model.cpp" \
    "$work/cmake/src/libllama.a" "$work/cmake/ggml/src/libggml.a" "$work/cmake/ggml/src/libggml-cpu.a" \
    "$work/cmake/ggml/src/libggml-base.a" -o "$work/model-reference"
run="$root/test-results/local/model-reference/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$run"
header="$root/build/native-model/gen/model-reference.h"
"$work/model-reference" "$model" "$header" > "$run/output.log" 2> "$run/llama.log"
cp "$header" "$run/model-reference.h"
python3 - "$run" "$model" <<'PY'
import hashlib, json, pathlib, sys
run, model = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
h = hashlib.sha256()
with model.open('rb') as f:
    for block in iter(lambda: f.read(1 << 24), b''):
        h.update(block)
line = [l for l in (run / 'output.log').read_text().splitlines() if l.startswith('PS5LM_MODEL_REFERENCE ')][-1]
report = {'model': model.name, 'model_bytes': model.stat().st_size, 'model_sha256': h.hexdigest(),
          'llama_commit': '552f18f912a32ea86edf82e2b76431cb7131538d', 'backend': 'host CPU (no Metal/BLAS/Accelerate)',
          'reference': json.loads(line.split(' ', 1)[1])}
(run / 'reference.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
PY
echo "Reference: $header (evidence $run)"
