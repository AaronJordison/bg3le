#!/usr/bin/env python3
"""make-sigs.py OLD_BG3 NEW_BG3 TARGETS...: signatures for bg3le's engine targets, checked against both builds.

TARGET is name=kind:0xOLD, kind one of
  code  a function (or an instruction) at OLD
  data  a global at OLD, found through an instruction that loads it (rip-relative)
  slot  a pointer-table slot at OLD, through the code that loads the table's base

For each it prints the Sig to paste into bg3le (pattern taken from OLD with every call target and rip-relative
displacement wildcarded, grown until unique in OLD's .text) and where it lands in NEW. A target is only reported
as found when the pattern is unique in NEW too; data and slots also need two independent loads to agree.
"""
import bisect, os, re, struct, subprocess, sys

MIN_FIXED, MAX_LEN = 12, 96

class Binary:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        b = self.data
        phoff, phentsize, phnum = struct.unpack_from("<Q", b, 0x20)[0], *struct.unpack_from("<HH", b, 0x36)
        self.loads = []
        for i in range(phnum):
            t, flags, off, va, _, filesz = struct.unpack_from("<IIQQQQ", b, phoff + i * phentsize)
            if t == 1:
                self.loads.append((off, va, filesz, flags))
        text = next(l for l in self.loads if l[3] & 1)
        self.text_off, self.text_va, self.text_size = text[0], text[1], text[2]
        self._refs = None

    def v2f(self, va):
        for off, v, sz, _ in self.loads:
            if v <= va < v + sz:
                return va - v + off
        return None

    def text_bytes(self):
        return self.data[self.text_off:self.text_off + self.text_size]

    def insns(self, va, length=MAX_LEN + 16):
        """(va, bytes, text) for the instructions from va, via objdump."""
        out = subprocess.run(["objdump", "-d", "--insn-width=15", "--start-address=%#x" % va,
                              "--stop-address=%#x" % (va + length), self.path], capture_output=True, text=True).stdout
        res = []
        for line in out.splitlines():
            m = re.match(r"\s*([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*\t?(.*)", line)
            if m:
                res.append((int(m.group(1), 16), bytes.fromhex(m.group(2).replace(" ", "")), m.group(3)))
        return res

    def rip_refs(self):
        """{target va: [instruction va, ...]} for every rip-relative operand, from a full disassembly (cached)."""
        if self._refs is not None:
            return self._refs
        cache_dir = os.path.join(os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"), "bg3le")
        os.makedirs(cache_dir, exist_ok=True)
        cache = os.path.join(cache_dir, "make-sigs-%d.refs" % len(self.data))
        refs = {}
        if os.path.exists(cache):
            for line in open(cache):
                t, i = line.split()
                refs.setdefault(int(t, 16), []).append(int(i, 16))
        else:
            p = subprocess.Popen(["objdump", "-d", "--no-show-raw-insn", self.path], stdout=subprocess.PIPE, text=True)
            with open(cache + ".part", "w") as f:
                for line in p.stdout:
                    m = None
                    if "(%rip)" in line:
                        m = re.match(r"\s*([0-9a-f]+):.*#\s*([0-9a-f]+)", line)
                    elif "\tcall" in line or "\tjmp" in line:
                        m = re.match(r"\s*([0-9a-f]+):\s*(?:call|jmp)\s+([0-9a-f]+) <", line)
                    if m:
                        i, t = int(m.group(1), 16), int(m.group(2), 16)
                        refs.setdefault(t, []).append(i)
                        f.write("%x %x\n" % (t, i))
            os.rename(cache + ".part", cache)
        self._refs = refs
        return refs

def wildcard(insn_va, raw, text):
    """Mask for one instruction: True where the byte is stable across builds."""
    mask = [True] * len(raw)
    def blank(pos):
        for k in range(pos, min(pos + 4, len(raw))):
            mask[k] = False
    if raw[:1] in (b"\xe8", b"\xe9") and len(raw) == 5:
        blank(1)
    elif len(raw) == 6 and raw[0] == 0x0f and 0x80 <= raw[1] <= 0x8f:
        blank(2)
    m = re.search(r"(-?0x[0-9a-f]+)\(%rip\)", text)
    if m:
        disp = int(m.group(1), 16) & 0xffffffff
        enc = struct.pack("<I", disp)
        pos = raw.find(enc, 1)
        if pos > 0:
            blank(pos)
    return mask

def count(bin_, pat, mask, limit=2):
    """Matches of pat (with mask) in bin_'s .text, up to limit; returns (count, first va)."""
    text = bin_.text_bytes()
    anchor = next(i for i, f in enumerate(mask) if f)
    run_end = anchor
    while run_end < len(mask) and mask[run_end]:
        run_end += 1
    key = pat[anchor:run_end]
    hits, first, start = 0, None, 0
    while True:
        i = text.find(key, start)
        if i < 0:
            break
        at = i - anchor
        if at >= 0 and at + len(pat) <= len(text) and all(not mask[k] or text[at + k] == pat[k] for k in range(len(pat))):
            hits += 1
            first = first if first is not None else bin_.text_va + at
            if hits >= limit:
                break
        start = i + 1
    return hits, first

def signature(bin_, va):
    """(pattern text, byte length) for the code at va: whole instructions, grown until unique in bin_."""
    pat, mask = b"", []
    for iva, raw, text in bin_.insns(va):
        pat += raw
        mask += wildcard(iva, raw, text)
        if sum(mask) >= MIN_FIXED and count(bin_, pat, mask)[0] == 1:
            return " ".join("%02x" % b if f else "??" for b, f in zip(pat, mask)), pat, mask
        if len(pat) >= MAX_LEN:
            break
    return None, pat, mask

def rip_disp_at(raw, text):
    if raw[:1] in (b"\xe8", b"\xe9") and len(raw) == 5:
        return 1  # direct call or jump: rel32 right after the opcode
    m = re.search(r"(-?0x[0-9a-f]+)\(%rip\)", text)
    if not m:
        return None
    return raw.find(struct.pack("<I", int(m.group(1), 16) & 0xffffffff), 1)

def through_loads(old, new, target, lo_span=0):
    """Resolve an OLD data address in NEW through instructions that load it (or, for slots, load a base within
    lo_span below it). Returns (new address, sig lines) or (None, reasons)."""
    refs = old.rip_refs()
    bases = [t for t in refs if target - lo_span <= t <= target]
    results, sigs = [], []
    for base in sorted(bases, reverse=True):
        for insn in refs[base][:40]:
            ins = old.insns(insn, 16)
            if not ins:
                continue
            _, raw, text = ins[0]
            disp_at = rip_disp_at(raw, text)
            if disp_at is None or disp_at < 0:
                continue
            sig, pat, mask = signature(old, insn)
            if sig is None:
                continue
            hits, at = count(new, pat, mask)
            if hits != 1:
                continue
            f = new.v2f(at)
            disp = struct.unpack_from("<i", new.data, f + disp_at)[0]
            new_base = at + len(raw) + disp
            results.append(new_base + (target - base))
            sigs.append((insn, sig, disp_at, len(raw), target - base, at))
            if len(results) >= 3:
                break
        if len(results) >= 3:
            break
    return results, sigs

def main():
    old, new = Binary(sys.argv[1]), Binary(sys.argv[2])
    for spec in sys.argv[3:]:
        name, rest = spec.split("=")
        kind, addr = rest.split(":")
        addr = int(addr, 16)
        if kind == "code":
            sig, pat, mask = signature(old, addr)
            if sig is None:
                # Inlined everywhere: anchor on the nearest unique code before it (Sig.start = the distance).
                anchored = None
                for s_va, _, _ in reversed([i for i in old.insns(addr - 512, 512) if i[0] < addr]):
                    s_sig, s_pat, s_mask = signature(old, s_va)
                    if s_sig and count(new, s_pat, s_mask)[0] == 1:
                        anchored = (s_va, s_sig, count(new, s_pat, s_mask)[1])
                        break
                if anchored:
                    s_va, s_sig, s_new = anchored
                    at = s_new + (addr - s_va)
                    same = old.data[old.v2f(addr):old.v2f(addr) + 5] == new.data[new.v2f(at):new.v2f(at) + 5]
                    print("%-28s old %#x -> new %#x (%+d)  (anchored %d bytes back%s)\n    Sig{\"%s\", %#x, \"%s\", %d}" % (
                        name, addr, at, at - addr, addr - s_va, "" if same else "; TARGET BYTES DIFFER",
                        name, s_va, s_sig, addr - s_va))
                    continue
                # Identical copies of the function exist: go through a call site instead (resolve_call).
                results, sigs = through_loads(old, new, addr)
                agree = len(results) >= 2 and len(set(results)) == 1
                verdict = ("%#x (%+d)" % (results[0], results[0] - addr)) if agree else (
                    "DISAGREE %s" % [hex(r) for r in results] if results else "no unique body and no usable call site")
                print("%-28s old %#x -> new %s  (through a call site)" % (name, addr, verdict))
                for insn, s, disp_at, ilen, extra, at in sigs[:1]:
                    print("    resolve_call(Sig{\"%s\", %#x, \"%s\"})" % (name, insn, s))
                continue
            hits, at = count(new, pat, mask)
            where = ("%#x (%+d)" % (at, at - addr)) if hits == 1 else ("%d matches" % hits)
            print("%-28s old %#x -> new %s\n    Sig{\"%s\", %#x, \"%s\"}" % (name, addr, where, name, addr, sig))
        else:
            results, sigs = through_loads(old, new, addr, lo_span=0x2000 if kind == "slot" else 0)
            agree = len(results) >= 2 and len(set(results)) == 1
            verdict = ("%#x (%+d)" % (results[0], results[0] - addr)) if agree else ("DISAGREE %s" % [hex(r) for r in results] if results else "no usable load")
            print("%-28s old %#x -> new %s" % (name, addr, verdict))
            for insn, sig, disp_at, ilen, extra, at in sigs[:1]:
                print("    load at old %#x (new %#x), disp at +%d, insn %d bytes, target = rip target %+#x" % (insn, at, disp_at, ilen, extra))
                print("    Sig{\"%s\", %#x, \"%s\"}" % (name, insn, sig))

if __name__ == "__main__":
    main()
