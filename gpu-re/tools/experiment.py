#!/usr/bin/env python3
"""Differential experiment: compile kernels that differ in one detail, check
each one computes what we think on the real GPU, and record its machine code.

    experiment.py            run every variant, write data/corpus.json
    experiment.py add sub    run only the named variants

Each variant is `o[id] = <expr>` over a[id] = id + 1 and b[id] = 2*id + 3
(or a custom body). A variant only enters the corpus if the GPU's output
matches the value computed here, so every code sample is tied to known
semantics.
"""
import json
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import agxbin  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "out" / "corpus"
MTLRUN = ROOT / "out" / "mtlrun"
N = 16
M = 0xFFFFFFFF

TEMPLATE = """#include <metal_stdlib>
using namespace metal;

kernel void k(device const {t} *a [[buffer(0)]],
              device const {t} *b [[buffer(1)]],
              device {t} *o [[buffer(2)]],
              uint id [[thread_position_in_grid]])
{{
{body}
}}
"""


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def expr(e):
    return f"    o[id] = {e};"


# name, element type, body, expected(a, b, id) -> 32-bit word or None to skip the check
VARIANTS = [
    # the baseline and its binary-operator neighbours
    ("add", "uint", expr("a[id] + b[id]"), lambda a, b, i: (a + b) & M),
    ("sub", "uint", expr("a[id] - b[id]"), lambda a, b, i: (a - b) & M),
    ("rsub", "uint", expr("b[id] - a[id]"), lambda a, b, i: (b - a) & M),
    ("mul", "uint", expr("a[id] * b[id]"), lambda a, b, i: (a * b) & M),
    ("and", "uint", expr("a[id] & b[id]"), lambda a, b, i: a & b),
    ("or", "uint", expr("a[id] | b[id]"), lambda a, b, i: a | b),
    ("xor", "uint", expr("a[id] ^ b[id]"), lambda a, b, i: a ^ b),
    ("shl", "uint", expr("a[id] << (b[id] & 31u)"), lambda a, b, i: (a << (b & 31)) & M),
    ("shr", "uint", expr("a[id] >> (b[id] & 31u)"), lambda a, b, i: a >> (b & 31)),
    # immediates of growing width
    ("addi_1", "uint", expr("a[id] + 1u"), lambda a, b, i: (a + 1) & M),
    ("addi_2", "uint", expr("a[id] + 2u"), lambda a, b, i: (a + 2) & M),
    ("addi_3", "uint", expr("a[id] + 3u"), lambda a, b, i: (a + 3) & M),
    ("addi_255", "uint", expr("a[id] + 255u"), lambda a, b, i: (a + 255) & M),
    ("addi_256", "uint", expr("a[id] + 256u"), lambda a, b, i: (a + 256) & M),
    ("addi_65535", "uint", expr("a[id] + 65535u"), lambda a, b, i: (a + 65535) & M),
    ("addi_65536", "uint", expr("a[id] + 65536u"), lambda a, b, i: (a + 65536) & M),
    ("addi_12345678", "uint", expr("a[id] + 0x12345678u"), lambda a, b, i: (a + 0x12345678) & M),
    ("addi_aabbccdd", "uint", expr("a[id] + 0xaabbccddu"), lambda a, b, i: (a + 0xAABBCCDD) & M),
    # loads and stores in isolation
    ("copy_a", "uint", expr("a[id]"), lambda a, b, i: a),
    ("copy_b", "uint", expr("b[id]"), lambda a, b, i: b),
    ("store_0", "uint", expr("0u"), lambda a, b, i: 0),
    ("store_1", "uint", expr("1u"), lambda a, b, i: 1),
    ("store_id", "uint", expr("id"), lambda a, b, i: i),
    ("store_12345678", "uint", expr("0x12345678u"), lambda a, b, i: 0x12345678),
    ("empty", "uint", "", lambda a, b, i: 0xDEADBEEF),
    # floating point
    ("fadd", "float", expr("a[id] + b[id]"), lambda a, b, i: f32(float(a) + float(b))),
    ("fsub", "float", expr("a[id] - b[id]"), lambda a, b, i: f32(float(a) - float(b))),
    ("fmul", "float", expr("a[id] * b[id]"), lambda a, b, i: f32(float(a) * float(b))),
    ("fma", "float", expr("fma(a[id], b[id], a[id])"), lambda a, b, i: f32(float(a) * float(b) + float(a))),
    # three operands and control flow
    ("add3", "uint", expr("a[id] + b[id] + a[id]"), lambda a, b, i: (a + b + a) & M),
    ("select", "uint", expr("a[id] > 4u ? a[id] : b[id]"), lambda a, b, i: a if a > 4 else b),
    (
        "branch",
        "uint",
        "    if (a[id] > 4u) { o[id] = a[id] * 3u; } else { o[id] = b[id] + 7u; }",
        lambda a, b, i: (a * 3) & M if a > 4 else (b + 7) & M,
    ),
    (
        "loop",
        "uint",
        "    uint s = 0u;\n    for (uint j = 0u; j < (a[id] & 7u); j++) { s += b[id] ^ j; }\n    o[id] = s;",
        lambda a, b, i: sum(b ^ j for j in range(a & 7)) & M,
    ),
]


