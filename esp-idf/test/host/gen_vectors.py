#!/usr/bin/env python3
"""Regenerate vectors.h: CBOR encodings made by cbor2, the encoder rrcd uses.

    python3 -m pip install cbor2
    python3 gen_vectors.py > vectors.h
"""
import cbor2

VECTORS = [
    ("welcome", {0: 1, 1: 2, 2: bytes(range(8)), 3: 1730419200123, 4: bytes(16),
                 6: {0: "spangap test hub", 1: "0.3.2", 2: {0: True, 1: True, 2: True},
                     3: {0: 32, 1: 64, 2: 350, 3: 32, 4: 240}}}),
    ("msg", {0: 1, 1: 20, 2: b"\x01" * 8, 3: 5, 4: b"\xaa" * 16, 5: "#test",
             6: "héllo wörld", 7: "rop"}),
    ("joined", {0: 1, 1: 11, 2: b"\x02" * 8, 3: 70000, 4: b"\x03" * 16, 5: "#test",
                6: [b"\x04" * 16, b"\x05" * 16]}),
    ("announce", {"proto": "rrc", "v": 1, "hub": "spangap test hub"}),
    ("nested", {0: [1, [2, [3, None, False]], -5, 2 ** 40]}),
]

print("/* Generated with Python cbor2 — the reference encoder. */")
print("#pragma once\n#include <cstdint>\n#include <cstddef>")
for name, v in VECTORS:
    b = cbor2.dumps(v)
    print(f"static const uint8_t vec_{name}[] = {{{','.join(str(x) for x in b)}}};")
