#!/usr/bin/env bash
# Reproduce the prepared Mac-hosted PS5 RADV archive. No console access.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
upstream="$root/.deps/PS5_Vulkan"
mesa="$root/.deps/PS5_Mesa"
sdk="$upstream/.deps/native/ps5-payload-sdk"
work="$root/build/radv-preflight"
python="$root/.deps/radv-python"
host_llvm="${RADV_HOST_LLVM:-/opt/homebrew/opt/llvm}"
target_llvm="${RADV_TARGET_LLVM:-/opt/homebrew/opt/llvm@21}"
[ "$(git -C "$upstream" rev-parse HEAD)" = f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed ]
[ "$(git -C "$mesa" rev-parse HEAD)" = 1ba9874f478657ccf6da72ce55f0260164cc0bd0 ]
[ "$(cat "$sdk/.ps5-sdk-revision")" = b83202be73e930050e00de0f1b4d9ec46c0391bf ]
[ "$("$host_llvm/bin/llvm-config" --version)" = 23.1.3 ]
[ "$("$target_llvm/bin/llvm-config" --version)" = 21.1.8 ]
if [ ! -x "$python/bin/meson" ]; then
    python3 -m venv "$python"
    "$python/bin/python" -m pip install meson==1.10.2 mako==1.3.10 MarkupSafe==3.0.3 PyYAML==6.0.3 packaging==26.0
fi
mkdir -p "$work"
host="$work/mesa-clc-llvm23"
host_env=(env "PATH=$python/bin:$host_llvm/bin:/opt/homebrew/bin:/usr/bin:/bin"
    "LLVM_CONFIG=$host_llvm/bin/llvm-config" "CC=$host_llvm/bin/clang" "CXX=$host_llvm/bin/clang++"
    "PKG_CONFIG_PATH=/opt/homebrew/opt/spirv-llvm-translator/lib/pkgconfig:/opt/homebrew/opt/libclc/share/pkgconfig")
if [ ! -f "$host/build.ninja" ]; then
    "${host_env[@]}" meson setup "$host" "$mesa" -Dbuildtype=release -Dmesa-clc=enabled -Dinstall-mesa-clc=true \
        -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
        -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=enabled -Dshared-llvm=enabled \
        -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dtools=
fi
"${host_env[@]}" ninja -C "$host" -j "${BUILD_JOBS:-4}" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2
printf "[constants]\nsdk = '%s'\n" "$sdk" > "$work/cross-constants.ini"
cross_env=(env "PATH=$host/src/compiler/clc:$host/src/compiler/spirv:$python/bin:$target_llvm/bin:/opt/homebrew/bin:/usr/bin:/bin"
    "LLVM_CONFIG=$target_llvm/bin/llvm-config")
if [ ! -f "$work/radv-release/build.ninja" ]; then
    "${cross_env[@]}" meson setup "$work/radv-release" "$mesa" --cross-file "$work/cross-constants.ini" \
        --cross-file "$upstream/tooling/radv/ps5-cross.ini" -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= \
        -Dradv-winsys=ps5 -Dllvm=disabled -Damd-use-llvm=false -Dvideo-codecs= \
        -Dbuildtype=debugoptimized -Db_ndebug=true -Dglx=disabled -Degl=disabled -Dgbm=disabled \
        -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
        -Dzstd=disabled -Dzlib=enabled --force-fallback-for=zlib -Dexpat=disabled -Dxmlconfig=disabled \
        -Dshader-cache=enabled -Dbuild-tests=false -Dvulkan-layers= -Dtools= -Dmesa-clc=system \
        -Dradv-build-id=1ba9874f478657ccf6da72ce55f0260164cc0bd0
fi
"${cross_env[@]}" ninja -C "$work/radv-release" -j "${BUILD_JOBS:-4}" src/amd/vulkan/libvulkan_radeon.a
python3 - "$work" "$host_llvm" "$target_llvm" "$sdk" <<'PY'
import hashlib, json, pathlib, subprocess, sys
work, host, target, sdk = map(pathlib.Path, sys.argv[1:])
archive = work / 'radv-release/src/amd/vulkan/libvulkan_radeon.a'
report = {
    'console_access_performed': False, 'hardware_verified': False,
    'ps5_vulkan_revision': 'f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed',
    'mesa_revision': '1ba9874f478657ccf6da72ce55f0260164cc0bd0',
    'platform_sdk_revision': (sdk / '.ps5-sdk-revision').read_text().strip(),
    'sdk_base_release': 'v0.42', 'build_command': 'bash scripts/build-native-radv.sh',
    'host_compiler': subprocess.check_output([str(host / 'bin/clang'), '--version'], text=True),
    'target_compiler': subprocess.check_output([str(target / 'bin/clang'), '--version'], text=True),
    'variant': 'debugoptimized, b_ndebug=true, radv-winsys=ps5',
    'radv_archive': str(archive), 'radv_archive_bytes': archive.stat().st_size,
    'radv_archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'closure_limit': 'Static driver archive only. Native link, loader compatibility and GPU behavior are separate checks.',
}
(work / 'provenance.json').write_text(json.dumps(report, indent=2) + '\n')
print(archive)
PY
