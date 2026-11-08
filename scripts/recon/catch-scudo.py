import gdb, struct

gdb.execute("set pagination off")
gdb.execute("catch syscall write")
gdb.execute("catch syscall writev")
gdb.execute("catch syscall sendto")
gdb.execute("catch syscall sendmsg")
gdb.execute("catch signal SIGABRT")
gdb.execute("run")

def read_buf(addr, n):
    try:
        return gdb.selected_inferior().read_memory(addr, min(n, 512)).tobytes()
    except Exception:
        return b""

def check(data):
    return b"Scudo" in data or b"scudo" in data

for _ in range(8000):
    try:
        gdb.execute("continue")
    except gdb.error:
        break
    inf = gdb.selected_inferior()
    if not inf.is_valid() or not gdb.selected_thread():
        break
    try:
        frame = gdb.selected_frame()
        r1 = int(frame.read_register("r1"))
        r2 = int(frame.read_register("r2"))
    except Exception:
        continue
    found = b""
    # direct buffer forms: write/sendto -> r1=buf, r2=len
    d = read_buf(r1, r2 if 0 < r2 < 4096 else 512)
    if check(d):
        found = d
    else:
        # iovec forms: writev/sendmsg -> r1=iov or msghdr; scan first few {base,len}
        hdr = read_buf(r1, 128)
        for off in range(0, 96, 8):
            try:
                base, ln = struct.unpack("<II", hdr[off:off+8])
            except Exception:
                break
            if 0x10000 < base < 0xfff00000 and 0 < ln < 4096:
                d = read_buf(base, ln)
                if check(d):
                    found = d
                    break
    if found:
        print("\n==== SCUDO ERROR WRITE DETECTED ====")
        print("thread:", gdb.selected_thread().num, gdb.selected_thread().name)
        print("payload:", found[:400])
        gdb.execute("info registers r0 r1 r2 r3 lr pc sp")
        gdb.execute("bt")
        gdb.execute("info threads")
        gdb.execute("x/128xw $sp")
        break
print("==== DONE ====")
