# m5-baremetal

Two things for an M5 Pro Mac (Mac17,9, SoC T6050):

- **A bare-metal runtime** booted by m1n1, with no macOS or Linux kernel underneath. Milestone 1 is diagnostics only: prove that our own code runs, prints to the screen, survives exceptions, manages memory and returns to macOS.
- **A first GPU reverse-engineering attempt** that runs under macOS today: differential analysis of the GPU's machine code and a log of the driver's kernel calls. See [gpu-re/FINDINGS.md](gpu-re/FINDINGS.md).

**Status (9 November 2026): the runtime builds and passes host tests but has never been booted; nothing about its behaviour on the hardware is verified. The GPU experiments have run on this Mac and their results are measured.**

## What is verified, and what is not

| Claim | Status | Evidence |
|---|---|---|
| Upstream m1n1 has T6050 code | Verified in source | Commit `b2d3f5eb` "Initial support for T6050/T6051" (29 June 2026): early UART base, CPU start offset, core IDs. Pinned tree is `6ee64bdd`. |
| m1n1 reached its console on an M5 Pro | Reported by its author, not by us | Same commit message: boots on core 0 (an M-core); tested on macOS 26.2b2 and 27.0b2 firmware. This Mac runs 26.6.2 (`mBoot-18000.161.10`), which is untested. |
| Secondary cores | Broken upstream | Same commit: cores power on but never run code. The runtime stays on core 0. |
| Memory controller, CPU frequency | Unsupported upstream | `mcc.c` has no `mcc,t6050` entry (this Mac's node is `mcc,t6050`); `cpufreq.c` has no T6050 case. Both failures are non-fatal in m1n1. |
| Asahi installer on M5 Pro | Not supported | Asahi's M5 feature page lists Installer "no" and everything else "TBA". |
| m1n1 hypervisor on M5 | Base M5 only | Docs describe T8142; nothing names T6050. Not needed for this milestone. |
| Toolchain and build | Verified here | Stock m1n1 and `out/m5rt.bin` both build on this Mac. |
| Allocator | Verified on host | `make test` (ASan + UBSan). |
| Screen layout and paging | Verified in simulation | `make sim` renders the real `rt_ui.c` at 3024x1964 and 1920x1080. |
| Device names (`sgx` = `gpu,t6050`, `gfx-asc`, `gfx1-asc`, `pmgr1,t6050`, `aic,3`, `wdt,t8101`, `ans`, 242 devices, 42 DARTs, 18 cores) | Verified from macOS | `ioreg -p IODeviceTree` on this Mac. |
| GPU machine code: container, framing, immediates, truth tables | Measured here | 134 kernels, each checked on the GPU; held-out predictions 7 of 7, 8 of 8, 2 of 2. `gpu-re/data/analysis.txt`. |
| Installing a custom boot object on M5 Pro | **Unverified** | Procedure is Asahi's, written for M1 to M3. |
| Watchdog registers, NVMe driver, GPU register reads, GPU power gates on T6050 | **Unverified** | Off by default; see the risk ladder. |

## Design: the runtime is linked into m1n1

m1n1's only handoff to an appended payload is the Linux boot protocol. It needs a flattened device tree whose root matches `apple,j714s`, runs about thirty device-tree fix-ups that have never run on this board, then switches off the MMU, exception vectors and framebuffer console before jumping. A failure there would leave a blank screen and no way to see why.

So the runtime is a small set of C files compiled into the m1n1 image and called from `m1n1_main()` after m1n1's own bring-up. There is still no OS underneath. We reuse m1n1's start-up code, MMU, exception vectors, framebuffer console, heap and device-tree parser, and later its `dart.c`, `pmgr.c`, `rtkit.c` and `asc.c`. The cost is three small hooks in m1n1 (`patches/`, 27 lines) and the GPL-2.0-or-later licence.

## Layout

| Path | What it is |
|---|---|
| `rt/rt_main.c` | Stage sequence, summary, reboot, fatal hold |
| `rt/rt_diag.c` | Stages: ident, cpu, exceptions, memory, devtree, gpu, nvme |
| `rt/rt_ui.c` | Paged console text plus coloured status tiles |
| `rt/rt_arena.c` | 16 KiB page allocator for device-visible buffers |
| `rt/rt_probe.c` | Exception-guarded memory and register reads |
| `rt/rt_wdt.c` | Watchdog hang guard (opt-in) |
| `rt/rt.mk` | Build fragment included by m1n1's Makefile |
| `patches/` | The three m1n1 hooks |
| `tests/host/` | Allocator tests and the screen simulator |
| `scripts/` | Toolchain setup, m1n1 fetch, staging for recovery |
| `docs/TESTING.md` | Hardware test procedure, failure table, risk ladder, undo |
| `gpu-re/` | GPU experiments under macOS: Metal harness, call logger, analysis, findings |
| `vendor/m1n1` | m1n1 at the pinned commit (fetched, git-ignored) |

## Build

```bash
make setup
```

```bash
make fetch
```

```bash
make test
```

```bash
make sim
```

```bash
make image
```

`make image` writes `out/m5rt.bin` and prints its build id, which also appears on every screen page.

## Testing on the Mac

The full procedure, failure table and undo steps are in [docs/TESTING.md](docs/TESTING.md). **None of it has been run on an M5 Pro.** In outline:

1. Add a second APFS volume and install macOS on it. Your main volume is not modified.
2. From recovery (1TR), lower security for that volume only and install `out/m5rt.bin` as its boot object with the staged `install.sh`.
3. Boot it, film the screen, and select your main volume again afterwards.

Every new build repeats the recovery step until rung 3 below passes.

## Reading the result without a serial console

- **Tiles on the right** show each stage: grey pending, yellow running, green pass, orange warning, red fail, blue skipped.
- **A hang** leaves one tile yellow. The last console line names the step. Hold the power button for ten seconds.
- **A crash** prints registers, turns the tile red and holds the screen for 90 seconds before rebooting.
- **Blank screen or Apple logo only** means m1n1 itself did not get as far as its console on this firmware. That is an upstream m1n1 problem, not a runtime one.

## Milestone 1 is reached when

1. The screen shows `m5rt 0.1` with the build id you just built.
2. `ident` reports chip-id `0x6050`.
3. `cpu`, `exceptions`, `memory` and `devtree` are green or orange, and each warning is understood.
4. The summary page prints `MILESTONE 1: REACHED`.
5. The Mac reboots by itself and macOS starts normally.
6. A second run gives the same results.

## Risk ladder

| Rung | Build | What it adds |
|---|---|---|
| 1 | `make image` | Reads RAM, CPU registers and the device tree, including the GPU's nodes. Writes only our own memory and the screen. |
| 2 | `make image RT_WDT_GUARD=1` | Arms the watchdog so a hang reboots. Enable only after rung 1's final reboot works. |
| 3 | add `RT_NVME_PROBE=1` | Runs m1n1's NVMe driver with read commands. If it passes, m1n1 can chainload builds from disk and the recovery trip per build goes away. |
| 4 | add `RT_GPU_PROBE=1` | One guarded read at the start of each GPU register range. |
| 5 | add `RT_GPU_POWER=1` | Switches the GPU power gates on and off through m1n1's power manager, with reads in between. |

## GPU reverse engineering so far

Run under macOS with `make -C gpu-re corpus analyze trace`. Details and evidence are in [gpu-re/FINDINGS.md](gpu-re/FINDINGS.md).

- **The compiled code is reachable.** A serialized Metal binary archive contains an `applegpu` Mach-O whose `__compute` section is a nested object holding the machine code.
- **Some encodings are decoded and tested.** The 32-bit constant move, the 8-bit add immediate and the 4-bit truth table of bitwise instructions are located bit by bit, and the maps predict unseen samples exactly.
- **The encoding is not the published M1 one.** The M1 reference's opcodes for stop, constant moves, loads and stores do not appear.
- **The driver interface is visible.** One user client (`AGXAcceleratorG17X`), about fifteen selectors and two fast traps cover a compute run.

This does not yet run anything on the GPU without macOS.

## Next milestones (not started)

- **Boot rung 1 on the hardware.** Everything bare-metal waits on this.
- **DART**: `sgx` has no `dart-sgx` on this SoC; the GPU coprocessors point at `mapper-gfx-asc` (`iommu-mapper,gfx`). That differs from M1 and needs working out first.
- **GPU power**: `sgx` lists gates `0x268` and `0x267`; m1n1's `pmgr.c` knows `pmgr1,t6050`. Rung 5 is the first test.
- **Firmware**: `gfx-asc` and `gfx1-asc` are `iop,ascwrap-v6`, for m1n1's `asc.c` and `rtkit.c`.
- **Instruction set**: register fields, the constant program and `__GPU_METADATA` (see the findings' open questions).

## Strategies from the reverse-engineering literature

- **Record and replay before understanding** (GPUReplay, ASPLOS 2022): capture the vendor stack's register and memory traffic for one workload and replay it. A working replay is a valid first compute milestone.
- **Trace at the hardware boundary** (Linux mmiotrace and Nouveau; the m1n1 hypervisor): log what the vendor driver writes, per device.
- **Differential testing** (Panfrost; Rosenzweig's "Dissecting the Apple M1 GPU"): change one input, diff the output bytes, and build a driver early to test each hypothesis.
- **Watch the doorbell** (arXiv 2604.26889 on NVIDIA command streams): find the submission register and capture at that point.

All four need macOS tracing, which on M5 means the m1n1 hypervisor and, in practice, a second computer.
