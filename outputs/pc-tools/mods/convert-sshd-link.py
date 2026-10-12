"""Experimental SSHD BRRES+XTX Link asset conversion for SkyBreak SOUE01.

Scope: Wii-compatible MDL0 revision 11 models preserving Link's model/bone names,
with external TEX0 placeholders in dictionary order matching XTX image indices.
No executable patches, BFRES import, rig conversion or arbitrary HD mod support.
Format references: Switch-Toolbox XTX.cs; Tegra X1 block-linear addressing/TRM.
Requires Pillow, numpy, texture2ddecoder (MIT); generated mods need none of these.
"""
import argparse,hashlib,importlib.util,json,struct,sys,uuid,shutil
from pathlib import Path,PurePosixPath
from codec import check,u32,string,u8_files,decompress_lz,FormatError
from lz11_encode import compress as compress_lz11

def u16(d,p):check(d,p,2);return struct.unpack_from('>H',d,p)[0]
def s32(d,p):check(d,p,4);return struct.unpack_from('>i',d,p)[0]
def dictionary(d,p):
    count=u32(d,p+4)
    if count>4096:raise FormatError('Oversized BRRES dictionary')
    check(d,p,8+(count+1)*16)
    for i in range(count):
        node=p+8+(i+1)*16
        yield string(d,p+s32(d,node+8)),p+s32(d,node+12),node+12

def groups(d):
    if len(d)<16 or d[:4]!=b'bres' or d[4:6]!=b'\xfe\xff':raise FormatError('Expected big-endian BRRES')
    if u32(d,8)>len(d):raise FormatError('Truncated BRRES')
    return {name:(pos,list(dictionary(d,pos))) for name,pos,_ in dictionary(d,u16(d,12)+8)}

def check_rig(base,source):
    a=groups(base).get('3DModels(NW4R)',(0,[]))[1];b=groups(source).get('3DModels(NW4R)',(0,[]))[1]
    by_name={name:pos for name,pos,_ in b}
    if {n for n,_,_ in a}!={n for n,_,_ in b}:raise FormatError('Different model names; manual adaptation required')
    for name,pos,_ in a:
        other=by_name[name]
        if u32(source,other+8)!=11:raise FormatError('Only MDL0 revision 11 is supported')
        def bones(d,p):return [n for n,_,_ in dictionary(d,p+s32(d,p+0x14))]
        if bones(base,pos)!=bones(source,other):raise FormatError(f'Bone names/order differ in {name}')

ASTC=[(4,4),(5,4),(5,5),(6,5),(6,6),(8,5),(8,6),(8,8),(10,5),(10,6),(10,8),(10,10),(12,10),(12,12)]
def xtx_images(d):
    if len(d)<16 or d[:4]!=b'DFvN':raise FormatError('Expected NVIDIA XTX (DFvN)')
    hs,major,minor=struct.unpack_from('<III',d,4)
    if hs!=16 or major!=1:raise FormatError('Unsupported XTX header')
    p=hs;infos={};images={};notes=[]
    while p<len(d):
        check(d,p,36)
        if d[p:p+4]!=b'HBvN':raise FormatError('Invalid XTX block')
        size,n,off,kind,bid,index=struct.unpack_from('<IQQIII',d,p+4)
        if size!=36 or off<size:raise FormatError('Invalid XTX block size/offset')
        check(d,p+off,n);payload=d[p+off:p+off+n]
        if kind==2:
            check(payload,0,120)
            declared,align,w,h,depth,target,fmt,mips,slice_size=struct.unpack_from('<Q8I',payload)
            if not(0<w<=4096 and 0<h<=4096 and depth==1 and target==1 and 1<=mips<=17):raise FormatError('Unsupported XTX texture dimensions/target')
            if index in infos:raise FormatError('Duplicate XTX info index')
            infos[index]={'width':w,'height':h,'format':fmt,'mips':mips,'block_height':1<<(struct.unpack_from('<I',payload,108)[0]&7),'declared_size':declared,'first_mip_offset':struct.unpack_from('<I',payload,40)[0]}
        elif kind==3:
            if index in images:raise FormatError('Duplicate XTX image index')
            images[index]=payload
        elif kind!=5:raise FormatError('Unsupported XTX block type')
        p+=off+n
    if set(infos)!=set(images) or sorted(infos)!=list(range(len(infos))):raise FormatError('XTX info/data indices differ')
    out=[]
    for i in sorted(infos):
        info=infos[i];info['data']=images[i];info['index']=i
        # Some Toolbox exports write mip sizes into the offset table, and stale
        # declared sizes. The base image still starts at its actual data block.
        if info['declared_size']!=len(images[i]) or info['first_mip_offset']:
            notes.append(f'Image {i}: exported metadata differs; using bounded actual base-image data block')
        out.append(info)
    return out,notes

