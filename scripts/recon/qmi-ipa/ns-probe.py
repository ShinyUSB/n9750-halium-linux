#!/usr/bin/env python3
# Read-only QRTR NS probe: ask ns-lite what it knows about a service.
# Wire format (this kernel): ctrl pkt = 5 x u32 LE [cmd, a(svc), b(inst), c(node), d(port)]
# NEW_LOOKUP=10, NEW_SERVER=4. NS lives at (node 1, port 0xfffffffe).
import socket, struct, select, sys

NS = (1, 0xFFFFFFFE)
CMD_NEW_SERVER = 4
CMD_NEW_LOOKUP = 10

def pkt(cmd, a=0, b=0, c=0, d=0):
    return struct.pack("<IIIII", cmd, a, b, c, d)

def main():
    svc = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x31
    inst = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    s = socket.socket(42, 2)          # AF_QIPCRTR, SOCK_DGRAM
    s.bind((1, 0))                    # ephemeral local port
    s.setblocking(False)
    s.sendto(pkt(CMD_NEW_LOOKUP, svc, inst), NS)
    print("sent NEW_LOOKUP svc=0x%x inst=0x%x from %s" % (svc, inst, s.getsockname()))
    end = None
    r, _, _ = select.select([s], [], [], 3)
    while r:
        data, addr = s.recvfrom(4096)
        if len(data) >= 20:
            cmd = struct.unpack("<I", data[:4])[0]
            f = struct.unpack("<IIII", data[4:20])
            print("  reply cmd=%d svc=0x%x inst=0x%x -> node %d port 0x%x (from %s)"
                  % (cmd, f[0], f[1], f[2], f[3], addr))
        else:
            print("  reply (short, %d bytes) from %s" % (len(data), addr))
        r, _, _ = select.select([s], [], [], 1)
    s.close()

if __name__ == "__main__":
    main()
