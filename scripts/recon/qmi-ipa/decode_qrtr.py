#!/usr/bin/env python3
# Decode the kernel qrtr_0 ipc log into a service/msg_id flow.
# Format per line: [ ts] TX/RX DATA: Len:0xNN CF:0xN src[0xN:0xP] dst[0xN:0xP] [u32 u32] [comm]
#   or TX/RX CTRL: cmd:0xN SVC[0xS:0xI] addr[0xN:0xP]
# QMI header in the DATA payload: [type u8][txn u16 LE][msg_id u16 LE][len u16 LE]
import re, sys

svc_by_port = {}   # (node,port) -> service id, learned from NEW_SERVER/announcements
lines = open(sys.argv[1], errors='replace').read().splitlines()
pat = re.compile(r'(TX|RX) (DATA|CTRL):(.*)')

rows = []
for ln in lines:
    m = pat.search(ln)
    if not m:
        continue
    ts = ln.split(']')[0].lstrip('[').strip()
    dirn, kind, rest = m.group(1), m.group(2), m.group(3)
    if kind == 'CTRL':
        cm = re.search(r'cmd:0x([0-9a-f]+)', rest)
        sm = re.search(r'SVC\[0x([0-9a-f]+):0x([0-9a-f]+)\]', rest)
        am = re.search(r'addr\[0x([0-9a-f]+):0x([0-9a-f]+)\]', rest)
        if cm and sm and am:
            cmd = int(cm.group(1),16); svc=int(sm.group(1),16); inst=int(sm.group(2),16)
            n=int(am.group(1),16); p=int(am.group(2),16)
            if cmd == 4:  # NEW_SERVER
                svc_by_port[(n,p)] = svc
            rows.append((ts,dirn,'CTRL',f"cmd={cmd} svc=0x{svc:x}/0x{inst:x} at {n}:0x{p:x}",None,None))
        continue
    dm = re.search(r'src\[0x([0-9a-f]+):0x([0-9a-f]+)\] dst\[0x([0-9a-f]+):0x([0-9a-f]+)\] \[([0-9a-f]{8}) ([0-9a-f]{8})\]', rest)
    lm = re.search(r'Len:0x([0-9a-f]+)', rest)
    cm = re.search(r'\[([^\]]+)\]\s*$', rest)
    if not dm:
        continue
    g=dm.groups(); sn,sp,dn,dp = int(g[0],16),int(g[1],16),int(g[2],16),int(g[3],16)
    w0 = int(g[4],16); w1 = int(g[5],16)
    b = bytes([w0&0xff,(w0>>8)&0xff,(w0>>16)&0xff,(w0>>24)&0xff,
               w1&0xff,(w1>>8)&0xff,(w1>>16)&0xff,(w1>>24)&0xff])
    qtype = b[0]; txn = b[1]|(b[2]<<8); msg = b[3]|(b[4]<<8); mlen = b[5]|(b[6]<<8)
    comm = cm.group(1) if cm else ''
    l = int(lm.group(1),16) if lm else 0
    # the receiving side's service: for TX, dst is the modem service; map by dst port
    port = dp if dirn=='TX' else sp
    svc = svc_by_port.get((0,port) if dirn=='TX' else (0,port), None)
    svcstr = f"svc=0x{svc:x}" if svc is not None else f"port=0x{port:x}"
    rows.append((ts,dirn,'DATA',f"{svcstr} msg=0x{msg:04x} type={qtype} mlen={mlen} len=0x{l:x}", msg, comm))

# Print the flow, skipping pure keepalives (len 0x7, all-zero)
for ts,dirn,kind,desc,msg,comm in rows:
    if kind=='DATA' and 'len=0x7' in desc and 'msg=0x0000' in desc:
        continue
    print(f"{dirn} {kind} {desc}  [{comm}]")
