#!/usr/bin/env bash
# Build a query-only native title on macOS/Linux. Never deploys or contacts a PS5.
# Uses the upstream native converter/CRT and verified clean-room libc, not payload CRT.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "$root/scripts/env.sh"
revision=b8dc4a3fe2e05e826b037bd68824751c7468a6d7
upstream="$root/.deps/ps5-native-app-boilerplate"
work="$root/build/native-capabilities"
app="$work/PPSA99040"
sdk="$PS5_PAYLOAD_SDK"

if [ ! -d "$upstream" ]; then
    git clone --no-checkout https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git "$upstream"
    git -C "$upstream" checkout --detach "$revision"
fi
[ "$(git -C "$upstream" rev-parse HEAD)" = "$revision" ] || {
    echo "native boilerplate must be at $revision: $upstream" >&2; exit 2;
}
# Refuse modified dependency sources, including new files that could alter the build.
[ -z "$(git -C "$upstream" status --porcelain --untracked-files=normal)" ] || {
    echo "native boilerplate has local modifications: $upstream" >&2; exit 2;
}
[ -x "$sdk/bin/prospero-lld" ] || { echo "Run scripts/setup-sdk.sh first" >&2; exit 2; }
if [ -n "${PS5_CLANG:-}" ]; then
    cc="$PS5_CLANG"
elif [ -n "${LLVM_CONFIG:-}" ]; then
    cc="$("$LLVM_CONFIG" --bindir)/clang"
else
    cc="$(command -v clang-21 || command -v clang-18 || command -v clang)"
fi
cxx="${CXX:-clang++}"
native="$upstream/tooling/native"
mkdir -p "$work/host" "$work/obj" "$app/sce_sys" "$app/sce_module" "$app/licenses"

# The host converter uses only C++20 and zlib; no Linux ELF headers or SDK binaries.
patch --silent -o "$work/host/native_app_builder.cpp" "$native/native_app_builder.cpp" \
    "$root/patches/native/0003-native-import-library-override.patch"
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror -I "$native" \
    "$work/host/native_app_builder.cpp" "$native/self_container.cpp" \
    "$native/elf_object.cpp" "$native/sce_module_writer.cpp" -lz -o "$work/host/ps5-native-tool"
"$cxx" -std=c++20 -O2 -Wall -Wextra -Werror \
    "$native/libc_builder.cpp" -o "$work/host/libc-builder"
tool="$work/host/ps5-native-tool"

compile() {
    PS5_CLANG="$cc" USE_CCACHE=0 sh "$upstream/tooling/prospero-clang18" \
        -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections "$@"
}
compile -std=c11 -c "$root/ps5/native-capabilities/main.c" -o "$work/obj/main.o"
for name in app_crt app_cpp_runtime; do
    compile -std=c++20 -fno-exceptions -fno-rtti -c "$native/$name.cpp" -o "$work/obj/$name.o"
done
"$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" --eh-frame-hdr \
    --version-script "$native/app-symbols.map" -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "$work/obj/main.o" \
    --as-needed "$sdk/target/lib/libSceLibcInternal.so" "$sdk/target/lib/libkernel.so" \
    "$sdk/target/lib/libSceSystemService.so"
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk/target/lib" --module-sdk 0x02000009 \
    --stub-library libSceSystemService=libSceShellCoreUtil \
    --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
python3 "$root/scripts/validate-native-capabilities.py" "$work" "$sdk"

# Generate the runtime twice, verify reproducibility and the upstream release hashes.
for copy in a b; do
    "$work/host/libc-builder" "$native/runtime/api-surface.txt" \
        "$native/runtime/imports.txt" "$work/libc-$copy.raw.elf"
    "$tool" self --sign --in "$work/libc-$copy.raw.elf" --out "$work/libc-$copy.prx"
done
cmp "$work/libc-a.raw.elf" "$work/libc-b.raw.elf"
cmp "$work/libc-a.prx" "$work/libc-b.prx"
python3 - "$work" <<'PY'
import hashlib, pathlib, sys
work = pathlib.Path(sys.argv[1])
for name, expected in {
    "libc-a.raw.elf": "8ee6e124993e1af26420cb455890fd002f5d6c7e78883c860ce45734e7d002bb",
    "libc-a.prx": "e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036",
}.items():
    actual = hashlib.sha256((work / name).read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit(f"{name}: expected {expected}, got {actual}")
    print(f"Verified {name}: {actual}")
PY
cp "$work/libc-a.prx" "$app/sce_module/libc.prx"
cp "$root/ps5/native-capabilities/param.json" "$app/sce_sys/param.json"
# Use the matching upstream presentation assets and retain their notices.
for asset in icon0.png pic0.dds pic1.dds snd0.at9; do
    cp "$upstream/sce_sys/$asset" "$app/sce_sys/$asset"
done
cp "$upstream/LICENSE" "$app/licenses/native-boilerplate-GPL-3.0.txt"
cp "$upstream/THIRD_PARTY_NOTICES.md" "$app/licenses/native-boilerplate-notices.txt"
"$tool" self --inspect --file "$app/eboot.bin"
"$tool" self --inspect --file "$app/sce_module/libc.prx"
python3 - "$app" "$revision" "$sdk" "$cc" "$cxx" <<'PY'
import hashlib, json, pathlib, re, subprocess, sys, zipfile
app = pathlib.Path(sys.argv[1])
param = json.loads((app / "sce_sys/param.json").read_text())
if (param["titleId"] != app.name or param["conceptId"] != app.name[4:]
        or not re.fullmatch(rf"[A-Z]{{2}}\d{{4}}-{app.name}_00-[A-Z0-9]{{16}}", param["contentId"])):
    raise SystemExit("Native title metadata does not match the packaged title ID/content ID")
files = sorted(p for p in app.rglob("*") if p.is_file())
manifest = {
    "boilerplate_revision": sys.argv[2],
    "sdk_path": sys.argv[3],
    "sdk_setup": "scripts/setup-sdk.sh pins v0.43; PS5_PAYLOAD_SDK may override",
    "target_compiler": subprocess.check_output([sys.argv[4], "--version"], text=True),
    "host_compiler": subprocess.check_output([sys.argv[5], "--version"], text=True),
    "gpu_tested": False,
    "files": {str(p.relative_to(app)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
}
(app.parent / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
with zipfile.ZipFile(app.with_suffix(".zip"), "w", zipfile.ZIP_DEFLATED) as archive:
    for p in files:
        entry = zipfile.ZipInfo(f"{app.name}/{p.relative_to(app)}")
        entry.create_system = 3
        entry.external_attr = 0o100777 << 16
        entry.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(entry, p.read_bytes())
print(f"Native capability title: {app}")
print(f"Archive: {app.with_suffix('.zip')}")
print("Not deployed. This title does not execute GPU work or request sandbox elevation.")
PY
