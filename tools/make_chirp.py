#!/usr/bin/env python3
"""Regenerate esp-idf/data/rrc/chirp.wav: two short rising notes, 16 kHz mono
16-bit like lxmf's ding, and audibly not it.

    python3 tools/make_chirp.py
"""
import math
import os
import struct
import wave

RATE = 16000


def note(freq_from, freq_to, ms, vol=0.45):
    n = int(RATE * ms / 1000)
    out, phase = [], 0.0
    for i in range(n):
        f = freq_from + (freq_to - freq_from) * i / n
        phase += 2 * math.pi * f / RATE
        env = min(1.0, i / 80) * min(1.0, (n - i) / 400)
        out.append(int(32767 * vol * env * math.sin(phase)))
    return out


samples = note(1320, 1560, 70) + [0] * int(RATE * 0.035) + note(1760, 2090, 110)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "esp-idf", "data", "rrc", "chirp.wav")
with wave.open(OUT, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(b"".join(struct.pack("<h", s) for s in samples))
