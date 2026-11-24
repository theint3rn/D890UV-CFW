"""usage: python3 -I blob_stats.py <blob>  (entropy, repeated blocks, periodicity)"""
import sys, math, collections
if len(sys.argv) != 2:
    sys.exit(__doc__)
d=open(sys.argv[1],'rb').read()
print(len(d), hex(len(d)))
def ent(b):
    c=collections.Counter(b); n=len(b)
    return -sum(v/n*math.log2(v/n) for v in c.values())
for off in range(0,len(d),len(d)//16):
    print(hex(off), round(ent(d[off:off+65536]),3))
print('head', d[:128].hex())
print('tail', d[-160:].hex())
# most common 16-byte blocks
c=collections.Counter(d[i:i+16] for i in range(0,len(d)-16,16))
for k,v in c.most_common(6): print(v,k.hex())
# check periodicity of repeats: autocorrelation of equality for small periods
for p in (2,4,8,16,32,64,128,256,512,1024,2048,4096):
    eq=sum(1 for i in range(0,200000) if d[i]==d[i+p])
    print('period',p,eq)
