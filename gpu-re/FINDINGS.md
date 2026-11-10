# GPU reverse engineering: first attempt

Target: Apple M5 Pro GPU (driver class `AGXAcceleratorG17X`, device tree node `sgx`, `gpu,t6050`), macOS 26.6.2. All results are from 9 November 2026 on one machine.

**What this is.** A black-box, differential study of the GPU's machine code, run entirely under macOS through public Metal API. It compiles tiny compute kernels that differ in one known detail, checks on the real GPU that each computes what we expect, and compares the compiled bytes. It also logs which kernel-driver calls a run makes.

**What it is not.** It is not a disassembler, and it does not run anything on the GPU without macOS. It decodes a handful of fields in a handful of instructions.

**Clean-room rule.** Only our own compiled output and our own process's system calls are examined. No Apple binary is disassembled.

## Reproduce

```bash
make -C gpu-re
```

```bash
make -C gpu-re corpus
```

```bash
make -C gpu-re analyze
```

```bash
make -C gpu-re trace
```

`corpus` takes about two minutes. Results land in `gpu-re/data/corpus.json` and `gpu-re/data/analysis.txt`.

## Established facts

Each item names the evidence. "Predicted" means a mapping learned from one set of samples reproduced the full code of samples it had not seen.

### Container

- A serialized `MTLBinaryArchive` is a fat file with two slices: MetalLib bitcode (cputype `0x01000017`) and an `applegpu` Mach-O (cputype `0x01000013`, subtype `0x163`).
- The `applegpu` slice has a `__TEXT,__compute` section that is itself a Mach-O object. Its `__text` holds the machine code, with symbols `_agc.main` and `_agc.main.constant_program`.
- The same object carries driver metadata sections (`__GPU_METADATA`, `__GPU_LD_MD`, `__GPU_ARCH_LD_MD`, `__GPU_STATS_MD`), not decoded here.

### Program framing (134 kernels, all verified on the GPU)

- Every kernel ends with `0e 00 00 00`. The empty kernel is exactly those four bytes.
- Every non-empty kernel starts with `1c a0 10 06` (129 kernels) or `0c a0 10 06` (4 kernels).
- The byte sequence `41 00 80 00 00 00` appears in exactly the 81 kernels whose source reads a buffer.
- The constant program is `23 00 07 02 42 20 a0 02` followed by the end marker in those same 81 kernels, and just the end marker in the other 53. It is padded with `06 00`.

### Buffer bindings

- `a[id] + b[id]` and `b[id] - a[id]` differ in their two load instructions only at the byte after the first byte of each (`00` and `04` exchanged).
- Loads use `00` and `04`; the store in a two-input kernel uses `08`.
- A kernel that copies buffer `a` and one that copies buffer `b` compile to identical code. So the code holds a slot number, assigned in order of use in steps of 4, and the mapping to Metal buffer indices is kept outside the code.

### Immediates

- **`o[id] = K`, K from 1 to 0x7f:** a 2-byte instruction `04 KK`. Predicted 2 of 2 held-out constants. K = 0 uses a different 4-byte form, `0b 00 00 00`.
- **`o[id] = K`, K from 0x80:** an 8-byte instruction starting `0c`. All 32 value bits are located:

  | Value bits | Location in the instruction |
  |---|---|
  | 0 to 6 | byte 1, bits 0 to 6 (bit 7 of that byte is always set) |
  | 7 to 10 | byte 4, bits 1 to 4 |
  | 11 to 12 | byte 5, bits 2 to 3 |
  | 13 to 20 | byte 6, bits 0 to 7 |
  | 21 to 24 | byte 7, bits 0 to 3 |
  | 25 to 31 | byte 3, bits 1 to 7 |

  Learned from 32 single-bit constants; predicted 7 of 7 held-out constants byte for byte.
- **`a[id] + K`, K below 256:** the constant is inside the 12-byte add instruction, split into four pieces (bits 0 to 1 in byte 1, bits 2 to 4 in byte 3 bits 5 to 7, bit 5 in byte 5 bit 7, bits 6 to 7 in byte 8 bits 0 to 1). Predicted 8 of 8.
- **`a[id] + K`, K from 256:** 27 constants produce only 3 distinct code sequences, so the constant is not in the code. It is stored as four little-endian bytes in `__GPU_METADATA` at offset `0x114`.
  - For K from 2^12 to 2^19 the stored value is K / 16, and two bytes of the add differ (`82 03` becomes `02 02`).
  - `a[id] + 0x80000000` is compiled to the bitwise instruction instead.

