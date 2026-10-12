"""Package a compatible Wii Alink.arc into an asset-only SkyBreak mod.

This reads your extracted USA 1.0 Wii game. It never changes the source game.
No game assets are included with this tool. Requires Python 3.11+.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import uuid
import shutil
from codec import decompress_lz, u8_files, u32, FormatError

def repack_u8(data, replacements):
    entries=list(u8_files(data))
    missing=set(replacements)-{name for name,_ in entries}
    if missing: raise FormatError(f'Archive entries not found: {sorted(missing)}')
    root=u32(data,4);count=u32(data,root+8);start=u32(data,12)
    out=bytearray(data[:start]); file_nodes=[root+i*12 for i in range(1,count) if data[root+i*12]==0]
    if len(file_nodes)!=len(entries): raise FormatError('Invalid U8 file table')
    for pos,(name,payload) in zip(file_nodes,entries):
        payload=replacements.get(name,payload)
        out.extend(b'\0'*((-len(out))%32))
        struct.pack_into('>II',out,pos+4,len(out),len(payload));out.extend(payload)
    out.extend(b'\0'*((-len(out))%32))
    result=bytes(out)
    assert dict(u8_files(result))=={name:replacements.get(name,payload) for name,payload in entries}
    return result

def compress_literals(data):
    # LZ11 literal groups are valid, deterministic and require no external tools.
    # The compressed input is a little larger; the decoded archive size is exact.
    if not data or len(data)>=0x1000000: raise FormatError('Archive must be smaller than 16 MiB')
    out=bytearray(b'\x11'+len(data).to_bytes(3,'little'))
    for p in range(0,len(data),8):out.append(0);out.extend(data[p:p+8])
    return bytes(out)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--game-dir',type=Path,required=True,help='Extracted game with sys/ and files/')
    source=p.add_mutually_exclusive_group(required=True)
    source.add_argument('--alink',type=Path,help='Compatible Wii Alink.arc, not FBX/GLB/Switch model')
    source.add_argument('--brres',type=Path,help='Edited compatible Wii model.brres; constructs Alink.arc from your original')
    source.add_argument('--extract',action='store_true',help='Extract original Alink.arc and model.brres as an editing starter')
    p.add_argument('--out',type=Path,required=True,help='New mod directory (must not already exist)')
    p.add_argument('--id',default='link-model');p.add_argument('--name',default='Link model swap');p.add_argument('--author',default='')
    a=p.parse_args()
    if not re.fullmatch('[a-z0-9_-]{1,64}',a.id):p.error('id: lowercase letters, digits, - and _ only')
    if a.out.exists():p.error('Output already exists; choose a new mod directory')
    if a.out.resolve().is_relative_to(a.game_dir.resolve()):p.error('Output must be outside the extracted game')
    if (a.game_dir/'sys/boot.bin').read_bytes()[:6]!=b'SOUE01':p.error('Expected USA Wii game SOUE01')
    pack=decompress_lz((a.game_dir/'files/Object/ObjectPack.arc.LZ').read_bytes())
    original=dict(u8_files(pack))['oarc/Alink.arc']
    if a.extract:
        model=dict(u8_files(original))['g3d/model.brres']
        a.out.mkdir(parents=True);(a.out/'Alink.arc').write_bytes(original);(a.out/'model.brres').write_bytes(model)
        print(f'Original editing starter: {a.out}\nEdit model.brres, then use --brres to make a mod.');return
    if a.brres:
        model=a.brres.read_bytes()
        if len(model)<16 or model[:4]!=b'bres' or model[4:6]!=b'\xfe\xff' or u32(model,8)>len(model):p.error('Expected a big-endian Wii BRRES file')
        alink=repack_u8(original,{'g3d/model.brres':model})
    else:alink=a.alink.read_bytes()
    contents=dict(u8_files(alink))
    if 'g3d/model.brres' not in contents:p.error('Alink.arc must contain g3d/model.brres')
    rebuilt=repack_u8(pack,{'oarc/Alink.arc':alink}); compressed=compress_literals(rebuilt)
    assert decompress_lz(compressed)==rebuilt
    a.out.parent.mkdir(parents=True,exist_ok=True)
    # Normal mkdir inherits the Windows directory ACL (mkdtemp's mode 0700
    # can make a package unreadable to the user's installer account).
    tmp=a.out.parent/('.skybreak-mod-'+uuid.uuid4().hex)
    tmp.mkdir()
    try:
        dest=tmp/'files/Object/ObjectPack.arc.LZ';dest.parent.mkdir(parents=True);dest.write_bytes(compressed)
        (tmp/'Alink.arc').write_bytes(alink)
        metadata={'id':a.id,'name':a.name,'game':'SOUE01','version':'1.0','author':a.author,'description':'Compatible Wii Link archive swap. Enable in SkyBreak > Mods.'}
        (tmp/'mod.toml').write_text('schema = 1\n'+''.join(f'{k} = {json.dumps(v,ensure_ascii=False)}\n' for k,v in metadata.items()),encoding='utf-8')
        tmp.rename(a.out)
    except BaseException:
        assert tmp.resolve().parent==a.out.resolve().parent and tmp.name.startswith('.skybreak-mod-') and not tmp.is_symlink()
        shutil.rmtree(tmp);raise
    print(f'Created {a.out}\nEnable this mod in SkyBreak > Mods. Other ObjectPack replacements conflict with it.')

if __name__=='__main__':main()
