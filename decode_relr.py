#!/usr/bin/env python3
"""Decode NRO RELR entries and report target segments."""
import struct, sys

path = sys.argv[1]
data = open(path, 'rb').read()
nb = data.find(b'NRO0')
t_off, t_sz, r_off, r_sz, d_off, d_sz, bss_sz = struct.unpack_from('<IIIIIII', data, nb + 0x10)
m = data.find(b'MOD0')
dyn_off = struct.unpack_from('<I', data, m + 4)[0]
i = m + dyn_off
relr = None
sizes = {}
for _ in range(64):
    tag, val = struct.unpack_from('<QQ', data, i)
    if tag == 0: break
    if tag == 36: relr = val
    sizes[tag] = val   # 35 and 37 both seen in the wild as RELRSZ-adjacent
    i += 16
relrsz = max(sizes.get(35, 0), sizes.get(37, 0))
print(f"{path}:\n  text@{t_off:#x}+{t_sz:#x}  ro@{r_off:#x}+{r_sz:#x}  data@{d_off:#x}+{d_sz:#x}  bss={bss_sz:#x}")
print(f"  DT_RELR@{relr:#x} size {relrsz:#x}")

def seg(a):
    if a < t_off + t_sz: return '.text'
    if a < r_off + r_sz: return '.ro(READ-ONLY!)'
    return '.data/.bss(RW)'

targets, addr = [], None
for k in range(relrsz // 8):
    w = struct.unpack_from('<Q', data, relr + k * 8)[0]
    if w & 1 == 0:
        addr = w
        targets.append(addr)
        addr += 8
    else:
        for bit in range(1, 64):
            if w >> bit & 1:
                targets.append(addr + 8 * (bit - 1))
        addr += 8 * 63
ro = [t for t in targets if r_off <= t < r_off + r_sz]
print(f"  reloc targets: {len(targets)} total, {len(ro)} in READ-ONLY seg")
for t in targets[:8]:
    print(f"    {t:#x} -> {seg(t)}")

