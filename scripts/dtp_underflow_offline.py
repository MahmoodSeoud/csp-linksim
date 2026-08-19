import struct
PAY = 256 - 4
def parse(path):
    b = open(path,'rb').read()
    print(f"{path}: {len(b)} bytes")
    # header: ver u32, node u16, mtu u16, timeout u8, throughput u32, payload_id u16,
    #         bytes_received u32, payload_size u32, then nof_segments u8, then pairs
    off=0
    ver,=struct.unpack_from('<I',b,off); off+=4
    node,mtu=struct.unpack_from('<HH',b,off); off+=4
    timeout,=struct.unpack_from('<B',b,off); off+=1
    thr,=struct.unpack_from('<I',b,off); off+=4
    pid,=struct.unpack_from('<H',b,off); off+=2
    brx,=struct.unpack_from('<I',b,off); off+=4
    psz,=struct.unpack_from('<I',b,off); off+=4
    nseg,=struct.unpack_from('<B',b,off); off+=1
    print(f"  ver={ver} node={node} mtu={mtu} timeout={timeout} thr={thr} pid={pid} bytes_received={brx} payload_size={psz} nof_segments={nseg}")
    rem=(len(b)-off)
    print(f"  bytes left for intervals={rem} => {rem/8:.1f} pairs; nof_segments says {nseg}")
    ivs=[]
    for i in range(rem//8):
        s,e=struct.unpack_from('<II',b,off); off+=8; ivs.append((s,e))
    return nseg, ivs, psz

def cts(ivs, filesz):
    size=0
    for s,e in ivs:
        if e==0xFFFFFFFF: size=(size+((filesz-(s*PAY))&0xFFFFFFFF))&0xFFFFFFFF
        else: size=(size+(((e*PAY)-(s*PAY))&0xFFFFFFFF))&0xFFFFFFFF
    return size

for tag,path,logged in [
  ('loss0','captures/evidence/rawdtp_trace_loss0_20260816/dtp_session_meta.bin',3871951764),
  ('0719', 'captures/evidence/rawdtp_trace_20260719/dtp_session_meta.bin',      3090533264)]:
    print(f"===== {tag} (logged size_in_bytes={logged}) =====")
    nseg,ivs,psz=parse(path)
    print(f"  intervals: {ivs}")
    # use nof_segments count (what the client actually sends)
    used=ivs[:nseg]
    print(f"  compute_transfer_size over nof_segments={nseg}: {cts(used,psz)}  logged={logged}  MATCH={cts(used,psz)==logged}")
    print(f"  compute_transfer_size over ALL stored pairs: {cts(ivs,psz)}")
    inv=[(s,e) for s,e in used if e!=0xFFFFFFFF and e<s]
    print(f"  inverted (end<start) among sent: {inv}")
