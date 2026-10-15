#!/usr/bin/env bash
# Local-only compiler-rt subset for the native RADV title; no Darwin objects.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$root/scripts/env.sh"
version=21.1.8
hash=dd54ae21aee1780fac59445b51ebff601ad016b31ac3a7de3b21126fd3ccb229
cache="$root/.deps/compiler-rt"
archive="$cache/compiler-rt-$version.src.tar.xz"
src="$cache/compiler-rt-$version.src"
work="$root/build/ps5-builtins"
sdk="${PS5_NATIVE_SDK:-$root/.deps/PS5_Vulkan/.deps/native/ps5-payload-sdk}"
cc="${PS5_CLANG:-$("${LLVM_CONFIG:?LLVM_CONFIG is required}" --bindir)/clang}"
bindir="$(dirname -- "$cc")"
mkdir -p "$cache" "$work/obj"
if [ ! -f "$archive" ]; then
    curl --fail --location --retry 2 "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/compiler-rt-$version.src.tar.xz" -o "$archive.download"
    mv "$archive.download" "$archive"
fi
python3 - "$archive" "$hash" <<'PY'
import hashlib, pathlib, sys
actual = hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest()
if actual != sys.argv[2]:
    raise SystemExit(f"compiler-rt source hash mismatch: {actual}")
PY
# Re-extract the verified source, rather than accepting a modified cached tree.
tar -xf "$archive" -C "$cache"
objects=()
# The final section-collected native link needs only emulated TLS and CPUID.
for name in emutls cpu_model/x86; do
    obj="$work/obj/${name//\//_}.o"
    "$cc" -target x86_64-sie-ps5 -isystem "$sdk/target/include" \
        -O2 -fPIC -fno-stack-protector -fno-plt -femulated-tls \
        -fvisibility=hidden -fvisibility-nodllstorageclass=default \
        -ffunction-sections -fdata-sections -DNDEBUG \
        -c "$src/lib/builtins/$name.c" -o "$obj"
    objects+=("$obj")
done
rm -f "$work/libclang_rt.builtins-ps5-x86_64.a"
"$bindir/llvm-ar" rcsD "$work/libclang_rt.builtins-ps5-x86_64.a" "${objects[@]}"
"$bindir/llvm-readobj" --file-headers "$work/libclang_rt.builtins-ps5-x86_64.a" > "$work/architecture.txt"
"$bindir/llvm-nm" --defined-only "$work/libclang_rt.builtins-ps5-x86_64.a" > "$work/defined-symbols.txt"
python3 - "$work" "$archive" "$cc" "$sdk" <<'PY'
import hashlib, json, pathlib, re, subprocess, sys
work, archive, cc, sdk = map(pathlib.Path, sys.argv[1:])
formats = re.findall(r'^Format: (.+)$', (work / 'architecture.txt').read_text(), re.M)
if formats != ['elf64-x86-64'] * 2:
    raise SystemExit(f'Unexpected builtin object formats: {formats}')
symbols = (work / 'defined-symbols.txt').read_text()
for name in ('__emutls_get_address', '__cpu_model', '__cpu_indicator_init'):
    if not re.search(r'\b' + name + r'$', symbols, re.M):
        raise SystemExit(f'Missing real compiler-rt implementation: {name}')
lib = work / 'libclang_rt.builtins-ps5-x86_64.a'
manifest = {
    'upstream': 'https://github.com/llvm/llvm-project/releases/tag/llvmorg-21.1.8',
    'version': '21.1.8', 'source_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'target': 'x86_64-sie-ps5', 'sdk': str(sdk),
    'sdk_revision': (sdk / '.ps5-sdk-revision').read_text().strip(),
    'build_script_sha256': hashlib.sha256((work.parents[1] / 'scripts/build-ps5-builtins.sh').read_bytes()).hexdigest(),
    'compiler': subprocess.check_output([str(cc), '--version'], text=True),
    'sources': ['emutls.c', 'cpu_model/x86.c'],
    'archive_sha256': hashlib.sha256(lib.read_bytes()).hexdigest(),
    'hardware_verified': False,
}
(work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(lib)
PY
