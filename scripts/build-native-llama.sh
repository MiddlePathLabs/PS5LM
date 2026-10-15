#!/usr/bin/env bash
# Local static-library build only; no console access or payload CRT linkage.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$root/scripts/env.sh"
upstream="$root/.deps/PS5_Vulkan"
sdk="${PS5_NATIVE_SDK:-$upstream/.deps/native/ps5-payload-sdk}"
export PS5_PAYLOAD_SDK="$sdk"
export PS5_CLANG="${PS5_CLANG:-$("$LLVM_CONFIG" --bindir)/clang}"
llama="$root/third_party/llama.cpp"
work="$root/build/native-llama"
[ "$(git -C "$llama" rev-parse HEAD)" = 552f18f912a32ea86edf82e2b76431cb7131538d ]
[ "$(cat "$sdk/.ps5-sdk-revision")" = b83202be73e930050e00de0f1b4d9ec46c0391bf ]
# Build a source export: never modify the llama.cpp dependency working tree.
mkdir -p "$work/source"
git -C "$llama" archive HEAD | tar -x -C "$work/source"
for p in "$root"/patches/*.patch "$root"/patches/ggml-vulkan/*.patch; do
    [ -e "$p" ] || continue
    patch --silent -d "$work/source" -p1 < "$p"
done
vulkan="$(brew --prefix vulkan-headers)"
spirv="$(brew --prefix spirv-headers)"
flags="-fvisibility-nodllstorageclass=default -isysroot $sdk -isystem $sdk/target/include/c++/v1 -isystem $sdk/target/include -fno-stack-protector -fno-plt -femulated-tls -fdenormal-fp-math=ieee -ffunction-sections -fdata-sections"
cmake -S "$work/source" -B "$work/cmake" -G Ninja \
    -DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_C_COMPILER="$PS5_CLANG" -DCMAKE_C_COMPILER_TARGET=x86_64-sie-ps5 \
    -DCMAKE_CXX_COMPILER="$PS5_CLANG" -DCMAKE_CXX_COMPILER_TARGET=x86_64-sie-ps5 \
    -DCMAKE_AR="$(dirname "$PS5_CLANG")/llvm-ar" \
    -DCMAKE_RANLIB="$(dirname "$PS5_CLANG")/llvm-ranlib" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_CXX_FLAGS="$flags -fexceptions -frtti -I$spirv/include" \
    -DMATH_LIBRARY=SceLibcInternal -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=OFF \
    -DGGML_CPU_ALL_VARIANTS=OFF -DGGML_OPENMP=OFF -DGGML_CCACHE=OFF \
    -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON \
    -DGGML_AVX512=OFF -DGGML_LLAMAFILE=OFF -DGGML_VULKAN=ON \
    -DVulkan_INCLUDE_DIR="$vulkan/include" \
    -DVulkan_LIBRARY="${RADV_ARCHIVE:-$root/build/native-compute/libvulkan_radeon-ps5.a}" \
    -DVulkan_GLSLC_EXECUTABLE="$(command -v glslc)" \
    -DSPIRV-Headers_DIR="$spirv/share/cmake/SPIRV-Headers" \
    -DLLAMA_BUILD_COMMIT=552f18f912a32ea86edf82e2b76431cb7131538d \
    -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_TOOLS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_APP=OFF \
    -DLLAMA_OPENSSL=OFF
cmake --build "$work/cmake" --target llama ggml-vulkan ggml-cpu -j 4
python3 - "$root" "$work" <<'PY'
import hashlib, json, pathlib, sys
root, work = map(pathlib.Path, sys.argv[1:])
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
libraries = [work / 'cmake/src/libllama.a', *sorted((work / 'cmake/ggml/src').rglob('libggml*.a'))]
shaders = sorted((work / 'cmake/ggml/src/ggml-vulkan/vulkan-shaders.spv').glob('*.spv'))
if not shaders:
    raise SystemExit('No host-generated Vulkan shaders')
manifest = {
    'llama_commit': '552f18f912a32ea86edf82e2b76431cb7131538d',
    'gpu_executed': False,
    'libraries': {str(p.relative_to(work)): sha(p) for p in libraries},
    'patches': {str(p.relative_to(root)): sha(p) for p in
               [*sorted((root / 'patches').glob('*.patch')),
                *sorted((root / 'patches/ggml-vulkan').glob('*.patch'))]},
    'shader_count': len(shaders),
    'shader_digest': hashlib.sha256(''.join(p.name + ':' + sha(p) + '\n' for p in shaders).encode()).hexdigest(),
}
(work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY
