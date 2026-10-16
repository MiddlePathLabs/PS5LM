#!/usr/bin/env python3
"""Validate the linked native package, not its unexercised console behavior."""
import base64
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys
import zipfile


class Elf:
    def __init__(self, path, trimmed_runtime=False):
        self.path = pathlib.Path(path)
        self.data = self.path.read_bytes()
        if self.data[:7] != b'\x7fELF\x02\x01\x01':
            raise ValueError(f'{path}: not ELF64 little-endian')
        self.kind, machine = struct.unpack_from('<HH', self.data, 16)
        if machine != 62:
            raise ValueError(f'{path}: not x86-64')
        phoff, shoff = struct.unpack_from('<QQ', self.data, 32)
        phsize, phnum, shsize, shnum = struct.unpack_from('<HHHH', self.data, 54)
        self.ph = [struct.unpack_from('<IIQQQQQQ', self.data, phoff + i * phsize)
                   for i in range(phnum)]
        if shoff + shnum * shsize > len(self.data):
            # The byte-pinned upstream companion deliberately omits its old
            # section-table tail. Its loader metadata is in program headers.
            if not trimmed_runtime or self.kind != 0xFE18:
                raise ValueError(f'{path}: truncated section table')
            shnum = 0
        self.sh = [struct.unpack_from('<IIQQQQIIQQ', self.data, shoff + i * shsize)
                   for i in range(shnum)]
        self.dynamic = []
        for p in self.ph:
            if p[0] == 2:
                for at in range(p[2], p[2] + p[5], 16):
                    tag, value = struct.unpack_from('<qQ', self.data, at)
                    if tag == 0:
                        break
                    self.dynamic.append((tag, value))
        self.tags = dict(self.dynamic)

    def offset(self, address):
        for p in self.ph:
            if p[0] == 1 and p[3] <= address < p[3] + p[5]:
                return p[2] + address - p[3]
        raise ValueError(f'{self.path}: unmapped dynamic address {address:#x}')

    def string(self, index):
        at = self.offset(self.tags[5]) + index
        return self.data[at:self.data.index(b'\0', at)].decode()

    def needed(self):
        return [self.string(v) for t, v in self.dynamic if t == 1]

    def symbols(self):
        count = self.tags.get(0x6100003F, 0) // 24
        for s in self.sh:
            if s[1] == 11:
                count = s[5] // s[9]
        at = self.offset(self.tags[6])
        result = []
        for i in range(1, count):
            name, info, other, section, value, size = struct.unpack_from('<IBBHQQ', self.data, at + i * 24)
            result.append((self.string(name), section, info, value))
        return result


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def check_reconstruction(original, reconstructed):
    data = pathlib.Path(reconstructed).read_bytes()
    if len(data) != len(original.data):
        raise ValueError(f'{reconstructed}: reconstructed file extent differs')
    cursor = 0
    omitted = []
    # Upstream self_container.cpp selects load/link/procparam/comment data and
    # separately carries version records, but excludes unmapped PT_NOTE data.
    for p in sorted(original.ph, key=lambda entry: entry[2]):
        if p[0] != 4 or p[1] != 0 or p[3] != 0 or p[6] != 0 or not p[5]:
            continue
        begin, end = p[2], p[2] + p[5]
        if any(q[0] == 1 and q[2] < end and begin < q[2] + q[5] for q in original.ph):
            raise ValueError('Unmapped NOTE overlaps a loadable segment')
        if original.data[cursor:begin] != data[cursor:begin] or any(data[begin:end]):
            raise ValueError(f'{reconstructed}: changed stored bytes or nonzero omitted NOTE')
        omitted.append({'offset': begin, 'bytes': p[5], 'sha256': hashlib.sha256(original.data[begin:end]).hexdigest()})
        cursor = end
    if original.data[cursor:] != data[cursor:]:
        raise ValueError(f'{reconstructed}: changed stored executable bytes')
    return omitted


def encode_id(value):
    alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
    out = ''
    while value:
        out = alphabet[value % 64] + out
        value //= 64
    return out or 'A'


