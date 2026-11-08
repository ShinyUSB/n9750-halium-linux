#!/usr/bin/env python3
# wda_raw.py — WDA SET_DATA_FORMAT over raw AF_QIPCRTR (the stock CCI/QRTR wire),
# byte-exact stock msmnile request INCLUDING the TLVs libqmi 1.32 cannot express
# (flow_control=1, dl_minimum_padding=0).
#
# --print-only : build + hex-dump the exact request, DO NOT SEND (dry-run evidence).
# (no flag)    : NS-lookup WDA (0x1A), send once, hex-dump request+response.
#
# Framing = the proven CAF QRTR QMI wire (identical to the captured libqmi request):
#   [type u8][txn u16 LE][msg u16 LE][len u16 LE] + TLVs [tid u8][len u16 LE][val]
import socket, struct, sys, time

QRTR_NS_PORT = 1          # QRTR name-service well-known port
QRTR_TYPE_NEW_LOOKUP  = 2
QRTR_TYPE_DEL_LOOKUP  = 3
QRTR_TYPE_LOOKUP_BY_NAME = 5   # by-name
QRTR_TYPE_LOOKUP_BY_ID   = 6   # by-id  (CAF: 5/6; see qrtr.h)
WDA_SERVICE = 0x1A
WDA_SET_DATA_FORMAT = 0x0020

def tlv(tid, val):
    return struct.pack("<BH", tid, len(val)) + val

def build_request(fc_tlv=0x1A):
    """Stock msmnile WDA SET_DATA_FORMAT. TLV ids verified against the local
    stock IDL (libqmiservices WDA request ei table): fc=0x1A(u8), pad=0x19(u32,
    omitted — 0 is the modem default), qos_header_format=0x18 (not used).
    TLV order matches the captured libqmi request; fc appended last."""
    t = b""
    t += tlv(0x1C, struct.pack("<I", 16384))      # ul_data_aggregation_max_size
    t += tlv(0x1B, struct.pack("<I", 32))         # ul_data_aggregation_max_datagrams
    t += tlv(0x17, struct.pack("<II", 4, 1))      # endpoint_info: EMBEDDED, iface 1
    t += tlv(0x16, struct.pack("<I", 16384))      # dl_data_aggregation_max_size
    t += tlv(0x15, struct.pack("<I", 31))         # dl_data_aggregation_max_datagrams
    t += tlv(0x13, struct.pack("<I", 8))          # dl_data_aggregation_protocol = QMAPV4
    t += tlv(0x12, struct.pack("<I", 8))          # ul_data_aggregation_protocol = QMAPV4
    t += tlv(0x11, struct.pack("<I", 2))          # link_layer_protocol = RAW_IP
    t += tlv(fc_tlv, struct.pack("<B", 1))        # flow_control = 1 (stock, 0x1A)
    hdr = struct.pack("<BHHH", 0, 1, WDA_SET_DATA_FORMAT, len(t))
    return hdr + t

def hexdump(tag, b):
    print("%s (%d bytes):" % (tag, len(b)))
    for i in range(0, len(b), 16):
        print("  " + " ".join("%02x" % x for x in b[i:i+16]))

def ns_lookup(sock, service):
    """QRTR NS lookup via this phone's ns-lite (NEW_LOOKUP -> NEW_SERVER pushes
    + all-zero terminator). Returns (node, port) of the WDA service or None."""
    NS = (1, 0xFFFFFFFE)
    msg = struct.pack("<IIIII", 10, service, 0, 0, 0)   # CMD_NEW_LOOKUP
    sock.sendto(msg, NS)
    sock.settimeout(3.0)
    found = None
    try:
        while True:
            data, addr = sock.recvfrom(2048)
            if len(data) < 20:
                continue
            cmd, = struct.unpack_from("<I", data, 0)
            svc, inst, node, port = struct.unpack_from("<IIII", data, 4)
            if cmd == 4 and svc == 0 and inst == 0 and node == 0 and port == 0:
                break                              # terminator
            if cmd == 4 and svc == service:
                found = (node, port)
    except socket.timeout:
        pass
    return found

def main():
    print_only = "--print-only" in sys.argv
    req = build_request()
    hexdump("REQUEST", req)
    if print_only:
        print("dry-run: NOT sent.")
        return 0
    s = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM, 0)
    s.bind((1, 0))
    tgt = ns_lookup(s, WDA_SERVICE)
    print("WDA service ->", tgt)
    if not tgt:
        print("NS lookup failed"); return 1
    s.sendto(req, tgt)
    s.settimeout(5.0)
    try:
        resp, addr = s.recvfrom(2048)
        hexdump("RESPONSE", resp)
        # decode result TLV
        if len(resp) >= 14:
            tid, ln = struct.unpack_from("<BH", resp, 7)
            if tid == 2 and ln == 4:
                result, err = struct.unpack_from("<HH", resp, 10)
                print("result=%d error=%d (%s)" % (result, err,
                      "SUCCESS" if result == 0 else "FAILURE"))
    except socket.timeout:
        print("no response (timeout)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