def block_address(x,y,width,bpp,bh):
    xb=x*bpp;pitch_gobs=(width*bpp+63)//64
    return (y//(8*bh))*512*bh*pitch_gobs+(xb//64)*512*bh+((y//8)%bh)*512+((xb//32)%2)*256+((y//2)%4)*64+((xb//16)%2)*32+(y%2)*16+xb%16

def linear_base(info):
    fmt=info['format']
    if 0x79<=fmt<=0x94:
        bw,bh=ASTC[(fmt-0x79)%14];bpp=16
    elif fmt in (0x42,0x44):bw=bh=4;bpp=8 if fmt==0x42 else 16
    elif fmt in (0x25,0x38):bw=bh=1;bpp=4
    else:raise FormatError(f'Unsupported XTX texture format {fmt:#x}')
    w=(info['width']+bw-1)//bw;h=(info['height']+bh-1)//bh
    linear=bytearray(w*h*bpp)
    for y in range(h):
        for x in range(w):
            src=block_address(x,y,w,bpp,info['block_height']);check(info['data'],src,bpp)
            dst=(y*w+x)*bpp;linear[dst:dst+bpp]=info['data'][src:src+bpp]
    return bytes(linear),bw,bh

def decode(info):
    import texture2ddecoder
    from PIL import Image
    raw,bw,bh=linear_base(info);w,h=info['width'],info['height'];fmt=info['format']
    if 0x79<=fmt<=0x94:pixels=texture2ddecoder.decode_astc(raw,w,h,bw,bh)
    elif fmt==0x42:pixels=texture2ddecoder.decode_bc1(raw,w,h)
    elif fmt==0x44:pixels=texture2ddecoder.decode_bc3(raw,w,h)
    else:return Image.frombytes('RGBA',(w,h),raw)
    return Image.frombytes('RGBA',(w,h),pixels,'raw','BGRA')

def rgba8_tiles(image):
    import numpy as np
    a=np.asarray(image.convert('RGBA'),dtype=np.uint8);h,w=a.shape[:2]
    padded=np.zeros(((h+3)//4*4,(w+3)//4*4,4),dtype=np.uint8);padded[:h,:w]=a
    t=padded.reshape(padded.shape[0]//4,4,padded.shape[1]//4,4,4).transpose(0,2,1,3,4).reshape(-1,16,4)
    return np.concatenate((t[:,:,[3,0]].reshape(-1,32),t[:,:,[1,2]].reshape(-1,32)),axis=1).tobytes()

def compact_tiles(image):
    """Small Wii fallback; the original HD RGBA image is a native replacement."""
    import numpy as np
    a=np.asarray(image.convert('RGBA'),dtype=np.uint8);h,w=a.shape[:2]
    # Smooth alpha uses RGB5A3 rather than one-bit CMPR transparency.
    if np.any((a[:,:,3]>0)&(a[:,:,3]<255)):
        pad=np.zeros(((h+3)//4*4,(w+3)//4*4,4),dtype=np.uint16);pad[:h,:w]=a
        r,g,b,alpha=pad.transpose(2,0,1)
        v=np.where(alpha>=224,0x8000|((r>>3)<<10)|((g>>3)<<5)|(b>>3),((alpha>>5)<<12)|((r>>4)<<8)|((g>>4)<<4)|(b>>4))
        return 5,v.reshape(v.shape[0]//4,4,v.shape[1]//4,4).transpose(0,2,1,3).astype('>u2').tobytes()
    pad=np.zeros(((h+7)//8*8,(w+7)//8*8,4),dtype=np.uint8);pad[:h,:w]=a
    # Macroblocks TL/TR/BL/BR, each containing sixteen row-major texels.
    blocks=pad.reshape(pad.shape[0]//8,2,4,pad.shape[1]//8,2,4,4).transpose(0,3,1,4,2,5,6).reshape(-1,16,4)
    rgb=blocks[:,:,:3].astype(np.int32);visible=blocks[:,:,3]>=128
    lo=np.min(np.where(visible[:,:,None],rgb,255),axis=1);hi=np.max(np.where(visible[:,:,None],rgb,0),axis=1)
    def pack565(c):return ((c[:,0]>>3)<<11)|((c[:,1]>>2)<<5)|(c[:,2]>>3)
    low,high=pack565(lo),pack565(hi);transparent=np.any(~visible,axis=1)
    c0=np.where(transparent,np.minimum(low,high),high);c1=np.where(transparent,np.maximum(low,high),low)
    # Flat opaque blocks still need the four-color mode.
    c0=np.where((~transparent)&(c0==c1)&(c0<65535),c0+1,c0)
    c1=np.where((~transparent)&(c0==c1)&(c1>0),c1-1,c1)
    def unpack(c):
        r,g,b=(c>>11)&31,(c>>5)&63,c&31
        return np.stack(((r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)),axis=1)
    p0,p1=unpack(c0),unpack(c1);opaque=c0>c1
    p2=np.where(opaque[:,None],(p0*5+p1*3)>>3,(p0+p1)//2)
    p3=np.where(opaque[:,None],(p0*3+p1*5)>>3,(p0+p1)//2)
    palette=np.stack((p0,p1,p2,p3),axis=1)
    error=np.sum((rgb[:,:,None,:]-palette[:,None,:,:])**2,axis=3)
    error[:,:,3]=np.where(transparent[:,None],2**30,error[:,:,3])
    indices=np.argmin(error,axis=2);indices=np.where(visible,indices,3)
    bits=np.sum(indices.reshape(-1,4,4)<<np.array([6,4,2,0]),axis=2)
    out=np.empty((len(blocks),8),dtype=np.uint8)
    out[:,0]=c0>>8;out[:,1]=c0&255;out[:,2]=c1>>8;out[:,3]=c1&255;out[:,4:]=bits
    return 14,out.tobytes()

def convert_brres(source,xtx,base,texture_dir,preview_dir):
    from PIL import Image
    try:from gx_texture import dolphin_name,decode_rgba
    except ImportError:
        sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'hdtex'))
        from gx_texture import dolphin_name,decode_rgba
    check_rig(base,source)
    src_group=groups(source)['Textures(NW4R)'];base_textures={n:p for n,p,_ in groups(base)['Textures(NW4R)'][1]}
    external=[(n,p,slot) for n,p,slot in src_group[1] if u16(source,p+28)==1 and u16(source,p+30)==1 and s32(source,p+16)==96]
    images,notes=xtx_images(xtx)
    if len(external)!=len(images):raise FormatError('TEX0 placeholder count does not match XTX images; mapping needs manual review')
    patched=bytearray(source);rows=[];keys={}
    texture_dir.mkdir(parents=True,exist_ok=True);preview_dir.mkdir(parents=True,exist_ok=True)
    for (name,pos,slot),info in zip(external,images):
        if name not in base_textures:raise FormatError(f'No Wii reference texture for {name}')
        image=decode(info);original=base_textures[name];w,h=u16(base,original+28),u16(base,original+30)
        fallback=image.resize((w,h),Image.Resampling.LANCZOS);gx_format,pixels=compact_tiles(fallback)
        # Validate bounded decoding and use the encoded bytes for replacement IDs.
        assert len(decode_rgba(w,h,gx_format,pixels))==w*h*4
        key=dolphin_name(w,h,gx_format,pixels)
        digest=hashlib.sha256(image.tobytes()).hexdigest()
        if key in keys and keys[key]!=digest:raise FormatError('Distinct HD images have the same fallback hash; manual mapping required')
        keys[key]=digest;image.save(texture_dir/(key+'_arb.png'));image.save(preview_dir/(name+'.png'))
        # Append complete disjoint Wii TEX0 blocks, update only root dictionary
        # data pointers. All model, bone, shader and animation offsets stay put.
        patched.extend(b'\0'*((-len(patched))%32));newpos=len(patched)
        header=bytearray(source[pos:pos+64]);header[56:64]=b'\0'*8
        struct.pack_into('>IIiIiIHHIIffII',header,4,64+len(pixels),1,-newpos,64,pos+s32(source,pos+20)-newpos,0,w,h,gx_format,1,0.,0.,0,0)
        # Explicit fields avoid relying on revision-specific trailing data.
        struct.pack_into('>I',header,36,1);struct.pack_into('>ff',header,40,0.,0.)
        struct.pack_into('>i',patched,slot,newpos-src_group[0]);patched.extend(header);patched.extend(pixels)
        rows.append({'name':name,'xtx_index':info['index'],'format':info['format'],'hd_size':[info['width'],info['height']],'fallback_size':[w,h],'fallback_format':gx_format,'texture_key':key,'pixels_sha256':digest})
    struct.pack_into('>I',patched,8,len(patched));return bytes(patched),rows,notes

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--alink',type=Path,required=True);p.add_argument('--game-dir',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
    p.add_argument('--id',default='sshd-link');p.add_argument('--name',default='SSHD Link conversion');p.add_argument('--author',default='See original source mod')
    p.add_argument('--decoder-dir',type=Path)
    a=p.parse_args()
    if a.decoder_dir:sys.path.insert(0,str(a.decoder_dir.resolve()))
    if a.out.exists():p.error('Output must be a new directory')
    if a.out.resolve().is_relative_to(a.game_dir.resolve()):p.error('Output must be outside source game')
    if not a.id or len(a.id)>64 or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_-' for c in a.id):p.error('Invalid mod id')
    if (a.game_dir/'sys/boot.bin').read_bytes()[:6]!=b'SOUE01':p.error('Expected SOUE01 Wii game')
    spec=importlib.util.spec_from_file_location('maker',Path(__file__).with_name('make-link-mod.py'));maker=importlib.util.module_from_spec(spec);spec.loader.exec_module(maker)
    def entries(d):
        out={}
        for name,data in u8_files(d):
            path=PurePosixPath(name)
            if path.is_absolute() or '..' in path.parts or path.as_posix() in out:raise FormatError('Unsafe/duplicate U8 path')
            out[path.as_posix()]=data
        return out
    original_pack=decompress_lz((a.game_dir/'files/Object/ObjectPack.arc.LZ').read_bytes());outer=entries(original_pack);original_arc=outer['oarc/Alink.arc'];base=entries(original_arc)['g3d/model.brres']
    hd=entries(a.alink.read_bytes());source=hd['g3d/model.brres'];xtx=hd['g3d/model.xtx']
    a.out.parent.mkdir(parents=True,exist_ok=True)
    stage=a.out.parent/('.skybreak-hd-'+uuid.uuid4().hex);stage.mkdir()
    try:
        converted,rows,notes=convert_brres(source,xtx,base,stage/'textures',stage/'author-textures')
        alink=maker.repack_u8(original_arc,{'g3d/model.brres':converted});pack=maker.repack_u8(original_pack,{'oarc/Alink.arc':alink});compressed=compress_lz11(pack)
        assert decompress_lz(compressed)==pack
        new=entries(pack);assert all(new[n]==outer[n] for n in outer if n!='oarc/Alink.arc')
        dest=stage/'files/Object/ObjectPack.arc.LZ';dest.parent.mkdir(parents=True);dest.write_bytes(compressed)
        metadata={'schema':1,'id':a.id,'name':a.name,'game':'SOUE01','version':'0.1-experimental','author':a.author,'description':'Experimental SSHD BRRES+XTX asset conversion. Enable only one Link model mod; restart to apply.'}
        (stage/'mod.toml').write_text(''.join(f'{k} = {json.dumps(v,ensure_ascii=False)}\n' for k,v in metadata.items()),encoding='utf-8')
        (stage/'Alink.arc').write_bytes(alink)
        (stage/'conversion.json').write_text(json.dumps({'textures':rows,'metadata_notes':notes,'original_geometry_sha256':hashlib.sha256(source).hexdigest(),'converted_brres_sha256':hashlib.sha256(converted).hexdigest(),'objectpack_sha256':hashlib.sha256(compressed).hexdigest(),'other_archives_preserved':len(outer)-1,'compression':{'raw':len(pack),'packed':len(compressed)},'scope':'BRRES/MDL0 rev11 + XTX with preserved Wii Link rig; no code mods'},indent=2))
        stage.rename(a.out)
    except BaseException:
        assert stage.resolve().parent==a.out.resolve().parent and stage.name.startswith('.skybreak-hd-') and not stage.is_symlink()
        shutil.rmtree(stage);raise
    print(f'Converted {len(rows)} external textures and preserved Link rig: {a.out}. Requires native game testing.')

if __name__=='__main__':main()
