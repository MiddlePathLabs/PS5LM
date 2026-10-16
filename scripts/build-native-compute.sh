#!/usr/bin/env bash
# Build/package only. Never contacts a console, starts a loader, or registers a title.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$root/scripts/env.sh"
upstream="$root/.deps/PS5_Vulkan"
mesa="$root/.deps/PS5_Mesa"
mode="${1:-compute}"
case "$mode" in
    compute) work="$root/build/native-compute" ;;
    ggml-ops) work="$root/build/native-ggml-ops" ;;
    model) work="$root/build/native-model" ;;
    moe) work="$root/build/native-moe" ;;   # the model mode with the small MoE (PS5LM_MODEL_MOE)
    server) work="$root/build/native-server" ;;            # llama.cpp's server, Granite MoE
    server-qwen) work="$root/build/native-server-qwen" ;;  # llama.cpp's server, Qwen3.5 0.8B
    # The MoE climb (docs/PORTING.md "Larger MoE models"): model mode and server mode per step.
    moe3b|olmoe|qwen3moe) work="$root/build/native-$mode" ;;
    server-moe3b|server-olmoe|server-qwen3moe) work="$root/build/native-$mode" ;;
    # SSD streaming (docs/PORTING.md "SSD streaming"): the storage/mmap/CPU probe.
    stream-probe) work="$root/build/native-$mode" ;;
    *) echo "Usage: $0 [compute|ggml-ops|model|moe|server|server-qwen|moe3b|olmoe|qwen3moe|server-moe3b|server-olmoe|server-qwen3moe|stream-probe]" >&2; exit 2 ;;
esac
# Which model a model or server build loads: its define and its file (checked below).
variant_flag=(); want=Qwen3.5-0.8B-Q4_K_M.gguf
case "$mode" in
    moe|server) variant_flag=(-DPS5LM_MODEL_MOE); want=granite-3.1-1b-a400m-instruct-Q4_K_M.gguf ;;
    moe3b|server-moe3b) variant_flag=(-DPS5LM_MODEL_GRANITE3B); want=granite-3.1-3b-a800m-instruct-Q4_K_M.gguf ;;
    olmoe|server-olmoe) variant_flag=(-DPS5LM_MODEL_OLMOE); want=OLMoE-1B-7B-0125-Instruct-Q4_K_M.gguf ;;
    stream-probe) variant_flag=(-DPS5LM_MODEL_OLMOE -DPS5LM_STREAM_PROBE); want=OLMoE-1B-7B-0125-Instruct-Q4_K_M.gguf ;;
    qwen3moe|server-qwen3moe) variant_flag=(-DPS5LM_MODEL_QWEN3MOE); want=Qwen_Qwen3-30B-A3B-Instruct-2507-IQ2_M.gguf ;;
esac
model_files=(Qwen3.5-0.8B-Q4_K_M.gguf granite-3.1-1b-a400m-instruct-Q4_K_M.gguf
    granite-3.1-3b-a800m-instruct-Q4_K_M.gguf OLMoE-1B-7B-0125-Instruct-Q4_K_M.gguf
    Qwen_Qwen3-30B-A3B-Instruct-2507-IQ2_M.gguf)
app="$work/PPSA99041"
sdk="${PS5_NATIVE_SDK:-$upstream/.deps/native/ps5-payload-sdk}"
archive="${RADV_ARCHIVE:-$root/build/radv-preflight/radv-release/src/amd/vulkan/libvulkan_radeon.a}"
cc="${PS5_CLANG:-$("${LLVM_CONFIG:?LLVM_CONFIG is required}" --bindir)/clang}"
cxx="${CXX:-clang++}"
llvm="$(dirname -- "$cc")"
export PS5_CLANG="$cc" PS5_PAYLOAD_SDK="$sdk"
native="$upstream/tooling/native"
[ "$(git -C "$upstream" rev-parse HEAD)" = f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed ]
[ "$(git -C "$mesa" rev-parse HEAD)" = 1ba9874f478657ccf6da72ce55f0260164cc0bd0 ]
[ "$(cat "$sdk/.ps5-sdk-revision")" = b83202be73e930050e00de0f1b4d9ec46c0391bf ]
[ -f "$archive" ] || { echo "Missing RADV archive: $archive" >&2; exit 2; }
mkdir -p "$work/obj" "$work/gen" "$work/stubs" "$work/host" "$app/sce_sys" "$app/sce_module" "$app/licenses"
PS5_NATIVE_SDK="$sdk" bash "$root/scripts/build-ps5-builtins.sh"
builtins="$root/build/ps5-builtins/libclang_rt.builtins-ps5-x86_64.a"
compile() {
    sh "$upstream/tooling/prospero-clang18" -O2 -Wall -Wextra -Werror \
        -ffunction-sections -fdata-sections "$@"
}
# Keep the prepared archive intact. Recompile only its Linux proc-fd diagnostic
# object with the versioned native portability patch and its exact Meson flags.
mkdir -p "$work/compat"
patch --silent -o "$work/compat/ac_gpu_info.c" "$mesa/src/amd/common/ac_gpu_info.c" \
    "$root/patches/native/0002-no-proc-fd-diagnostic.patch"
