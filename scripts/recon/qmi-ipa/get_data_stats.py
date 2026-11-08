#!/usr/bin/env python3
# get_data_stats.py — query the MODEM's own IPA data statistics over QMI_IPA
# (svc 0x31, the Q6/modem instance on node 0). READ-ONLY probe.
# Answers: does the modem's IPA see our UL packets (ul_src_pipe), does it
# forward DL (dl_dst_pipe), and do our DL filter rules ever hit (dl_filter_rule)?
#
# Wire: QMI over AF_QIPCRTR = 7B hdr [type u8][txn u16 LE][msg u16 LE][len u16 LE]
#       + TLVs [tid u8][len u16 LE][val]. Same wire as wda_raw.py.
import socket, struct, sys

IPA_SERVICE = 0x31
GET_DATA_STATS = 0x0030
STATS_TYPE_PIPE = 1
STATS_TYPE_FILTER_RULES = 2

def tlv(tid, val):
    return struct.pack("<BH", tid, len(val)) + val

def build(stats_type):
    t = tlv(0x01, struct.pack("<I", stats_type))   # ipa_stats_type (mandatory)
    return struct.pack("<BHHH", 0, 1, GET_DATA_STATS, len(t)) + t

def hexdump(tag, b):
    print("%s (%d bytes):" % (tag, len(b)))
    for i in range(0, len(b), 16):
        print("  " + " ".join("%02x" % x for x in b[i:i+16]))

def ns_lookup_modem(sock, service):
    """NEW_LOOKUP, pick the node-0 (modem) server entry, not the AP's own."""
    NS = (1, 0xFFFFFFFE)
    sock.sendto(struct.pack("<IIIII", 10, service, 0, 0, 0), NS)
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
                break
            if cmd == 4 and svc == service and node == 0:   # modem side only
                found = (node, port)
    except socket.timeout:
        pass
    return found

def parse_stats(resp):
    # walk TLVs after the 7-byte header; result TLV 0x02 first
    off = 7
    while off + 3 <= len(resp):
        tid, ln = struct.unpack_from("<BH", resp, off)
        val = resp[off+3:off+3+ln]
        if tid == 0x02 and ln == 4:
            result, err = struct.unpack_from("<HH", val, 0)
            print("  result=%d error=%d (%s)" % (result, err,
                  "SUCCESS" if result == 0 else "FAILURE"))
        elif tid in (0x10, 0x11, 0x12):
            name = {0x10: "ul_src_pipe", 0x11: "dl_dst_pipe", 0x12: "dl_filter_rule"}[tid]
            # each list: [u32 count][entries...]; pipe entry = {u32 pipe, u64 pkts? ,...}
            if ln >= 4:
                cnt = struct.unpack_from("<I", val, 0)[0]
                print("  %s_list: count=%d (%d bytes)" % (name, cnt, ln))
                # best-effort: dump the list contents
                body = val[4:]
                for i in range(0, min(len(body), 64), 4):
                    print("      " + " ".join("%02x" % x for x in body[i:i+4]))
        off += 3 + ln

def main():
    st = STATS_TYPE_FILTER_RULES if "--filter" in sys.argv else STATS_TYPE_PIPE
    s = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM, 0)
    s.bind((1, 0))
    tgt = ns_lookup_modem(s, IPA_SERVICE)
    print("modem QMI_IPA service ->", tgt)
    if not tgt:
        print("NS lookup (node 0) failed"); return 1
    req = build(st)
    hexdump("REQUEST(stats_type=%d)" % st, req)
    s.sendto(req, tgt)
    s.settimeout(5.0)
    try:
        resp, addr = s.recvfrom(4096)
        hexdump("RESPONSE", resp)
        parse_stats(resp)
    except socket.timeout:
        print("no response (timeout)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
