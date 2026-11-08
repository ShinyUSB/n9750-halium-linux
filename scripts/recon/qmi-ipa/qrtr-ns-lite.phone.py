#!/usr/bin/env python3
# Minimal QRTR name-server proxy for the d2q (SM8150, kernel 4.14 Samsung fork).
#
# Samsung's qrtr_sendmsg silently DROPS userspace NEW_SERVER announcements to
# the local ctrl port (qrtr_must_forward: same net_id -> false), so no daemon
# can register services the stock way. This NS binds the local ctrl port,
# CACHES every NEW_SERVER it sees (the modem announces its whole QMI service
# stack to us), answers lookups from that cache plus a STATIC table
# (rmtfs = service 0x0e on node 1 port 0xe), periodically re-announces static
# services directly to the modem, and relays ctrl packets for debugging.
# Survives dead-modem windows (the modem used to crash-loop; keep robust).
import socket
import struct
import select
import sys
import time

CTRL = 0xFFFFFFFE
MODEM = (0, CTRL)

CMD_NEW_SERVER = 4
CMD_NEW_LOOKUP = 10

# service -> (node, port) served statically:
#   rmtfs on node 1 port 0xe (the real rmtfs daemon)
#   fake CTL on node 1 port 0x99 (qmi-fake-ctl.py; the modem never announces CTL)
STATIC = {0x0E: (1, 0x0E), 0x00: (1, 0x99), 0x40: (1, 0x50)}


def pkt(cmd, a=0, b=0, c=0, d=0):
    return struct.pack("<IIIII", cmd, a, b, c, d)


def parse(data):
    if len(data) < 20:
        return None, None
    cmd = struct.unpack("<I", data[:4])[0]
    return cmd, struct.unpack("<IIII", data[4:20])


def log(msg):
    print("%d %s" % (int(time.time()), msg), flush=True)


def main():
    ns = socket.socket(42, 2)
    ns.bind((1, CTRL))
    ns.setblocking(False)
    log("bound ctrl port on node 1")

    cache = {}  # (svc, inst) -> (node, port), from every NEW_SERVER we see
    lookups = {}  # (node, port) -> {svc: requested_instance} the client registered
    temps = {}  # temp socket -> (client_addr, deadline)
    last_announce = 0
    last_hello = 0
    while True:
        now = time.time()
        # HELLO the modem's NS on start and every 30 s (some NS ignore
        # clients that never said hello), then re-announce static services
        if now - last_hello > 30:
            last_hello = now
            try:
                h = socket.socket(42, 2)
                h.bind((1, 0))
                h.sendto(pkt(2), MODEM)
                h.close()
            except OSError:
                pass
        if now - last_announce > 5:
            last_announce = now
            for svc, (node, port) in STATIC.items():
                try:
                    a = socket.socket(42, 2)
                    a.bind((1, 0))
                    a.sendto(pkt(CMD_NEW_SERVER, svc, 1, node, port), MODEM)
                    a.close()
                except OSError:
                    pass  # modem down; retry next round
        r, _, _ = select.select([ns] + list(temps), [], [], 2)
        for s in r:
            try:
                data, addr = s.recvfrom(4096)
            except OSError:
                continue
            cmd, f = parse(data)
            if s is ns:
                if cmd == CMD_NEW_SERVER:
                    cache[(f[0], f[1])] = (f[2], f[3])
                    if f[0] not in STATIC and addr[0] == 0:
                        log("cache: modem svc 0x%x inst 0x%x -> node %d port 0x%x"
                            % (f[0], f[1], f[2], f[3]))
                    # push to clients that registered a lookup for this svc
                    # (kernel qmi sockets expect NS push notifications; their
                    # one-shot NEW_LOOKUP happens before daemons start)
                    for (lnode, lport), svcs in list(lookups.items()):
                        want = svcs.get(f[0], None)
                        if want is None or (want and f[1] != want):
                            continue
                        try:
                            ns.sendto(pkt(CMD_NEW_SERVER, f[0], f[1], f[2], f[3]), (lnode, lport))
                            log("pushed svc 0x%x inst 0x%x to %d:0x%x" % (f[0], f[1], lnode, lport))
                        except OSError:
                            pass
                elif cmd == CMD_NEW_LOOKUP:
                    svc, inst = f[0], f[1]
                    lookups.setdefault((addr[0], addr[1]), {})[svc] = inst
                    log("lookup svc 0x%x inst 0x%x from node %d port 0x%x" % (svc, inst, addr[0], addr[1]))
                    hits = []
                    if svc == 0:
                        # bus enumeration: dump everything (libqrtr-glib's
                        # initial lookup is svc 0 and expects every server)
                        for (csvc, cinst), (cnode, cport) in cache.items():
                            hits.append((csvc, cinst, cnode, cport))
                        for s2, (n2, p2) in STATIC.items():
                            hits.append((s2, 0, n2, p2))
                    else:
                        if svc in STATIC:
                            hits.append((svc, inst, STATIC[svc][0], STATIC[svc][1]))
                        for (csvc, cinst), (cnode, cport) in cache.items():
                            if csvc != svc:
                                continue
                            # kernel QMI lookups carry a specific instance
                            # (version | pd_id << 8): honour it, else the
                            # kernel binds to the WRONG subsystem servreg
                            if inst and cinst and cinst != inst:
                                continue
                            hits.append((csvc, cinst, cnode, cport))
                    for (csvc, cinst, cnode, cport) in hits:
                        try:
                            ns.sendto(pkt(CMD_NEW_SERVER, csvc, cinst, cnode, cport), addr)
                        except OSError as e:
                            log("reply failed: %s" % e)
                    # end-of-dump marker: libqrtr-glib finishes the initial
                    # lookup when it sees an all-zero NEW_SERVER packet
                    try:
                        ns.sendto(pkt(CMD_NEW_SERVER, 0, 0, 0, 0), addr)
                    except OSError as e:
                        log("terminator failed: %s" % e)
                    if not hits:
                        log("no hit for svc 0x%x (cache %d entries)" % (svc, len(cache)))
                else:
                    log("ctrl cmd %d from node %d port 0x%x" % (cmd, addr[0], addr[1]))
            else:
                # late reply from a proxied query -> relay to requester
                if s in temps:
                    try:
                        ns.sendto(data, temps[s][0])
                    except OSError as e:
                        log("relay failed: %s" % e)
                s.close()
                temps.pop(s, None)
        for t in [t for t, (_, dl) in temps.items() if time.time() > dl]:
            t.close()
            temps.pop(t, None)


if __name__ == "__main__":
    try:
        main()
    except OSError as e:
        print("qrtr-ns-lite fatal: %s" % e, file=sys.stderr)
        sys.exit(1)
