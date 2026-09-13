#!/usr/bin/env python3
"""Map NRO image-relative offsets to ELF vaddrs (gk.nro -> gk ELF)."""
import struct, sys

nro_path, elf_path = sys.argv[1], sys.argv[2]
addrs = [int(a, 16) for a in sys.argv[3:]]

# --- NRO header (libnx): magic@0, size@8, text{off,size}@0x10, ro@0x18, data@0x20, bsssize@0x28
nro_full = open(nro_path, 'rb').read(0x100)
nro_base = nro_full.find(b'NRO0')
assert nro_base >= 0, 'no NRO0 magic in first 0x100 bytes'
nro = nro_full[nro_base:]
if nro_base: print(f"NRO0 magic at file offset {nro_base:#x} (HOMEBREW ABI prefix)")
t_off, t_sz, r_off, r_sz, d_off, d_sz, bss_sz = struct.unpack_from('<IIIIIII', nro, 0x10)
print(f"NRO: text@{t_off:#x}+{t_sz:#x}  ro@{r_off:#x}+{r_sz:#x}  data@{d_off:#x}+{d_sz:#x}  bss={bss_sz:#x}")

# --- ELF program headers (64-bit LE)
elf = open(elf_path, 'rb').read()
e_phoff, e_phentsize, e_phnum = struct.unpack_from('<Q', elf, 0x20)[0], struct.unpack_from('<H', elf, 0x36)[0], struct.unpack_from('<H', elf, 0x38)[0]
segs = []
for i in range(e_phnum):
    ph = elf[e_phoff + i*e_phentsize : e_phoff + (i+1)*e_phentsize]
    p_type, p_flags, p_offset, p_vaddr, _pa, p_filesz, p_memsz, _al = struct.unpack_from('<IIQQQQQQ', ph, 0)
    if p_type == 1:  # PT_LOAD
        fl = ''.join(c for c, b in (('R', 4), ('W', 2), ('X', 1)) if p_flags & b)
        segs.append((p_offset, p_filesz, p_memsz, p_vaddr, fl))
        print(f"ELF LOAD: file@{p_offset:#x}+{p_filesz:#x} mem={p_memsz:#x} vaddr={p_vaddr:#x} [{fl}]")

# --- map: NRO seg order text,ro,data -> ELF LOADs sorted by vaddr (RX, R, RW)
nro_segs = [(t_off, t_sz, 'RX'), (r_off, r_sz, 'R'), (d_off, d_sz, 'RW')]
elf_by_flag = {fl: s for s in segs if (fl := s[4])}
for a in addrs:
    for n_off, n_sz, kind in nro_segs:
        if n_off <= a < n_off + n_sz:
            es = elf_by_flag.get(kind)
            if not es:
                print(f"{a:#x}: no ELF seg [{kind}]"); break
            vaddr = es[3] + (a - n_off)
            print(f"NRO {a:#x} -> ELF {vaddr:#x}  [{kind}]")
            break
    else:
        end = d_off + d_sz + bss_sz
        print(f"NRO {a:#x}: in bss/guard ({'bss' if a < end else 'PAST image'})")
