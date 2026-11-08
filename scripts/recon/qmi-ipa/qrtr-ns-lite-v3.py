#!/usr/bin/env python3
# Minimal QRTR name-server proxy for the d2q (SM8150, kernel 4.14 Samsung fork).
#
# v3 (2026-11-06) — QMI_IPA reachability fix:
#  * Local service registrations (kernel QMI servers: QMI_IPA 0x31, memshare
#    0x34, ...) arrive with a STALE payload: the kernel QMI layer caches the
#    socket address before the QRTR autobind assigns the real ephemeral port,
#    so the NEW_SERVER payload says port 0. The real endpoint is the SOURCE
#    address of the control packet. For local senders (node 1) we therefore
#    cache addr, not the payload.
#  * Learned local services are now ANNOUNCED to the modem (the modem never
#    sends us NEW_LOOKUP; push is the only way it discovers AP services).
#    This is what lets the modem reach the AP's QMI_IPA server: it sends
#    INDICATION_REGISTER / INIT_CMPLT / INSTALL_FLTR_RULE -> DL filters.
#  * DEL_CLIENT/DEL_SERVER/BYE are handled so dead services are dropped and
#    de-announced (socket teardown only emits DEL_CLIENT on this fork).
#
# Older notes (still true): Samsung's qrtr_sendmsg drops userspace NEW_SERVER
# to the local ctrl port when the payload names a remote node; the kernel
# never relays local registrations to remote nodes itself (qrtr_sendmsg ->
# qrtr_local_enqueue only), so this daemon is the modem-facing NS.
import socket
import struct
import select
import sys
import time

CTRL = 0xFFFFFFFE
MODEM = (0, CTRL)
LOCAL_NID = 1

CMD_NEW_SERVER = 4
CMD_DEL_SERVER = 5
CMD_DEL_CLIENT = 6
CMD_BYE = 3
CMD_NEW_LOOKUP = 10

# service -> (node, port) served statically:
#   rmtfs on node 1 port 0xe (the real rmtfs daemon)
#   fake CTL on node 1 port 0x99 (qmi-fake-ctl.py; the modem never announces CTL)
#   servreg locator on node 1 port 0x50 (pdmapper_lite.py)
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


def announce(svc, inst, node, port, cmd=CMD_NEW_SERVER):
    """Announce/de-announce one service to the modem's NS."""
    try:
        a = socket.socket(42, 2)
        a.bind((1, 0))
        a.sendto(pkt(cmd, svc, inst, node, port), MODEM)
        a.close()
    except OSError:
        pass  # modem down; the periodic loop retries


def main():
    ns = socket.socket(42, 2)
    ns.bind((1, CTRL))
    ns.setblocking(False)
    log("bound ctrl port on node 1 (v3: source-addr local cache + local push)")

    cache = {}       # (svc, inst) -> (node, port), every live service we know
    local_svcs = {}  # (svc, inst) -> (node, port), local registrations only
    lookups = {}     # (node, port) -> {svc: requested_instance}
    temps = {}       # temp socket -> (client_addr, deadline)
    last_announce = 0
    last_hello = 0
    while True:
        now = time.time()
        # HELLO the modem's NS on start and every 30 s (some NS ignore
        # clients that never said hello)
        if now - last_hello > 30:
            last_hello = now
            try:
                h = socket.socket(42, 2)
                h.bind((1, 0))
                h.sendto(pkt(2), MODEM)
                h.close()
            except OSError:
                pass
        # Periodically re-announce static + learned-local services directly
        # to the modem: recovers from missed one-shots and modem restarts.
        if now - last_announce > 5:
            last_announce = now
            for svc, (node, port) in STATIC.items():
                announce(svc, 1, node, port)
            for (svc, inst), (node, port) in local_svcs.items():
                announce(svc, inst, node, port)
        r, _, _ = select.select([ns] + list(temps), [], [], 2)
        for s in r:
            try:
                data, addr = s.recvfrom(4096)
            except OSError:
                continue
            cmd, f = parse(data)
            if s is ns:
                if cmd == CMD_NEW_SERVER:
                    if addr[0] == LOCAL_NID:
                        # Local registration: trust the SOURCE address, never
                        # the payload (kernel QMI caches port 0 pre-autobind).
                        anode, aport = addr[0], addr[1]
                        new = cache.get((f[0], f[1])) != (anode, aport)
                        cache[(f[0], f[1])] = (anode, aport)
                        if f[0] not in STATIC:
                            local_svcs[(f[0], f[1])] = (anode, aport)
                            log("local svc 0x%x inst 0x%x -> %d:0x%x "
                                "(payload said %d:0x%x)"
                                % (f[0], f[1], anode, aport, f[2], f[3]))
                            if new and aport != 0:
                                announce(f[0], f[1], anode, aport)
                                log("announced local svc 0x%x to modem"
                                    % f[0])
                    else:
                        anode, aport = f[2], f[3]
                        cache[(f[0], f[1])] = (anode, aport)
                        if f[0] not in STATIC:
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
                            ns.sendto(pkt(CMD_NEW_SERVER, f[0], f[1], anode, aport),
                                      (lnode, lport))
                            log("pushed svc 0x%x inst 0x%x to %d:0x%x"
                                % (f[0], f[1], lnode, lport))
                        except OSError:
                            pass
                elif cmd == CMD_DEL_SERVER:
                    # [cmd][svc][inst][node][port]
                    key = (f[0], f[1])
                    was_local = key in local_svcs
                    cache.pop(key, None)
                    local_svcs.pop(key, None)
                    log("del svc 0x%x inst 0x%x (local=%d)" % (f[0], f[1], was_local))
                    if was_local:
                        announce(f[0], f[1], f[2], f[3], cmd=CMD_DEL_SERVER)
                elif cmd == CMD_DEL_CLIENT:
                    # [cmd][node][port][..][..] — a socket died; kernel QMI
                    # servers only ever signal teardown this way (no DEL_SERVER
                    # is sent by the QMI layer on this fork).
                    dead = (f[0], f[1])
                    for key, np in list(local_svcs.items()):
                        if np == dead:
                            cache.pop(key, None)
                            local_svcs.pop(key, None)
                            log("local svc 0x%x inst 0x%x died with %d:0x%x; "
                                "de-announcing" % (key[0], key[1], f[0], f[1]))
                            announce(key[0], key[1], f[0], f[1],
                                     cmd=CMD_DEL_SERVER)
                    for key, np in list(cache.items()):
                        if np == dead:
                            cache.pop(key, None)
                            log("dropped dead svc 0x%x inst 0x%x at %d:0x%x"
                                % (key[0], key[1], f[0], f[1]))
                elif cmd == CMD_BYE and addr[0] == 0:
                    # modem going down: its services are gone; ours are not.
                    for key, np in list(cache.items()):
                        if np[0] == 0:
                            cache.pop(key, None)
                    log("modem BYE: dropped modem-side cache (%d local kept)"
                        % len(local_svcs))
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