python3 - "$root" "$work" "$archive" "$llvm" <<'PY'
import hashlib, json, os, pathlib, shlex, shutil, subprocess, sys
root, work, base, llvm = map(lambda p: pathlib.Path(p).resolve(), sys.argv[1:])
commands = pathlib.Path(os.environ.get('RADV_COMPILE_COMMANDS', base.parents[3] / 'compile_commands.json'))
entry, = [e for e in json.loads(commands.read_text()) if e['file'].endswith('/src/amd/common/ac_gpu_info.c')]
directory = pathlib.Path(entry['directory'])
source = (directory / entry['file']).resolve()
if source != root / '.deps/PS5_Mesa/src/amd/common/ac_gpu_info.c':
    raise SystemExit('Diagnostic compile recipe is not from the pinned prepared Mesa tree')
args = entry.get('arguments') or shlex.split(entry['command'])
args = list(args)
obj = work / 'compat/ac_gpu_info.c.o'
if args.count(entry['file']) != 1:
    raise SystemExit('Unexpected Meson compile recipe')
args[args.index(entry['file'])] = str(work / 'compat/ac_gpu_info.c')
for option, value in (('-o', obj), ('-MF', str(obj) + '.d'), ('-MQ', obj)):
    args[args.index(option) + 1] = str(value)
args += ['-iquote', str(source.parent)]
subprocess.run(args, cwd=directory, check=True)
derived = work / 'libvulkan_radeon-ps5.a'
shutil.copyfile(base, derived)
subprocess.run([str(llvm / 'llvm-ar'), 'rcsD', str(derived), str(obj)], check=True)
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
report = {'base_archive': str(base), 'base_sha256': sha(base), 'derived_sha256': sha(derived),
          'patch': 'patches/native/0002-no-proc-fd-diagnostic.patch',
          'patch_sha256': sha(root / 'patches/native/0002-no-proc-fd-diagnostic.patch'),
          'compile_directory': str(directory), 'compile_command': args,
          'reason': 'No Linux proc-fd/readlink diagnostic in a native PS5 title; no GPU computation changes.'}
(work / 'driver-compat.json').write_text(json.dumps(report, indent=2) + '\n')
PY
archive="$work/libvulkan_radeon-ps5.a"
patch --silent -o "$work/host/native_app_builder.cpp" "$native/native_app_builder.cpp" \
    "$root/patches/native/0003-native-import-library-override.patch"
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror -I "$native" \
    "$work/host/native_app_builder.cpp" "$native/self_container.cpp" \
    "$native/elf_object.cpp" "$native/sce_module_writer.cpp" -lz -o "$work/host/ps5-native-tool"
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror "$native/libc_builder.cpp" -o "$work/host/libc-builder"
tool="$work/host/ps5-native-tool"
glslangValidator -V --target-env vulkan1.0 "$root/ps5/native-compute/compute.comp" -o "$work/gen/compute.spv"
spirv-val --target-env vulkan1.0 "$work/gen/compute.spv"
glslangValidator -V --target-env vulkan1.0 --vn ps5lm_compute_spv \
    "$root/ps5/native-compute/compute.comp" -o "$work/gen/compute_spv.h"
