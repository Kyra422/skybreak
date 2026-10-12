import struct

class FormatError(ValueError): pass

def check(data, pos, size):
    if pos < 0 or size < 0 or pos + size > len(data):
        raise FormatError(f'out of bounds: offset={pos:#x}, size={size:#x}, total={len(data):#x}')

def u32(d,p): check(d,p,4); return struct.unpack_from('>I',d,p)[0]

def string(d,p,limit=None):
    check(d,p,1)
    end=d.find(b'\0',p,min(len(d),p+4096,limit or len(d)))
    if end<0: raise FormatError(f'unterminated name at {p:#x}')
    return d[p:end].decode('utf-8',errors='backslashreplace')

def decompress_lz(data):
    """Nintendo LZ10/LZ11, including extended size and overlapping copies."""
    if data[:4]==b'LZ77': data=data[4:]
    check(data,0,4)
    kind=data[0]
    if kind not in (0x10,0x11): raise FormatError(f'unknown LZ method {kind:#x}')
    target=int.from_bytes(data[1:4],'little'); p=4
    if not target:
        check(data,4,4); target=int.from_bytes(data[4:8],'little'); p=8
    if not 0<target<=512*1024*1024: raise FormatError(f'invalid LZ output size {target}')
    out=bytearray()
    while len(out)<target:
        check(data,p,1); flags=data[p]; p+=1
        for bit in range(7,-1,-1):
            if len(out)==target: break
            check(data,p,1)
            if not flags&(1<<bit): out.append(data[p]); p+=1; continue
            first=data[p]; p+=1
            if kind==0x10:
                check(data,p,1); length=(first>>4)+3
                distance=((first&15)<<8|data[p])+1; p+=1
            elif first>>4==0:
                check(data,p,2); length=((first&15)<<4 | data[p]>>4)+0x11
                distance=((data[p]&15)<<8|data[p+1])+1; p+=2
            elif first>>4==1:
                check(data,p,3); length=((first&15)<<12|data[p]<<4|data[p+1]>>4)+0x111
                distance=((data[p+1]&15)<<8|data[p+2])+1; p+=3
            else:
                check(data,p,1); length=(first>>4)+1
                distance=((first&15)<<8|data[p])+1; p+=1
            if distance>len(out): raise FormatError(f'LZ reference before output start: {distance}>{len(out)}')
            if len(out)+length>target: raise FormatError('LZ copy exceeds declared output')
            # A repeated period is exactly equivalent to sequential overlapping copies.
            period=bytes(out[-distance:])
            out.extend((period*((length+distance-1)//distance))[:length])
    return bytes(out)

def u8_files(data):
    if data[:4]!=b'\x55\xaa\x38\x2d': raise FormatError('not U8')
    root=u32(data,4); header_size=u32(data,8); data_start=u32(data,12)
    check(data,root,12); count=u32(data,root+8)
    if data[root]!=1 or count<1 or count>1000000: raise FormatError('invalid U8 root')
    check(data,root,header_size); check(data,root,count*12)
    names=root+count*12; names_end=root+header_size
    if names>names_end or data_start<names_end: raise FormatError('U8 node/string overlap')
    stack=[(count,'')]
    for i in range(1,count):
        while stack and i>=stack[-1][0]: stack.pop()
        if not stack: raise FormatError('U8 entry beyond root')
        pos=root+i*12; packed=u32(data,pos); name_pos=names+(packed&0xffffff)
        if name_pos>=names_end: raise FormatError('U8 name outside string table')
        name=string(data,name_pos,names_end)
        # Names are provenance only: never used as output filesystem paths.
        path=f'{stack[-1][1]}/{name}'.lstrip('/')
        kind=packed>>24; start=u32(data,pos+4); size=u32(data,pos+8)
        if kind==1:
            if not i<size<=stack[-1][0]: raise FormatError('invalid U8 directory range')
            stack.append((size,path))
        elif kind==0:
            check(data,start,size)
            if start<data_start: raise FormatError('U8 file overlaps table')
            yield path,data[start:start+size]
        else: raise FormatError(f'unknown U8 node kind {kind}')
