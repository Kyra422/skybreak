"""Nintendo LZ11 encoder. Bounded 4 KiB history; standalone, no dependencies."""
from array import array

def compress(data):
    n=len(data)
    if not 0<n<=512*1024*1024:raise ValueError('Invalid LZ11 input size')
    out=bytearray(b'\x11'+(n if n<0x1000000 else 0).to_bytes(3,'little'))
    if n>=0x1000000:out.extend(n.to_bytes(4,'little'))
    previous=array('i',[-1])*n;heads={};p=0
    def key(i):return (data[i]<<16)|(data[i+1]<<8)|data[i+2]
    while p<n:
        flag_pos=len(out);out.append(0);flags=0
        for bit in range(7,-1,-1):
            if p>=n:break
            length=0;distance=0;limit=min(0x10110,n-p)
            if p+2<n:
                candidate=heads.get(key(p),-1);budget=16
                while candidate>=max(0,p-4096) and budget:
                    budget-=1
                    if length<limit and data[candidate+length]==data[p+length]:
                        j=3
                        while j+32<=limit and data[candidate+j:candidate+j+32]==data[p+j:p+j+32]:j+=32
                        while j<limit and data[candidate+j]==data[p+j]:j+=1
                        if j>length:length=j;distance=p-candidate
                    candidate=previous[candidate]
            if length>=3:
                flags|=1<<bit;d=distance-1
                if length<=16:out.extend((((length-1)<<4)|(d>>8),d&255))
                elif length<=272:
                    v=length-17;out.extend((v>>4,((v&15)<<4)|(d>>8),d&255))
                else:
                    v=length-273;out.extend((0x10|(v>>12),(v>>4)&255,((v&15)<<4)|(d>>8),d&255))
                advance=length
            else:out.append(data[p]);advance=1
            for i in range(p,min(p+advance,n-2)):
                k=key(i);previous[i]=heads.get(k,-1);heads[k]=i
            p+=advance
        out[flag_pos]=flags
    return bytes(out)