extra_objects=()
extra_libraries=()
compile_flags=()
server=0
case "$mode" in server*) server=1 ;; esac
model_mode=0
case "$mode" in model|moe|moe3b|olmoe|qwen3moe|stream-probe) model_mode=1 ;; esac
if [ "$mode" != compute ]; then
    PS5LM_LLAMA_SERVER=$server PS5_NATIVE_SDK="$sdk" RADV_ARCHIVE="$archive" bash "$root/scripts/build-native-llama.sh"
    llama="$root/build/native-llama"
    compile_flags+=(-DPS5LM_GGML_OPS)
    runner=ggml-ops
    if [ "$server" = 1 ]; then
        # The model mode's runtime and caps, with llama.cpp's server as the workload.
        compile_flags+=(-DPS5LM_GGML_MODEL -DPS5LM_GGML_SERVER ${variant_flag[@]+"${variant_flag[@]}"})
        runner=ggml-server
    elif [ "$mode" = stream-probe ]; then
        # No model load, so no CPU reference; the probe reads the OLMoE and Qwen3 files.
        compile_flags+=(-DPS5LM_GGML_MODEL ${variant_flag[@]+"${variant_flag[@]}"})
        runner=ggml-stream-probe
    elif [ "$model_mode" = 1 ]; then
        # The CPU reference comes first: scripts/build-host-model-reference.sh.
        [ -f "$work/gen/model-reference.h" ] || { echo "Missing $work/gen/model-reference.h" >&2; exit 2; }
        compile_flags+=(-DPS5LM_GGML_MODEL ${variant_flag[@]+"${variant_flag[@]}"})
        runner=ggml-model
    fi
    compile -std=c11 ${compile_flags[@]+"${compile_flags[@]}"} -I "$mesa/include" -I "$root/ps5/native-compute" \
        -c "$root/ps5/native-compute/ggml-dispatch.c" -o "$work/obj/ggml-dispatch.o"
    compile -std=c++17 -fexceptions -frtti ${compile_flags[@]+"${compile_flags[@]}"} \
        -I "$llama/source/ggml/include" -I "$llama/source/include" \
        -I "$root/ps5/native-compute" -I "$work/gen" \
        -c "$root/ps5/native-compute/$runner.cpp" -o "$work/obj/$runner.o"
    extra_objects+=("$work/obj/ggml-dispatch.o" "$work/obj/$runner.o")
    if [ "$server" = 1 ]; then
        # Only the libc and socket functions the server's link needs that no
        # module a title may import provides (net-compat.c), kept local.
        compile -std=c11 -c "$root/ps5/native-compute/net-compat.c" -o "$work/obj/net-compat.o"
        extra_objects+=("$work/obj/net-compat.o")
        server_libraries=("$llama/cmake/tools/server/libllama-server-impl.a"
            "$llama/cmake/tools/server/libserver-context.a" "$llama/cmake/common/libllama-common.a"
            "$llama/cmake/common/libllama-common-base.a" "$llama/cmake/tools/mtmd/libmtmd.a"
            "$llama/cmake/tools/ui/libllama-ui.a" "$llama/cmake/vendor/cpp-httplib/libcpp-httplib.a"
            "$llama/cmake/vendor/hash/libvendor-hash.a")
    fi
    extra_libraries+=(--start-group ${server_libraries[@]+"${server_libraries[@]}"} "$llama/cmake/src/libllama.a" "$llama/cmake/ggml/src/libggml.a"
        "$llama/cmake/ggml/src/libggml-cpu.a" "$llama/cmake/ggml/src/ggml-vulkan/libggml-vulkan.a"
        "$llama/cmake/ggml/src/libggml-base.a" --end-group)
fi
compile -std=c11 -c "$root/ps5/native-compute/posix.c" -o "$work/obj/posix.o"
compile -std=c11 ${compile_flags[@]+"${compile_flags[@]}"} -I "$mesa/include" -I "$work/gen" -c "$root/ps5/native-compute/main.c" -o "$work/obj/main.o"
for name in app_crt app_cpp_runtime; do
    compile -std=c++20 -fno-exceptions -fno-rtti -c "$native/$name.cpp" -o "$work/obj/$name.o"