# --- parameterised families -------------------------------------------------
# Each family varies one known quantity. tools/analyze.py looks for code bits
# that track each bit of that quantity across the family.

FAMILY = {}  # variant name -> (family, parameter)


def family(fam, name, param, typ, body, expect):
    VARIANTS.append((name, typ, body, expect))
    FAMILY[name] = (fam, param)


def _tt(fn):
    """Truth table of a two-input boolean function: bit (2*a + b) = fn(a, b)."""
    return sum((fn(x, y) & 1) << (2 * x + y) for x in (0, 1) for y in (0, 1))


SMALL = [1 << k for k in range(8)] + [3, 5, 9, 85, 170, 255, 77, 200]
WIDE = [1 << k for k in range(32)] + [0x80 | (1 << k) for k in range(7)]
WIDE += [0x12345678, 0xAABBCCDD, 0xFFFFFFFF, 0x0F0F0F0F, 0x80000001, 0x00C0FFEE, 0x7E57AB1E, 99, 127]

for _k in SMALL:
    family("add_imm8", f"f_addi_{_k}", _k, "uint", expr(f"a[id] + {_k}u"),
           lambda a, b, i, k=_k: (a + k) & M)
for _k in WIDE:
    family("store_const", f"f_const_{_k:08x}", _k, "uint", expr(f"{_k}u"),
           lambda a, b, i, k=_k: k)
for _k in [1 << k for k in range(8, 32)] + [0x12345678, 0x00ABCDEF, 0x7FFFFFFF]:
    family("add_hoisted", f"f_addh_{_k:08x}", _k, "uint", expr(f"a[id] + {_k}u"),
           lambda a, b, i, k=_k: (a + k) & M)

for _name, _src, _fn in [
    ("and", "a[id] & b[id]", lambda x, y: x & y),
    ("or", "a[id] | b[id]", lambda x, y: x | y),
    ("xor", "a[id] ^ b[id]", lambda x, y: x ^ y),
    ("andn", "a[id] & ~b[id]", lambda x, y: x & ~y),
    ("nand_b", "~a[id] & b[id]", lambda x, y: ~x & y),
    ("nand", "~(a[id] & b[id])", lambda x, y: ~(x & y)),
    ("nor", "~(a[id] | b[id])", lambda x, y: ~(x | y)),
    ("xnor", "~(a[id] ^ b[id])", lambda x, y: ~(x ^ y)),
    ("orn", "a[id] | ~b[id]", lambda x, y: x | ~y),
    ("norn", "~a[id] | b[id]", lambda x, y: ~x | y),
]:
    family("bitop", f"f_bit_{_name}", _tt(_fn), "uint", expr(_src),
           lambda a, b, i, fn=_fn: fn(a, b) & M)


def run_variant(name, typ, body, expect):
    OUT.mkdir(parents=True, exist_ok=True)
    src = OUT / f"{name}.metal"
    arc = OUT / f"{name}.bin"
    src.write_text(TEMPLATE.format(t=typ, body=body))

    cmd = [str(MTLRUN), str(src), "--archive", str(arc), "--n", str(N), "--quiet"]
    if typ == "float":
        cmd.append("--float")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode:
        return {"ok": False, "error": res.stderr.strip()}

    got = [int(w, 16) for w in res.stdout.split()[1:]]
    want = [expect(i + 1, 2 * i + 3, i) for i in range(N)]
    stage = agxbin.stages(arc)[0]

    return {
        "ok": got == want,
        "type": typ,
        "body": body.strip(),
        "got": [f"{w:08x}" for w in got[:4]],
        "want": [f"{w:08x}" for w in want[:4]],
        "main": stage.funcs.get("_agc.main", b"").hex(),
        "constant_program": stage.funcs.get("_agc.main.constant_program", b"").hex(),
        "symbols": list(stage.funcs),
        "family": FAMILY.get(name, (None, None))[0],
        "param": FAMILY.get(name, (None, None))[1],
    }


def main():
    if not MTLRUN.exists():
        sys.exit("build out/mtlrun first (make -C gpu-re)")

    wanted = set(sys.argv[1:])
    corpus = {}
    for name, typ, body, expect in VARIANTS:
        if wanted and name not in wanted:
            continue
        r = run_variant(name, typ, body, expect)
        corpus[name] = r
        if not r["ok"]:
            print(f"{name:<16} FAILED {r.get('error', '')} got {r.get('got')} want {r.get('want')}")
            continue
        code = bytes.fromhex(r["main"])
        if not name.startswith("f_") or wanted:
            print(f"{name:<16} {len(code):3d}  {agxbin.hexs(code)}")

    if not wanted:
        data = ROOT / "data" / "corpus.json"
        data.write_text(json.dumps(corpus, indent=1) + "\n")
        bad = [n for n, r in corpus.items() if not r["ok"]]
        print(f"\n{len(corpus) - len(bad)} of {len(corpus)} variants verified on the GPU; wrote {data}")
        if bad:
            print("not verified:", ", ".join(bad))


if __name__ == "__main__":
    main()
