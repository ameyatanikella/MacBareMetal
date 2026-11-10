#!/usr/bin/env python3
"""Turn the corpus into evidence about the instruction encoding.

The method is bit correlation. For a family of kernels that differ only in a
known quantity (an immediate, a truth table), find the code bits that equal
each bit of that quantity in every sample. A mapping learned from "walking
one" samples is then tested by predicting the full code of samples that were
held out. A prediction that matches byte for byte is real evidence; anything
else is printed as unexplained.

Nothing here interprets instructions beyond what the data forces.

    analyze.py            print the report (also written to data/analysis.txt)
"""
import io
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import agxbin  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CORPUS = json.loads((ROOT / "data" / "corpus.json").read_text())
END = bytes.fromhex("0e000000")
WAIT = bytes.fromhex("410080000000")

out = io.StringIO()


def p(*a):
    print(*a)
    print(*a, file=out)


def code(name, key="main"):
    return bytes.fromhex(CORPUS[name][key])


def fam(name):
    """[(param, code)] for one family, verified samples only."""
    return [
        (r["param"], bytes.fromhex(r["main"]))
        for r in CORPUS.values()
        if r.get("family") == name and r["ok"]
    ]


def bit(b, i):
    return (b[i // 8] >> (i % 8)) & 1


def pos(i):
    return f"byte {i // 8} bit {i % 8}"


def fit(samples, nbits):
    """Map each parameter bit to the code bits that track it in every sample.

    Returns (mapping, unexplained): mapping[j] = [code bit index, ...] for
    each parameter bit j that varies; unexplained = code bits that vary but
    track no parameter bit.
    """
    n = len(samples[0][1]) * 8
    varying = [i for i in range(n) if len({bit(c, i) for _, c in samples}) > 1]
    mapping, used = {}, set()
    for j in range(nbits):
        if len({(prm >> j) & 1 for prm, _ in samples}) < 2:
            continue
        hits = [i for i in varying if all(bit(c, i) == (prm >> j) & 1 for prm, c in samples)]
        mapping[j] = hits
        used.update(hits)
    return mapping, [i for i in varying if i not in used]


def predict(template_param, template, mapping, param):
    b = bytearray(template)
    for j, hits in mapping.items():
        for i in hits:
            if (param >> j) & 1:
                b[i // 8] |= 1 << (i % 8)
            else:
                b[i // 8] &= ~(1 << (i % 8))
    return bytes(b)


def show_mapping(mapping, nbits):
    for j in range(nbits):
        if j not in mapping:
            p(f"    value bit {j:2d}: did not vary in the training samples")
        elif not mapping[j]:
            p(f"    value bit {j:2d}: NO code bit tracks it")
        else:
            p(f"    value bit {j:2d} -> " + ", ".join(pos(i) for i in mapping[j]))


def validate(title, train, held, nbits):
    mapping, unexplained = fit(train, nbits)
    p(f"\n{title}")
    p(f"  trained on {len(train)} samples, {len(train[0][1])} bytes each")
    show_mapping(mapping, nbits)
    if unexplained:
        p("    code bits that vary without tracking any value bit: " + ", ".join(pos(i) for i in unexplained))
    good = 0
    for prm, actual in held:
        if len(actual) != len(train[0][1]):
            p(f"    held-out {prm:#x}: different code length ({len(actual)}), not comparable")
            continue
        guess = predict(train[0][0], train[0][1], mapping, prm)
        if guess == actual:
            good += 1
        else:
            diff = [i for i in range(len(actual)) if guess[i] != actual[i]]
            p(f"    held-out {prm:#x}: prediction wrong at bytes {diff}")
    p(f"  held-out predictions correct: {good} of {len(held)}")
    return mapping


def one_bit(x):
    return x and not x & (x - 1)


# --- 1. program framing ---------------------------------------------------------

p("1. FRAMING")
ok = [n for n, r in CORPUS.items() if r["ok"]]
ends = sum(code(n).endswith(END) for n in ok)
p(f"  {ends} of {len(ok)} kernels end with {agxbin.hexs(END)}")
p(f"  the empty kernel is exactly: {agxbin.hexs(code('empty'))}")
firsts = {}
for n in ok:
    firsts.setdefault(code(n)[:4].hex(), []).append(n)
for k, v in sorted(firsts.items(), key=lambda kv: -len(kv[1])):
    p(f"  first 4 bytes {agxbin.hexs(bytes.fromhex(k))}: {len(v)} kernels (e.g. {', '.join(v[:4])})")

reads = [n for n in ok if "a[id]" in CORPUS[n]["body"] or "b[id]" in CORPUS[n]["body"]]
with_wait = [n for n in ok if WAIT in code(n)]
p(f"  kernels whose source reads a buffer: {len(reads)}; containing {agxbin.hexs(WAIT)}: {len(with_wait)}")
p(f"    reads but lacks it: {sorted(set(reads) - set(with_wait)) or 'none'}")
p(f"    has it but does not read: {sorted(set(with_wait) - set(reads)) or 'none'}")

cps = {}
for n in ok:
    cps.setdefault(code(n, "constant_program").rstrip(b"\x06\x00").hex(), []).append(n)
p("  constant program (padding of 06 00 removed):")
for k, v in cps.items():
    reading = sum(n in reads for n in v)
    p(f"    {agxbin.hexs(bytes.fromhex(k)):<40} {len(v):3d} kernels, {reading} of them read a buffer")

# --- 2. buffer slots -------------------------------------------------------------

p("\n2. BUFFER SLOTS")
a, r = code("add"), code("rsub")
p("  add  (a[id] + b[id]) vs rsub (b[id] - a[id]) share their two load instructions;")
p("  the loads differ only at: " + ", ".join(
    f"byte {i} ({a[i]:02x} vs {r[i]:02x})" for i in range(26) if a[i] != r[i]))
p(f"  copy_a and copy_b compile to identical code: {code('copy_a') == code('copy_b')}")
p("  so the byte after 0f/1f/2f/3f selects among bindings in steps of 4, numbered")
p("  by order of use, and which Metal buffer that is must be recorded outside the code.")

# --- 3. immediates ---------------------------------------------------------------

p("\n3. IMMEDIATES")
consts = fam("store_const")
small = [(k, c) for k, c in consts if len(c) == 18]
wide = [(k, c) for k, c in consts if len(c) == 24]
p(f"  o[id] = K: {len(small)} constants give 18 bytes, {len(wide)} give 24 bytes")
p(f"    largest 18-byte K: {max(k for k, _ in small):#x}; smallest 24-byte K: {min(k for k, _ in wide):#x}")

validate("  3a. short constant move (18-byte kernels)",
         [s for s in small if one_bit(s[0])], [s for s in small if not one_bit(s[0])], 7)

train = [s for s in wide if one_bit(s[0]) or (s[0] < 0x100 and one_bit(s[0] & 0x7F))]
held = [s for s in wide if s not in train]
validate("  3b. 32-bit constant move (24-byte kernels)", train, held, 32)

imm = fam("add_imm8")
validate("  3c. a[id] + K with K < 256, immediate inside the add instruction",
         [s for s in imm if one_bit(s[0])], [s for s in imm if not one_bit(s[0])], 8)

hoist = fam("add_hoisted")
shapes = {}
for k, c in hoist:
    shapes.setdefault(c.hex(), []).append(k)
p(f"\n  3d. a[id] + K with K >= 256: {len(hoist)} constants give only {len(shapes)} distinct code sequences")
for h, ks in shapes.items():
    p(f"    {len(ks):2d} constants, e.g. {', '.join(f'{k:#x}' for k in ks[:5])}")
    p(f"       {agxbin.hexs(bytes.fromhex(h)[18:-12])}")
p("    The constant is therefore not in the code. Where its bytes appear in the archive:")
for name in ("f_addh_12345678", "f_addh_00abcdef"):
    k = CORPUS[name]["param"]
    needle = k.to_bytes(4, "little")
    arc = ROOT / "out" / "corpus" / f"{name}.bin"
    places = []
    for st in agxbin.stages(arc):
        for s in st.obj.sections:
            at = s.data.find(needle)
            if at >= 0:
                places.append(f"{s.segname} +{at:#x}")
    p(f"      {k:#010x}: {', '.join(places) or 'not found as 4 little-endian bytes in the stage object'}")

# --- 4. bitwise operations ---------------------------------------------------------

p("\n4. BITWISE OPERATIONS")
bitop = fam("bitop")
p("  truth table bit (2*a + b) = f(a, b); code bytes 26..35 of each kernel:")
names = {r["param"]: n for n, r in CORPUS.items() if r.get("family") == "bitop"}
for tt, c in sorted(bitop):
    p(f"    {tt:04b}  {names[tt][6:]:<7} {agxbin.hexs(c[26:36])}")
mapping, unexplained = fit(bitop, 4)
p("  raw fit over all ten functions:")
show_mapping(mapping, 4)
p("    unexplained varying bits: " + (", ".join(pos(i) for i in unexplained) or "none"))

# Hypothesis: for some functions the compiler exchanges the two operands,
# which exchanges truth-table bits 1 and 2. Try each unexplained bit as the
# marker of that exchange and keep the one that makes all four bits fit.
for marker in unexplained:
    base = bit(dict(bitop)[0b1000], marker)
    swapped = []
    for tt, c in bitop:
        if bit(c, marker) != base:
            tt = (tt & 0b1001) | ((tt & 0b0010) << 1) | ((tt & 0b0100) >> 1)
        swapped.append((tt, c))
    m2, u2 = fit(swapped, 4)
    if all(m2.get(j) for j in range(4)):
        flipped = sorted(names[tt][6:] for tt, c in bitop if bit(c, marker) != base)
        p(f"  refit assuming the operands are exchanged whenever {pos(marker)} flips")
        p(f"  (it flips for: {', '.join(flipped)}):")
        show_mapping(m2, 4)
        p("    still unexplained: " + (", ".join(pos(i) for i in u2) or "none"))
        p("    All four truth-table bits then fit all ten functions. The bits still")
        p("    unexplained flip together with the marker, as two operand fields")
        p("    trading places would.")
        break
else:
    p("  no single marker bit makes all four truth-table bits fit")

# --- 5. arithmetic ----------------------------------------------------------------

p("\n5. ARITHMETIC (bytes between the loads and the store)")
for n in ("add", "sub", "mul", "add3", "fadd", "fsub", "fmul", "fma"):
    c = code(n)
    p(f"    {n:<5} {agxbin.hexs(c[26:-12])}")
p("  add vs sub differ at: " + ", ".join(
    f"byte {i} ({x:02x} vs {y:02x})" for i, (x, y) in enumerate(zip(code('add'), code('sub'))) if x != y))

(ROOT / "data" / "analysis.txt").write_text(out.getvalue())