done
for pair in 'libSceAgc agc_canary_link_stub.c' 'libSceAgcDriver agc_driver_canary_link_stub.c'; do
    read -r library source <<< "$pair"
    compile -std=c11 -fPIC -c "$upstream/vendor/ps5/sdk/stubs/$source" -o "$work/obj/$library.o"
    "$sdk/bin/prospero-lld" --shared -soname "$library.prx" -o "$work/stubs/$library.so" "$work/obj/$library.o"
done
# Apply the one-line host-build override to a generated copy, not dependency sources.
patch --silent -o "$work/radv-link.sh" "$upstream/tools/radv-link.sh" "$root/patches/native/0001-target-builtins-override.patch"
source "$work/radv-link.sh"
export RADV_BUILTINS="$builtins"
radv_link_recipe "$upstream" "$sdk" "$archive"
radv_link_flags+=(--wrap=sceKernelAllocateDirectMemory --wrap=sceKernelReleaseDirectMemory --wrap=ps5p_heap_count_fallback)
radv_link_flags+=(--wrap=getenv --wrap=secure_getenv --wrap=fopen --wrap=freopen --wrap=open --wrap=mkdir)
radv_link_flags+=(--gc-sections --defsym=mkstemp=ps5lm_mkstemp)
if [ "$mode" != compute ]; then
    radv_link_flags+=(--wrap=abort --defsym=vkGetInstanceProcAddr=ps5lm_vkGetInstanceProcAddr
        --defsym=vkGetDeviceProcAddr=ps5lm_vkGetDeviceProcAddr
        --defsym=vkCmdCopyBuffer=vk_common_CmdCopyBuffer
        --defsym=vkGetPhysicalDeviceFeatures2=vk_common_GetPhysicalDeviceFeatures2)
fi
if [ "$model_mode" = 1 ] || [ "$server" = 1 ]; then
    radv_link_flags+=(--defsym=posix_madvise=ps5lm_posix_madvise)
fi
net_compat=(getaddrinfo freeaddrinfo getnameinfo accept4 isatty getpwuid timegm readlink link symlink pathconf remove)
if [ "$server" = 1 ]; then
    # ggml-server.cpp checks the one model load and the one context, and their caps.
    radv_link_flags+=(--wrap=llama_model_load_from_file --wrap=llama_init_from_model)
    # The platform's resolver refuses every lookup; net-compat.c's numeric one
    # replaces it, and binds the other names the server needs (all kept local).
    kept=()
    for flag in "${radv_link_flags[@]}"; do
        case $flag in --defsym=getaddrinfo=*|--defsym=freeaddrinfo=*) ;; *) kept+=("$flag") ;; esac
    done
    radv_link_flags=("${kept[@]}")
    for name in "${net_compat[@]}"; do
        radv_link_flags+=("--defsym=$name=ps5lm_$name")
    done
fi
# The PS5 target's nodllstorageclass policy overrides C visibility attributes.
# Localize wrappers explicitly, just as the upstream recipe does its bindings.
vk_local=''
if [ "$mode" != compute ]; then
    vk_local='vkGetInstanceProcAddr; vkGetDeviceProcAddr; vkCmdCopyBuffer; vkGetPhysicalDeviceFeatures2;'
fi
if [ "$model_mode" = 1 ] || [ "$server" = 1 ]; then
    vk_local="$vk_local posix_madvise;"
fi
if [ "$server" = 1 ]; then
    for name in "${net_compat[@]}"; do
        case $name in getaddrinfo|freeaddrinfo) ;; *) vk_local="$vk_local $name;" ;; esac
    done
fi
printf '{ local: __wrap_*; mkstemp; ps5lm_mkstemp; %s ps5lm_*; };\n' "$vk_local" > "$work/probe-local.map"
radv_link_flags+=(--version-script "$work/probe-local.map")
link() {
    "$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
        --version-script "$native/app-symbols.map" --exclude-libs=ALL -e _start \
        "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/main.o" "$work/obj/posix.o" \
        ${extra_objects[@]+"${extra_objects[@]}"} ${extra_libraries[@]+"${extra_libraries[@]}"} \
        "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" "$@" \
        --as-needed "$sdk/target/lib/libSceLibcInternal.so" "$sdk/target/lib/libkernel.so" \
        "$sdk/target/lib/libSceSystemService.so" "$sdk/target/lib/libSceVideoOut.so"
}
# Deliberately omit only builtins for a diagnostic link. Never package this output.
without_builtins=()
for input in "${radv_link_inputs[@]}"; do
    [ "$input" = "$builtins" ] || without_builtins+=("$input")
