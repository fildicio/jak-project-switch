#!/usr/bin/env python3
"""Dump MOD0 dynamic tags of an NRO (file arg)."""
import struct, sys

data = open(sys.argv[1], 'rb').read()
base = data.find(b'NRO0')
m = data.find(b'MOD0')
assert m > 0, 'no MOD0'
dyn_off, bss_start, bss_end = struct.unpack_from('<III', data, m + 4)
dyn = m + dyn_off
tags = {3: 'DT_PLTGOT', 7: 'DT_RELA', 8: 'DT_RELASZ', 9: 'DT_RELAENT', 20: 'DT_PLTREL',
        23: 'DT_JMPREL', 24: 'DT_BIND_NOW', 25: 'DT_INIT_ARRAY', 27: 'DT_FINI_ARRAY',
        30: 'DT_FLAGS', 36: 'DT_RELR', 37: 'DT_RELRSZ', 38: 'DT_RELRENT', 0x6ffffffb: 'DT_FLAGS_1'}
print(f"{sys.argv[1]}: NRO0@{base:#x} MOD0@{m:#x} dynamic@{dyn:#x} bss=[{m+bss_start:#x},{m+bss_end:#x})")
i = dyn
for _ in range(64):
    tag, val = struct.unpack_from('<QQ', data, i)
    if tag == 0:
        break
    name = tags.get(tag, hex(tag))
    extra = ''
    if tag == 30:  # DT_FLAGS
        extra = f"  (BIND_NOW={bool(val & 8)}, TEXTREL={bool(val & 4)})"
    if tag == 0x6ffffffb:
        extra = f"  (NOW={bool(val & 1)}, PIE={bool(val & 0x08000000)})"
    print(f"  {name:14} {val:#x}{extra}")
    i += 16
