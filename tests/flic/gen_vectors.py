# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Borys Pierov
"""Test vectors for components/flic/duo_codec.h (the Flic Duo event decoder).

Random event packets are encoded straight from the Flic Duo protocol specification ("Event encoding"),
decoded by a plain reference decoder written from the same text (which must reproduce the encoder
exactly), and written to vectors.txt for test_duo_codec.cpp. If pyflic-ble is importable it is
cross-checked too; it drops a packet's last event when that event starts mid-byte and is shorter
than 47 bits (its bit reader overcounts consumed bits by 8 there).

    python3 gen_vectors.py [seed] [packets]   ->  vectors.txt
"""
import random
import sys

try:
    from pyflic_ble.protocol import DuoParserState, _parse_duo_events_from_bytes
except ImportError:
    DuoParserState = None


COUNT_W = [2, 4, 8, 32]
TS_W = [8, 10, 13, 16, 24, 32, 40, 48]


class BitWriter:
    def __init__(self): self.bits = []
    def put(self, value, width):
        for i in range(width): self.bits.append((value >> i) & 1)
    def data(self):
        out = bytearray((len(self.bits) + 7) // 8)
        for i, b in enumerate(self.bits):
            if b: out[i >> 3] |= 1 << (i & 7)
        return bytes(out)

def encode_packet(rng, eoq):
    """Return (bytes, list of updates as encoded, eoq_after)."""
    w = BitWriter(); first = [True, True]; ups = []
    for _ in range(rng.randint(1, 4)):
        b = rng.randint(0, 1); w.put(b, 1)
        u = {"b": b}
        if first[b]:
            first[b] = False
            d = rng.choice([0, 1, 2, 3, 5, 14, 200, 70000, rng.randint(0, 2**31)])
            form = rng.random()
            if d == 0 and form < 0.7: w.put(0, 1)
            elif d == 1 and form < 0.7: w.put(1, 1); w.put(0, 1)
            else:
                w.put(1, 1); w.put(1, 1)
                idx = next(i for i, wd in enumerate(COUNT_W) if d < (1 << wd))
                w.put(idx, 2); w.put(d, COUNT_W[idx])
            u["diff"] = d
        ts = rng.choice([0, 3, 255, 256, 1000, 70000, 2**33 + 5, rng.randint(0, 2**47)])
        idx = next(i for i, wd in enumerate(TS_W) if ts < (1 << wd))
        idx = rng.randint(idx, 7)
        w.put(idx, 3); w.put(ts, TS_W[idx]); u["ts"] = ts
        queued = False
        if not eoq:
            r = rng.random()
            if r < 0.5: w.put(0, 1); queued = True
            else:
                w.put(1, 1); eoq = True
                last = rng.randint(0, 1); w.put(last, 1); queued = (last == 0)
        u["queued"] = queued
        t = rng.randint(0, 7); w.put(t, 3); u["type"] = t
        flag = 0
        if t in (4, 7): flag = rng.randint(0, 1); w.put(flag, 1)
        u["flag"] = flag
        g = -1
        if t <= 4 or t == 6:
            r = rng.random()
            if r < 0.4: w.put(0, 1)
            elif r < 0.55: w.put(1, 1); w.put(0, 1); g = -2
            else: g = rng.randint(0, 3); w.put(1, 1); w.put(1, 1); w.put(g, 2)
        u["gesture"] = g
        acc = [rng.randint(-128, 127) for _ in range(3)]
        for a in acc: w.put(a & 0xFF, 8)
        u["accel"] = acc
        ups.append(u)
    pad = rng.choice([0, 0, 1, 3, 7])  # stray padding bits, < 8
    w.put(0, pad)
    return w.data(), eoq, ups


class Trunc(Exception):
    pass

def ref_decode(data, ts, counts, eoq):
    """Returns (events, ts, counts, eoq). events: dicts b/type/flag/gesture/queued/accel/count/ts."""
    nbits = len(data) * 8
    pos = 0
    def bits(w):
        nonlocal pos
        if pos + w > nbits:
            raise Trunc()
        v = 0
        for i in range(w):
            if (data[(pos + i) >> 3] >> ((pos + i) & 7)) & 1:
                v |= 1 << i
        pos += w
        return v
    counts = list(counts)
    first = [True, True]
    out = []
    while nbits - pos >= 8:  # spec: stop when less than one full byte remains
        snap = (pos, ts, list(counts), eoq, list(first))
        try:
            b = bits(1)
            if first[b]:
                d = bits(1)
                if d and bits(1):
                    d = bits(COUNT_W[bits(2)])
                counts[b] += d + 1
                first[b] = False
            else:
                counts[b] += 1
            ts += bits(TS_W[bits(3)])
            queued = False
            if not eoq:
                if bits(1) == 0:
                    queued = True
                else:
                    eoq = True
                    queued = bits(1) == 0
            t = bits(3)
            flag = bits(1) if t in (4, 7) else 0
            g = -1
            if t <= 4 or t == 6:
                if bits(1):
                    g = bits(2) if bits(1) else -2
            acc = [bits(8) for _ in range(3)]
            acc = [a - 256 if a >= 128 else a for a in acc]
        except Trunc:
            pos, ts, counts, eoq, first = snap
            break
        if t <= 5 and counts[b] % 2 == 0:
            counts[b] += 1
        counts = [c & 0xFFFFFFFF for c in counts]
        out.append({"b": b, "type": t, "flag": flag, "gesture": g, "queued": int(queued),
                    "accel": acc, "count": counts[b], "ts": ts})
    return out, ts, counts, eoq


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    packets = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
    rng = random.Random(seed)
    enc_ok = same = dropped = 0
    with open("vectors.txt", "w") as f:
        for case in range(packets):
            counts0 = [rng.randint(0, 50), rng.randint(0, 2**32 - 100)]
            ts0 = rng.randint(0, 2**40)
            eoq0 = rng.random() < 0.5
            data, _, ups = encode_packet(rng, eoq0)
            truncated = rng.random() < 0.05
            if truncated:
                data = data[: max(1, len(data) - rng.randint(1, 3))]
            exp, ts1, counts1, eoq1 = ref_decode(data, ts0, counts0, eoq0)
            if not truncated:
                enc = [(u["b"], u["type"], u["flag"], u["gesture"], int(u["queued"]), u["accel"]) for u in ups]
                dec = [(e["b"], e["type"], e["flag"], e["gesture"], e["queued"], e["accel"]) for e in exp]
                assert enc == dec, (case, enc, dec)
                enc_ok += 1
            if DuoParserState is not None:
                st = DuoParserState()
                st.event_count, st.last_timestamp, st.has_processed_end_of_queue_marker = list(counts0), ts0, eoq0
                pev = _parse_duo_events_from_bytes(data, st)[0]
                pl = [(e.button_index, int(e.event_type), e.timestamp_ms) for e in pev]
                rl = [(e["b"], e["type"], e["ts"]) for e in exp]
                if pl == rl:
                    same += 1
                elif pl == rl[: len(pl)]:
                    dropped += 1
                else:
                    raise AssertionError(("pyflic-ble differs beyond a dropped tail", case, pl, rl))
            evs = ";".join(
                f'{e["b"]},{e["type"]},{e["flag"]},{e["gesture"]},{e["queued"]},'
                f'{e["accel"][0]},{e["accel"][1]},{e["accel"][2]},{e["count"]},{e["ts"]}'
                for e in exp
            )
            f.write(f"{data.hex()} {ts0} {counts0[0]} {counts0[1]} {int(eoq0)} {ts1} {counts1[0]} {counts1[1]} "
                    f"{int(eoq1)} {len(exp)} {evs or '-'}\n")
    print(f"{packets} packets; reference decoder == encoder on {enc_ok} complete packets")
    if DuoParserState is not None:
        print(f"pyflic-ble: identical on {same}, dropped trailing events on {dropped}")


if __name__ == "__main__":
    main()