def nid(name):
    suffix = bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')
    value = hashlib.sha1(name.encode() + suffix).digest()[:8][::-1]
    return base64.b64encode(value).decode().rstrip('=').replace('/', '-')


CLIMB = {
    'moe3b': {'repo': 'bartowski/granite-3.1-3b-a800m-instruct-GGUF',
              'revision': 'be9a36f042806cb586bc65556c527079782b78e0',
              'file': 'granite-3.1-3b-a800m-instruct-Q4_K_M.gguf', 'bytes': 2016888384,
              'sha256': '48e0edcd578fd4462f26127f04c651d0e650741110185297741089aea01a82b3'},
    'olmoe': {'repo': 'allenai/OLMoE-1B-7B-0125-Instruct-GGUF',
              'revision': '2ac1d27317927518c9ef7bd99f91f3f1e4ee288d',
              'file': 'OLMoE-1B-7B-0125-Instruct-Q4_K_M.gguf', 'bytes': 4213512192,
              'sha256': '4ddc0e53159ed512b8dd67914a66e27bc618f694672ba43a9a0454eabd9c684f'},
    'qwen3moe': {'repo': 'bartowski/Qwen_Qwen3-30B-A3B-Instruct-2507-GGUF',
                 'revision': '6c6e8692f43e4ca663f7ece8229a1361090d3a4c',
                 'file': 'Qwen_Qwen3-30B-A3B-Instruct-2507-IQ2_M.gguf', 'bytes': 9870270464,
                 'sha256': 'f2dc78edd3ec0171904f1945d8c05a948131b1103172b1710b763db2eb65f52a'},
    # Vision (docs/PORTING.md "Vision"): the text model and its mmproj.
    'vl4b': {'repo': 'Qwen/Qwen3-VL-4B-Instruct-GGUF',
             'revision': '1cd86afb9a95c410a6038ab3b40d8b578c892266',
             'file': 'Qwen3VL-4B-Instruct-Q4_K_M.gguf', 'bytes': 2497281664,
             'sha256': '66358cb18bb6b3b1b6675aa412c7a88ef01d228f481184d13668e5201c730a0a',
             'mmproj': {'file': 'mmproj-Qwen3VL-4B-Instruct-F16.gguf', 'bytes': 836180256,
                        'sha256': '256f3a43bd4205ffef48d6b92715e1e70b5b0e9aef06522584967513a9985331'}},
}
VISION = ('vl4b', 'vl8b')


