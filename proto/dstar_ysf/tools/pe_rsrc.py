"""usage: python3 -I pe_rsrc.py <file.exe> <outdir>  (dump PE resources, never runs the file)"""
import struct, sys, os
def main(path, outdir):
    d = open(path,'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe+6)[0]
    optsz = struct.unpack_from('<H', d, pe+20)[0]
    sec = pe+24+optsz
    secs=[]
    for i in range(nsec):
        name = d[sec+40*i:sec+40*i+8].rstrip(b'\0').decode()
        vsz, va, rsz, raw = struct.unpack_from('<IIII', d, sec+40*i+8)
        secs.append((name,va,vsz,raw,rsz))
    rs = [s for s in secs if s[0]=='.rsrc'][0]
    base_raw, base_va = rs[3], rs[1]
    def name_of(off):
        n = struct.unpack_from('<H', d, base_raw+off)[0]
        return d[base_raw+off+2:base_raw+off+2+2*n].decode('utf-16le')
    def walk(off, path):
        _,_,_,_,nn,ni = struct.unpack_from('<IIHHHH', d, base_raw+off)
        for i in range(nn+ni):
            nid, ptr = struct.unpack_from('<II', d, base_raw+off+16+8*i)
            nm = name_of(nid & 0x7fffffff) if nid & 0x80000000 else str(nid)
            if ptr & 0x80000000:
                walk(ptr & 0x7fffffff, path+[nm])
            else:
                rva, size, cp, _ = struct.unpack_from('<IIII', d, base_raw+ptr)
                fo = rva - base_va + base_raw
                blob = d[fo:fo+size]
                fn = os.path.join(outdir, '_'.join(path+[nm]).replace('/','_'))
                open(fn,'wb').write(blob)
                print(f"{'/'.join(path+[nm]):40s} size {size:8d} head {blob[:16].hex()} {blob[:16]!r}")
    walk(0, [])
if len(sys.argv) != 3:
    sys.exit(__doc__)
main(sys.argv[1], sys.argv[2])
