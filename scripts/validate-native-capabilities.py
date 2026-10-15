#!/usr/bin/env python3
"""Check capabilities' native import contract; does not establish hardware behavior."""
import json
import pathlib
import runpy
import sys


# Reuse the existing native ELF/NID decoder without running the compute validator.
helpers = runpy.run_path(str(pathlib.Path(__file__).with_name('validate-native-compute.py')))
Elf, nid, encode_id = (helpers[name] for name in ('Elf', 'nid', 'encode_id'))


def main():
    work, sdk = map(pathlib.Path, sys.argv[1:3])
    source = Elf(work / 'llvm-pie.elf')
    native = Elf(work / 'eboot.elf')
    needed = source.needed()
    allowed = {'libSceLibcInternal.sprx', 'libkernel.sprx', 'libSceSystemService.sprx'}
    if set(needed) != allowed or len(needed) != len(allowed):
        raise ValueError(f'Capabilities must use native libraries, not payload variants: {needed}')
    if native.needed() != [name.removesuffix('.sprx') + '.prx' for name in needed]:
        raise ValueError('Native conversion changed dependency order or identity')
    providers = {}
    for name in ('libSceLibcInternal', 'libkernel', 'libSceSystemService'):
        stub = Elf(sdk / 'target/lib' / (name + '.so'))
        providers[stub.string(stub.tags[14])] = {
            symbol for symbol, section, info, value in stub.symbols()
            if section and info >> 4 in (1, 2)
        }
    libraries = {module: module.removesuffix('.sprx') for module in needed}
    libraries['libSceSystemService.sprx'] = 'libSceShellCoreUtil'
    imports, expected = [], set()
    for name, section, info, value in source.symbols():
        if section:
            continue
        candidates = [module for module in needed if name in providers[module]]
        if not candidates:
            raise ValueError(f'No native dependency exports {name}')
        module = candidates[0]
        if module == 'libSceSystemService.sprx' and name != 'sceShellCoreUtilExitApp':
            raise ValueError(f'Unexpected symbol in the close-only import library: {name}')
        index = needed.index(module)
        expected.add(nid(name) + '#' + encode_id(index) + '#' + encode_id(index + 1))
        imports.append({'symbol': name, 'module': module, 'library': libraries[module]})
    actual = {name for name, section, info, value in native.symbols() if not section}
    if actual != expected:
        raise ValueError('Converted native NID imports do not match native providers')
    modules = {value >> 48: native.string(value & 0xffffffff)
               for tag, value in native.dynamic if tag == 0x61000045}
    imported_libraries = {value >> 48: native.string(value & 0xffffffff)
                          for tag, value in native.dynamic if tag == 0x61000049}
    if modules != {i + 1: name.removesuffix('.sprx') for i, name in enumerate(needed)}:
        raise ValueError(f'Incorrect native module records: {modules}')
    if imported_libraries != {i: libraries[name] for i, name in enumerate(needed)}:
        raise ValueError(f'Incorrect native import-library records: {imported_libraries}')
    if len(sys.argv) == 4:
        # Optional read-only firmware evidence; never packaged or needed to rebuild.
        firmware = Elf(sys.argv[3], trimmed_runtime=True)
        exports = {encode_id(value >> 48): firmware.string(value & 0xffffffff)
                   for tag, value in firmware.dynamic if tag == 0x61000047}
        matches = [name for name, section, info, value in firmware.symbols()
                   if section and name.split('#')[0] == nid('sceShellCoreUtilExitApp')]
        if len(matches) != 1 or exports.get(matches[0].split('#')[1]) != 'libSceShellCoreUtil':
            raise ValueError('Firmware does not export close API from the expected library')
        print('Verified close export against supplied firmware module.')
    (work / 'imports.json').write_text(json.dumps({'needed': needed, 'imports': imports}, indent=2) + '\n')
    print(f'Validated {len(imports)} capabilities native imports and module/library identities.')


if __name__ == '__main__':
    main()