def main():
    work, sdk, archive, llvm, root = map(pathlib.Path, sys.argv[1:])
    app = work / 'PPSA99041'
    server = work.name.startswith('native-server')
    moe = work.name in ('native-moe', 'native-server')
    # The MoE climb (docs/PORTING.md "Larger MoE models"): native-VARIANT / native-server-VARIANT.
    climb = next((v for v in CLIMB if work.name in ('native-' + v, 'native-server-' + v)), None)
    model = moe or server or climb is not None or work.name == 'native-model'
    # SSD streaming probe (docs/PORTING.md "SSD streaming"): llama.cpp linked, no model load.
    probe = work.name == 'native-stream-probe'
    ggml_ops = model or probe or work.name == 'native-ggml-ops'
    source = Elf(work / 'llvm-pie.elf')
    native = Elf(work / 'eboot.elf')
    omitted_notes = check_reconstruction(native, work / 'extracted.elf')
    if source.kind != 3 or native.kind != 0xFE10:
        raise ValueError('Expected PIE intermediate and ET_SCE_EXEC_ASLR final executable')
    if native.data[7:9] != b'\x09\x02' or any(p[0] == 3 for p in native.ph):
        raise ValueError('Wrong native ABI or unexpected PT_INTERP')
    needed = source.needed()
    native_needed = native.needed()
    if [n.removesuffix('.sprx') + '.prx' if n.endswith('.sprx') else n for n in needed] != native_needed:
        raise ValueError('Conversion changed normalized DT_NEEDED closure')
    allowed = {'libSceAgc.prx', 'libSceAgcDriver.prx', 'libSceLibcInternal.sprx',
               'libkernel.sprx', 'libSceSystemService.sprx', 'libSceVideoOut.sprx'}
    if set(needed) != allowed or len(needed) != len(allowed):
        raise ValueError(f'Unexpected native compute dependencies (payload variants forbidden): {needed}')
    providers = {}
    for path in sorted((sdk / 'target/lib').glob('*.so')) + sorted((work / 'stubs').glob('*.so')):
        stub = Elf(path)
        soname = stub.string(stub.tags[14])
        providers[soname] = (path, {n for n, section, info, value in stub.symbols()
                                  if section and info >> 4 in (1, 2)})
    missing_modules = set(needed) - providers.keys()
    if missing_modules:
        raise ValueError(f'Missing SDK/link-stub modules: {missing_modules}')
    libraries = {name: name.rsplit('.', 1)[0] for name in needed}
    libraries['libSceSystemService.sprx'] = 'libSceShellCoreUtil'
    imports = []
    expected_native = set()
    builtin_names = {'__divti3', '__udivti3', '__udivmodti4', '__emutls_get_address', '__cpu_model'}
    for name, section, info, value in source.symbols():
        if section:
            continue
        if name in builtin_names or name.startswith('__real_'):
            raise ValueError(f'Unclosed static runtime import: {name}')
        if name in ('_exit', '_Exit'):
            raise ValueError(f'Known-unsafe native process termination import: {name}')
        candidates = [module for module in needed if name in providers[module][1]]
        if not candidates:
            raise ValueError(f'No declared dependency exports {name}')
        module = candidates[0]  # Same declared-order resolution as the native converter.
        if module == 'libSceSystemService.sprx' and name != 'sceShellCoreUtilExitApp':
            raise ValueError(f'Unexpected close-library import: {name}')
        index = needed.index(module)
        expected_native.add(nid(name) + '#' + encode_id(index) + '#' + encode_id(index + 1))
        imports.append({'symbol': name, 'module': module, 'library': libraries[module]})
    native_imports = {n for n, section, info, value in native.symbols() if section == 0}
    if native_imports != expected_native:
        raise ValueError('Final SCE NID imports differ from verified symbol/provider closure')
    modules = {value >> 48: native.string(value & 0xffffffff)
               for tag, value in native.dynamic if tag == 0x61000045}
    imported_libraries = {value >> 48: native.string(value & 0xffffffff)
                          for tag, value in native.dynamic if tag == 0x61000049}
    if modules != {i + 1: name.rsplit('.', 1)[0] for i, name in enumerate(needed)}:
        raise ValueError(f'Incorrect native module records: {modules}')
    if imported_libraries != {i: libraries[name] for i, name in enumerate(needed)}:
        raise ValueError(f'Incorrect native import-library records: {imported_libraries}')
    if any(n.startswith('__emutls') or n == '__cpu_model' for n in native_imports):
        raise ValueError('Unexpected runtime import')
    audit = (work / 'runtime-audit.log').read_text()
    required = set(re.findall(r'undefined symbol: (\S+)', audit))
    if not required or required - builtin_names:
        raise ValueError(f'Unexpected no-builtins link failures: {sorted(required)}')
    extraction = (work / 'archive-extraction.tsv').read_text()
    if 'libclang_rt.builtins-ps5-x86_64.a' not in extraction:
        raise ValueError('Final link did not extract target builtins')
    libc_hash = digest(app / 'sce_module/libc.prx')
    if libc_hash != 'e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036':
        raise ValueError('Bundled runtime does not match verified upstream shim')
    companion = Elf(work / 'libc-a.raw.elf', trimmed_runtime=True)
    companion_omitted_notes = check_reconstruction(companion, work / 'libc-extracted.elf')
    runtime_needed = companion.needed()
    if set(runtime_needed) != {'libkernel.prx', 'libSceLibcInternal.prx', 'libSceSysmodule.prx'}:
        raise ValueError(f'Unexpected companion dependencies: {runtime_needed}')
    runtime_contract = root / '.deps/PS5_Vulkan/tooling/native/runtime/imports.txt'
    runtime_expected = set()
    for line in runtime_contract.read_text().splitlines():
        if line and not line.startswith('#'):
            name, suffix, plt, glob = line.split('|')
            runtime_expected.add(nid(name) + suffix)
    runtime_imports = {n for n, section, info, value in companion.symbols() if section == 0}
    if runtime_imports != runtime_expected:
        raise ValueError('Bundled companion NID imports differ from its upstream contract')
    param = json.loads((app / 'sce_sys/param.json').read_text())
    if (param['titleId'] != app.name or param['conceptId'] != '99041'
            or not re.fullmatch(r'UP9000-PPSA99041_00-[A-Z0-9]{16}', param['contentId'])):
        raise ValueError('Inconsistent package identity')
    files = sorted(p for p in app.rglob('*') if p.is_file())
    relative = {str(p.relative_to(app)) for p in files}
    expected = {'eboot.bin', 'sce_module/libc.prx', 'sce_sys/param.json', 'sce_sys/icon0.png',
                'licenses/PS5-Vulkan-GPL-3.0.txt', 'licenses/PS5-Vulkan-notices.txt', 'licenses/compiler-rt.txt'}
    if ggml_ops:
        expected.add('licenses/llama.cpp-MIT.txt')
    if server:
        expected |= {'licenses/cpp-httplib-MIT.txt', 'licenses/nlohmann-json-MIT.txt'}
    if relative != expected:
        raise ValueError(f'Unexpected package files: {relative ^ expected}')
    (work / 'imports.json').write_text(json.dumps({'needed': needed, 'imports': imports}, indent=2) + '\n')
    with zipfile.ZipFile(app.with_suffix('.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        for path in files:
            entry = zipfile.ZipInfo(f'{app.name}/{path.relative_to(app)}')
            entry.create_system = 3
            entry.external_attr = 0o100777 << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(entry, path.read_bytes())
    manifest = {
        'gpu_executed': False, 'hardware_verified': False,
        'hardware_gate': 'First launch PPSA99040 queries and observe clean exit; only then consider PPSA99041.',
        'build_command': 'bash scripts/build-native-compute.sh',
        'radv_archive': str(archive), 'radv_sha256': digest(archive),
        'radv_build_provenance': json.loads((root / 'build/radv-preflight/provenance.json').read_text()),
        'builtins': json.loads((root / 'build/ps5-builtins/manifest.json').read_text()),
        'actual_no_builtins_unresolved': sorted(required),
        'compiler': subprocess.check_output([str(llvm / 'clang'), '--version'], text=True),
        'llvm_pie_sha256': digest(work / 'llvm-pie.elf'), 'native_elf_sha256': digest(work / 'eboot.elf'),
        'needed_system_modules': native_needed, 'link_stub_modules': needed,
        'verified_native_nid_imports': len(imports),
        'driver_native_compatibility': json.loads((work / 'driver-compat.json').read_text()),
        'companion_system_modules': runtime_needed, 'verified_companion_nid_imports': len(runtime_imports),
        'fself_omitted_unmapped_notes': omitted_notes,
        'companion_omitted_unmapped_notes': companion_omitted_notes,
        'companion_section_table': 'Byte-pinned upstream shim omits the legacy section-table tail; program/dynamic tables are validated.',
        'files': {str(p.relative_to(app)): digest(p) for p in files},
        'archive_sha256': digest(app.with_suffix('.zip')),
        'source_files': {str(p.relative_to(root)): digest(p) for p in
                         sorted((root / 'ps5/native-compute').glob('*')) if p.is_file()},
        'build_scripts': {name: digest(root / 'scripts' / name) for name in
                          ('build-native-radv.sh', 'build-ps5-builtins.sh', 'build-native-compute.sh',
                           'validate-native-compute.py')},
        'link_override_patch_sha256': digest(root / 'patches/native/0001-target-builtins-override.patch'),
        'native_converter_patch_sha256': digest(root / 'patches/native/0003-native-import-library-override.patch'),
        'close_api': {'symbol': 'sceShellCoreUtilExitApp', 'module': 'libSceSystemService',
                      'library': 'libSceShellCoreUtil',
                      'limit': 'Shell IPC may block or reject closure; watchdog cannot guarantee recovery.'},
        'limits': 'See ps5/native-compute/main.c and docs/PORTING.md for physical allocation budget and fence/watchdog bounds; no hardware proof.',
        'closure_limit': 'SDK-stub and converted NID closure is checked locally; real module exports/permissions on Pro12.70 require hardware.',
    }
    manifest['workload'] = (('server-moe' if moe else 'server-dense') if server else
                            'model-moe-greedy' if moe else 'model-dense-greedy' if model else
                            'ggml-attn-prefill-scoped-fp32' if ggml_ops else 'integer-dispatch')
    if climb:
        manifest['workload'] = f'server-{climb}' if server else f'model-{climb}-greedy'
    if probe:
        manifest['workload'] = 'stream-probe'
    if ggml_ops:
        manifest['build_command'] += (' ' + work.name.removeprefix('native-') if climb or probe else
                                      (' server' if moe else ' server-qwen') if server else
                                      ' moe' if moe else ' model' if model else ' ggml-ops')
        manifest['llama'] = json.loads((root / 'build/native-llama/manifest.json').read_text())
        manifest['build_scripts']['build-native-llama.sh'] = digest(root / 'scripts/build-native-llama.sh')
        manifest['hardware_gate'] = 'Native capabilities/tiny compute gates passed separately; ggml operators require one monitored Play and independent lifecycle evidence.'
    if model:
        manifest['model'] = {'repo': 'bartowski/granite-3.1-1b-a400m-instruct-GGUF',
                             'revision': '940d2e1f9f65330615c7c8e980e6c5ac73d3360c',
                             'file': 'granite-3.1-1b-a400m-instruct-Q4_K_M.gguf', 'bytes': 821847360,
                             'sha256': '3a2ec1c2a78cb29d901e29bbf5162dcd03381e13803d2cbdcff838d4d08142eb',
                             'console_path': '/data/PS5LM/models/granite-3.1-1b-a400m-instruct-Q4_K_M.gguf'} if moe else {'repo': 'unsloth/Qwen3.5-0.8B-GGUF',
                             'revision': '6ab461498e2023f6e3c1baea90a8f0fe38ab64d0',
                             'file': 'Qwen3.5-0.8B-Q4_K_M.gguf', 'bytes': 532517120,
                             'sha256': 'bd258782e35f7f458f8aced1adc053e6e92e89bc735ba3be89d38a06121dc517',
                             'console_path': '/data/PS5LM/models/Qwen3.5-0.8B-Q4_K_M.gguf'}
        if climb:
            manifest['model'] = dict(CLIMB[climb], console_path='/data/PS5LM/models/' + CLIMB[climb]['file'])
        if not server:  # the server mode is checked against the model mode's tokens from the Mac
            manifest['cpu_reference_header_sha256'] = digest(work / 'gen/model-reference.h')
        if climb in VISION:
            manifest['build_scripts']['build-host-vision-reference.sh'] = digest(root / 'scripts/build-host-vision-reference.sh')
            manifest['test_image_sha256'] = digest(root / 'testdata/vision/hello-shapes-448.png')
            if not server:
                manifest['cpu_reference_files_sha256'] = {
                    f: digest(work / 'gen' / f) for f in ('vision-reference.h', 'vision-embd-ref.bin', 'vision-trace-ref.bin')}
        manifest['build_scripts']['build-host-model-reference.sh'] = digest(root / 'scripts/build-host-model-reference.sh')
        manifest['hardware_gate'] = 'One monitored ps5-native-test.sh run; read result.json and confirm closed before the next.'
    (work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Validated x86-64 native ABI, {len(imports)} NID imports, package closure and fSELF round trip.')
    print('Actual no-builtins unresolved:', ', '.join(sorted(required)))


if __name__ == '__main__':
    main()
