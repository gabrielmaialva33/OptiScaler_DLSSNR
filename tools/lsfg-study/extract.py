"""Dump the RT_RCDATA (or any) resources of lsfg-vk.dll that are SPIR-V modules, named by resource id."""
import sys, pathlib, pefile

dll = pathlib.Path(sys.argv[1]); out = pathlib.Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
pe = pefile.PE(str(dll), fast_load=False)
count = 0
for rtype in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    for rid in rtype.directory.entries:
        for lang in rid.directory.entries:
            data = pe.get_data(lang.data.struct.OffsetToData, lang.data.struct.Size)
            kind = 'spv' if data[:4] == bytes.fromhex('03022307') else 'bin'
            ident = rid.id if rid.id is not None else str(rid.name)
            (out / f'{ident}.{kind}').write_bytes(data)
            count += 1
            print(f'type={rtype.id} id={ident} size={len(data)} kind={kind}')
print('total', count)