### Bitwise operations

All ten two-input boolean functions compile to one 10-byte instruction that differs only in a 4-bit truth table. With truth-table bit (2a + b) = f(a, b):

| Truth-table bit | Location in the instruction |
|---|---|
| 0 | byte 4, bit 0 |
| 1 | byte 4, bit 1 |
| 2 | byte 5, bit 3 |
| 3 | byte 2, bit 0 |

This fits all ten functions once the two operands are taken as exchanged for `a & ~b`, `a | ~b` and `~(a ^ b)`. Those three flip two further bits together (byte 5 bit 6 and byte 9 bit 0).

### Arithmetic

Integer add, subtract and three-operand add are 12 bytes; multiply is 14. Float add and subtract are 12 bytes, multiply 14, fused multiply-add 16. Add and subtract differ in five bytes. Fields are not yet separated.

### Relation to the published M1 encoding

The public M1 (G13) reference lists `stop` as `0x0088`, constant moves with low seven bits `0x62`, and device load and store with low seven bits `0x05` and `0x45`. None of these appear where M5 Pro code has its end marker, constant moves, loads or stores. The structure is similar (variable-length instructions, operand bits scattered across the instruction, truth-table bitwise ops, a separate constant program), but the encodings differ.

This comparison was made by reading the reference's table. Running its disassembler over the corpus was blocked in this session (it is downloaded code), so that check is still to do.

### Driver interface (from the call log)

- Metal opens one user client on the GPU: `AGXAcceleratorG17X`, type `0x100005`.
- A vector-add run makes 29 calls to selector 9 (104 bytes in, 88 out) and one or two calls each to selectors 0, 2, 5, 6, 7, 8, 14, 15, 16, 17, 28, 256, 258 and 261.
- It also uses two fast traps: index 0 once, with four arguments (`1`, `0x40`, and two pointers), and index 1 ten times with one small integer.
- No `IOConnectMapMemory64` call is made.
- The call profile of the add kernel and the empty kernel is the same apart from one IOSurface call.

### Hardware description (from the I/O registry)

- GPU generation 17, variant "S", 20 cores, 2 "mGPUs" (matching the two coprocessor nodes `gfx-asc` and `gfx1-asc`), 14 performance states.
- `sgx` lists gate ids `0x268` and `0x267`; `gfx-asc` lists `0x266`; `gfx1-asc` lists `0x291`.
- The GPU has no `dart-sgx` node. The coprocessors' IOMMU parent is `mapper-gfx-asc` (`iommu-mapper,gfx`).

## Hypotheses (not established)

- **Instruction split of the add kernel.** A reading consistent with the corpus:

  ```
  1c a0 10 06                           thread position into a register
  0f 00 03 02 18 22 10 40               load from slot 0
  2f 04 03 02 18 02 10 c0 41 00         load from slot 4
  80 00 00 00                           wait for loads
  2f 00 04 3a 21 00 a3 02 28 80 21 00   add
  0f 08 03 02 01 06 10 40               store to slot 8
  0e 00 00 00                           end
  ```

- **Length bit.** The top bit of the eighth byte of a load or store adds two bytes. Stores are 8 bytes ending `40` or 10 bytes ending `c0 00 20`.
- **Trap 0 is the submit call.** It happens once per run, after resource setup.
- **Selector 9 creates resources.** It is by far the most frequent call and takes a fixed 104-byte structure.

## Open questions

- Where register numbers sit in arithmetic instructions.
- What the constant program computes for kernels that read buffers.
- What `0x1c` and `0x0c` in the first instruction select.
- The layout of `__GPU_METADATA`, which holds bindings and hoisted constants.
- How work reaches the firmware: nothing in the call log yet shows command data.

## Next steps

1. Vary register pressure (more live values) to locate register fields.
2. Extend the harness to more buffers, textures and threadgroup memory.
3. Decode `__GPU_METADATA` by the same bit-correlation method.
4. Capture the memory behind the trap 0 pointers before and after a run.
5. Tracing below the user client (firmware messages, registers) needs the m1n1 hypervisor and a second computer.
