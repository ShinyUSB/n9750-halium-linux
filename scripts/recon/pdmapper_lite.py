#!/usr/bin/env python3
# pd-mapper-lite for the d2q Debian audio bring-up.
#
# The CAF kernel (4.14) locates process-domain services through a userspace
# "servreg locator" daemon announcing QRTR service 0x40 (pd-mapper on
# Android). The kernel (drivers/soc/qcom/service-locator.c) looks up 0x40,
# then sends QMI_SERVREG_LOC_GET_DOMAIN_LIST (0x0021) requests with
# service_name "avs/audio" (audio_pdr.c, slim-msm-ngd.c) and expects the
# ADSP audio-PD domain (name + instance) so the per-domain state notifier
# (service-notifier.c) can register with the ADSP's own servreg (0x42 inst
# 0x4a01 on node 5 = version 1, instance 0x4a).
#
# ns-lite carries a STATIC entry 0x40 -> (1, 0x50) so kernel lookups land
# here. Encoding per qmi_encdec.c: TLV = [type u8][len u16 LE][value],
# strings = [u8 strlen][bytes] (strlen <= 255), array count = [u16 LE].
import socket
import struct
import select
import sys
import time

PORT = 0x50
NAME = b"msm/adsp/audio_pd"
INSTANCE = 0x4A          # ADSP servreg instance on the QRTR bus


def tlv(t, val):
    # QMI TLV header = [type u8][len u16 LE][value] — 3 bytes, NO pad byte
    return struct.pack("<BH", t, len(val)) + val


def domain_list_resp(txn):
    # CAF kernel qmi_encdec: ONE TLV per optional element, NO flag byte
    # (the decoder sets valid=1 from the TLV's presence and decodes the
    # value from the same TLV; the encoder never serializes the flag).
    body = bytearray()
    # resp.result: error u16 + code u16 (mandatory struct)
    body += tlv(0x02, struct.pack("<HH", 0, 0))
    # total_domains (u16 LE)
    body += tlv(0x10, struct.pack("<H", 1))
    # db_rev_count (u16 LE)
    body += tlv(0x11, struct.pack("<H", 1))
    # domain_list TLV TYPE = 0x12 (per the kernel's resp ei table — 0x01
    # would fail find_ei as a mandatory TLV -> EINVAL). Value = [u16 element
    # count] + entries (QMI_DATA_LEN is u16 on wire).
    entry = struct.pack("<B", len(NAME)) + NAME \
        + struct.pack("<I", INSTANCE) + b"\x00" + struct.pack("<I", 0)
    # element count is a U8 on the wire (ei: .elem_size = sizeof(uint8_t))
    body += tlv(0x12, bytes([1]) + entry)
    # kernel qmi_header (qmi.h): [type u8][txn u16 LE][msg_id u16 LE][msg_len u16 LE] = 7 bytes
    hdr = struct.pack("<BHHH", 2, txn & 0xFFFF, 0x0021, len(body))
    return bytes(hdr) + bytes(body)


def parse_tlvs(data):
    # TLV header = [type u8][len u16 LE] = 3 bytes
    out = []
    i = 7  # kernel qmi_header = [type u8][txn u16][msgid u16][len u16] = 7 bytes
    while i + 3 <= len(data):
        t = data[i]
        l = struct.unpack_from("<H", data, i + 1)[0]
        out.append((t, data[i+3:i+3+l]))
        i += 3 + l
    return out


def main():
    s = socket.socket(42, 2)
    s.bind((1, PORT))
    s.setblocking(False)
    print("pd-mapper-lite: serving 0x40 on node 1 port 0x%x" % PORT, flush=True)
    deadline = time.time() + 86400
    while time.time() < deadline:
        r, _, _ = select.select([s], [], [], 10)
        if not r:
            continue
        try:
            data, addr = s.recvfrom(4096)
        except OSError:
            continue
        print("QMI< %s from node %d port 0x%x" % (data.hex(), addr[0], addr[1]), flush=True)
        if len(data) < 8:
            print("short/ctrl packet ignored", flush=True)
            continue
        qtype, txn, msgid, ln = struct.unpack("<BHHH", data[:7])
        name = ""
        for t, v in parse_tlvs(data):
            if t == 0x01:
                n = v[0]
                name = v[1:1+n].decode(errors="replace")
        if qtype == 0 and msgid == 0x0021:
            resp = domain_list_resp(txn)
            s.sendto(resp, addr)
            print("-> domain list for '%s' (txn %d)" % (name, txn), flush=True)


if __name__ == "__main__":
    main()