done
if link -o "$work/no-builtins.elf" "${without_builtins[@]}" > "$work/runtime-audit.log" 2>&1; then
    echo 'Runtime audit unexpectedly needed no builtins; review the selected source subset.' >&2
    exit 2
fi
link -o "$work/llvm-pie.elf" --Map="$work/link.map" --why-extract="$work/archive-extraction.tsv" \
    "${radv_link_inputs[@]}"
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
    --stub "$work/stubs/libSceAgc.so" --stub "$work/stubs/libSceAgcDriver.so" \
    --stub-library libSceSystemService=libSceShellCoreUtil \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
for copy in a b; do
    "$work/host/libc-builder" "$native/runtime/api-surface.txt" "$native/runtime/imports.txt" "$work/libc-$copy.raw.elf"
    "$tool" self --sign --in "$work/libc-$copy.raw.elf" --out "$work/libc-$copy.prx"
done
cmp "$work/libc-a.raw.elf" "$work/libc-b.raw.elf"
cmp "$work/libc-a.prx" "$work/libc-b.prx"
cp "$work/libc-a.prx" "$app/sce_module/libc.prx"
cp "$root/ps5/native-compute/param.json" "$app/sce_sys/param.json"
# Only the icon: no selection audio, backgrounds, or unnecessary runtime files.
cp "$upstream/sce_sys/icon0.png" "$app/sce_sys/icon0.png"
cp "$upstream/LICENSE" "$app/licenses/PS5-Vulkan-GPL-3.0.txt"
cp "$upstream/NOTICE.md" "$app/licenses/PS5-Vulkan-notices.txt"
cp "$root/.deps/compiler-rt/compiler-rt-21.1.8.src/LICENSE.TXT" "$app/licenses/compiler-rt.txt"
if [ "$mode" != compute ]; then
    cp "$llama/source/LICENSE" "$app/licenses/llama.cpp-MIT.txt"
fi
if [ "$server" = 1 ]; then
    cp "$llama/source/vendor/cpp-httplib/LICENSE" "$app/licenses/cpp-httplib-MIT.txt"
    sed -n '1,8p' "$llama/source/vendor/nlohmann/json.hpp" > "$app/licenses/nlohmann-json-MIT.txt"
fi
"$tool" self --inspect --file "$app/eboot.bin" > "$work/eboot-integrity.txt"
"$tool" self --inspect --file "$app/sce_module/libc.prx" > "$work/libc-integrity.txt"
"$tool" self --extract --file "$app/eboot.bin" --out "$work/extracted.elf"
"$tool" self --extract --file "$app/sce_module/libc.prx" --out "$work/libc-extracted.elf"
bash "$upstream/tools/inspect.sh" "$work/eboot.elf" > "$work/native-structure.txt"
if [ "$model_mode" = 1 ] || [ "$server" = 1 ]; then
    # Every object must name the same model: the title's setup check (main.c) and
    # the loader (ggml-model.cpp) once disagreed because a flag missed one of them.
    for object in "$work/obj/main.o" "$work/obj/$runner.o"; do
        strings "$object" | grep -qF "$want" || { echo "$object does not name $want" >&2; exit 2; }
        for other in "${model_files[@]}"; do
            [ "$other" = "$want" ] && continue
            # The streaming probe reads the Qwen3 file too (read-only, never loaded).
            [ "$mode" = stream-probe ] && [ "$object" = "$work/obj/$runner.o" ] && continue
            if strings "$object" | grep -qF "$other"; then echo "$object names $other" >&2; exit 2; fi
        done
    done
fi
python3 "$root/scripts/validate-native-compute.py" "$work" "$sdk" "$archive" "$llvm" "$root"
echo "Packaged $app.zip (local validation only; GPU NOT executed)."
echo 'Hardware order REQUIRED: PPSA99040 capability queries + observed clean exit, then PPSA99041.'
