# asm3 — coalesced A panel + the 12-QPU corruption, root-caused and fixed

2026-11-18. Pi 3B, V3D 300 MHz / core 400 MHz (**stock**, untouched; verified with
`vcgencmd measure_clock` before and after the campaign, `get_throttled=0x0`).
Kernel 6.18.50+rpi-v8. All work under `/home/raspi/work/asm3` (a copy of
`asm2`; `asm2` was not modified). Every GPU run: `flock -w 2400 /tmp/vc4-gpu.lock`
+ `timeout`, `read_v3d_status` after each run. Clocks were never changed.

TL;DR — both targets are met, 6/6 clean runs at 12 QPUs for N=128 and N=256,
including the packing cost:

| N | QPUs | pack + GEMM | GFLOP/s (total) | GFLOP/s (GEMM only) | mismatches |
|---|------|-------------|-----------------|---------------------|------------|
| 128 | 12 | 0.063 + 0.464 = **0.527 ms** | **7.97** | 9.05 | 0/16384 (6/6 runs) |
| 256 | 12 | 0.356 + 3.183 = **3.539 ms** | **9.48** | 10.55 | 0/65536 (6/6 runs) |

compiled stack for comparison (`v16` + 12-QPU padding, stock clocks): 2.668 / 3.085.
So 2.97x / 3.07x, with the packing included.

---

## 1. The intermittent 12-QPU mismatch: what it was

### 1.1 It is one corrupted TMU payload, not an ordering bug

`diag_gemm.c` + `an*.py` decompose `C - ref` per 16x16 tile. For every corrupted
tile the error matrix is **rank 1** to ~1e-4:

    err[r][c] = x[r] * B[k][c]        (exactly one k, B row exact to 1e-5)

so one k-term used a *substituted* A vector (the B factor of that term is intact,
so the A/B pairing in the FIFOs was correct). That rules out FIFO/ordering
problems, the VPM store chunking, address arithmetic and the C store: it is the
*data* of a single A request.

### 1.2 What triggers it: a TMU issue and a TMU pop in the same instruction

The `asm2` k-loop head (39-word loop) was:

```
0: nop.load_tmu0                       pop A_k          -> r4
1: (spacer)
2: or  acc0, r4, r4                    A vector
3: or  tmu0s, AADR.load_tmu1           issue A_{k+2}  +  pop B_k     <-- both
4: or  tmu1s, BADR                     issue B_{k+2}
```

Word 3 writes a TMU address register **and** carries a `load_tmu1` signal.
Measured behaviour (new coalesced kernel, identical except for that word):

* with the shared word (`variant 0`), 1 QPU, TMU depth 1 / 2 / 3 / 4:
  the corrupted k is **exactly k = depth** for depth 1 and 2 (100% reproducible,
  `mm=14289` / `14302` of 16384), and clean for depth 3 and 4. k=depth is the
  first request *issued inside* the k loop, i.e. the one issued by that word.
* with 12 QPUs the same kernel gives 3504-7150 mismatches on every run, mostly at
  k=3 (= depth) plus scattered k's: same word, higher request rate, more events.
* `asm2`'s strided-A kernel has the same word: 2/5 clean at N=128, 1/5 at N=256
  (this is the bug reported as "loses one tile in 12 of its 16 lanes").

### 1.3 The fix (and the proof)

Keep the A/B pops and the TMU issues in *separate* instruction words. Nothing else
changed; the loop is still 39 words, so it costs nothing:

```
0: nop.load_tmu0                       pop A_k -> r4
1: or  acc0, r4, r4                    A vector (1 slot is enough for an accumulator)
2: nop.load_tmu1                       pop B_k -> r4
3: or  tmu0s, AADR                     issue A_{k+2}
4: or  tmu1s, BADR                     issue B_{k+2}
```

| kernel | head | 12-QPU result |
|---|---|---|
| `asm2` strided A | issue+pop shared | 2/5 clean (N=128), 1/5 (N=256) |
| `asm2` strided A, `gemm.py` fixed | separated | **5/5, 5/5 clean, same speed** |
| coalesced A, `variant 0` | issue+pop shared | 0/2 clean, 3504-7150 bad elements |
| coalesced A, `variant 1` | separated | **6/6, 6/6 clean** (with packing) |

The `asm2` numbers are from `gemm_run.py N 12` five times each; the only edit to
`gemm.py` is the head above (5 instructions, never executed differently).
So the original intermittent bug and the deterministic 1-QPU one are the same bug.

Related documented hazard (VC4C `Register.h`, from the Broadcom spec): r4 is the
single TMU result register and "there is a delay of at least 8 instructions
between writing the TMU address and issuing the TMU load signal". A pop signal
attached to the issuing word is not covered by that rule; it is what corrupts.

---

## 2. Coalesced A: the packed panel

### 2.1 Layout (k-major)

    PANEL[(rb*N + k)*16 + r] = A[rb*16 + r][k]          rb = 0 .. N/16-1

Same size as A (64 KB / 256 KB). The GEMM's per-k A request is then 16
consecutive words = **one 64 B line**, and the address is simply `+64` per k:

    AADR = PANEL + (rowblk << log2(64*N)) + lane*4 ,  AADR += 64 per k

versus `asm2`'s `A + rowblk*16*N*4 + lane*N*4 , += 4` which touched 16 different
lines per request. In the kernel this is three small edits (lane shift 2 instead
of log2(N*4), a 64-byte step held in a general register because the ALU
immediate field is only -16..15, and the panel base from uniform u4). The k-loop
stays at **39 words** and the 1-QPU time is unchanged (4.647 ms, 0.903 GFLOP/s),
i.e. the kernel is doing the same work with a better memory pattern.

### 2.2 The packer: two VPM DMAs per 16x16 tile, no ALU data movement

`pack.py`. Each QPU owns 16 VPM rows (row base `16*q`), so at most 4 QPUs.

Per 16x16 tile (row block `rb`, k block `kb`), 13 instructions + 2 DMA:

1. **VDR** (VPM DMA load, ID=1, `vpmwords.vdr_basic`): RowLength=16 words,
   NumberRows=16, `VPitch=1`, memory pitch = `N*4` bytes (VPR stride setup ID=9)
   -> the 16 rows of the A tile land in VPM rows 16q..16q+15.
2. **VDW vertical** (VPM DMA store, ID=2, `vpmwords.vdw_basic(row, 16, 16,
   horizontal=0)`): Depth=16 = VPM rows, Units=16 = elements per row. The
   hardware writes the tile **transposed** as 16 consecutive 64 B vectors, i.e.
   exactly one contiguous 1 KB block `PANEL[rb][kb*16 .. kb*16+15][0..15]`.
   (Documented in VC4C `src/periphery/VPM.h`, worked example; verified on
   silicon, see 2.4.)

QPU `i` packs row blocks `i, i+nqpu, ...` of `N/16` (uniform u1 = its first row
block byte offset, u5 = count, u6 = index). Uniforms are computed by the harness
so the kernel needs no multiplies or log2.

### 2.3 Cost (measured, included in the reported totals)

| N | pack QPUs | pack | GEMM | pack share |
|---|-----------|------|------|-----------|
| 128 | 1 / 2 / 4 | 0.123 / 0.063 / 0.063 ms | 0.464 ms | **12.0 %** (4 QPUs) |
| 256 | 1 / 2 / 4 | 0.556 / 0.363 / 0.356 ms | 3.183 ms | **10.1 %** (4 QPUs) |

The packer is DMA-latency bound (~0.9 µs per 1 KB tile pair at 2-4 QPUs);
4 QPUs is the maximum (VPM rows) and is what the final numbers use.

### 2.4 The new primitives, verified one at a time

`probe_vpm.py` + `check_probe.py` (each run element-verified, `STATE: LIVE` after):

| probe | primitive | result |
|---|---|---|
| `control` | generic VPM write + horizontal VDW (the asm2 store path) | 0/64 bad |
| `vert` | generic VPM write of 16 rows + **vertical** VDW | 0/256 bad (exact transpose) |
| `vdrread` | **VDR** load of a 16x16 A tile + generic VPM read + proven store | 0/256 bad |
| full `pack.py` | 64 / 256 tiles, 1 and 2-4 QPUs | **0/16384**, **0/65536** bad |

`check_panel` compares the packed panel bit-exactly against A on every measured
run (Pmm=0 in all runs below).

---

## 3. Two more bugs found on the way (harness-level, worth knowing)

1. **Two `qrun_gemm` processes = two dma-buf arenas.** The first pack/GEMM
   attempt produced C=0 because the second process allocated a fresh arena and
   read zeros where the panel should be. `qrun_gemm.c` now runs an optional pack
   program and the main program **in one process, one arena** (`QRUN_PACK_BIN`,
   `QRUN_PACK_NQPU`), reporting `PACK wall_ns=...` and `DONE wall_ns=...`
   separately.
2. **The QPU instruction fetch goes through the L2.** After overwriting the code
   area, a second launch can execute the *previous* program: observed signature
   was the packer running with the GEMM's uniforms (VPM row base 16*12 = 192 ->
   `v3d_errors=0x1030`, VPM read/write range errors, C=0). Fix: a distinct code
   *and* uniform offset per launch (`0x0000/0x8000` for the packer,
   `0x4000/0x8800` for the GEMM) plus an `SLCACTL` invalidate before each launch.
   `L2CACTL=1<<2` between the launches is safe (it writes the packed panel back)
   and changes nothing measurable.

Board-safety record: three unplanned resets happened during probe development —
one on the first packer attempt (a 4-bit VPM field overflow), one on the second
(same class: `add` used where a copy was needed, so every VPM setup word was
doubled), and one when a prototype harness launched a stale program (see §3.2)
which left a QPU stuck at a wild address. After the third, a manual reboot was
needed because the stuck QPU's DMA had left userland unstable. No work was lost
(`asm2` untouched, `asm3` scratch only) and every run in the campaign below was
`STATE: LIVE` / SRQCS clean before and after.

Also worth recording, both cost a board reboot while they were being found:

* VPM setup fields are 4 bits: `RowLength`/`NumberRows` = 16 **must be encoded as
  0** (writing 16 overflows into `MPitch` and the DMA never completes). All setup
  words are now generated by `vpmwords.py`, which range-checks every field and
  reproduces the four constants already proven in `asm2` (`0x80845A00`,
  `0x82104000`, `0xC0010000`, `0x00001A34`).
* generic VPM read setup `Number` = bits 20-23 (0 => 16 vectors); a setup with
  Number=1 followed by 16 `or rX, vpm, vpm` stalls the QPU forever.

---

## 4. Final measurements (12 QPUs, stock clocks, 300 MHz)

`run3.py` = pack + GEMM in one process; each rep rebuilds the panel and verifies
(a) the panel bit-exactly against A, (b) every C element against the
double-precision oracle (`check_gemm`, tolerance 1e-4 relative).

N=128, 6 reps:

```
rep0  pack 0.0660 + gemm 0.4636 = 0.5296 ms  gemm 9.047 GFLOP/s total 7.919  cyc/k 150.0/181.1  Cmm=0 Pmm=0
rep1  pack 0.0632 + gemm 0.4661 = 0.5294 ms  gemm 8.998 total 7.923        cyc/k 150.0/182.1  Cmm=0 Pmm=0
rep2  pack 0.0669 + gemm 0.4671 = 0.5340 ms  gemm 8.979 total 7.854        cyc/k 150.0/182.5  Cmm=0 Pmm=0
rep3  pack 0.0632 + gemm 0.4660 = 0.5293 ms  gemm 9.000 total 7.925        cyc/k 150.0/182.0  Cmm=0 Pmm=0
rep4  pack 0.0629 + gemm 0.4635 = 0.5264 ms  gemm 9.049 total 7.968        cyc/k 150.0/181.1  Cmm=0 Pmm=0
rep5  pack 0.0630 + gemm 0.4662 = 0.5292 ms  gemm 8.998 total 7.926        cyc/k 150.0/182.1  Cmm=0 Pmm=0
clean 6/6
```

N=256, 6 reps:

```
rep0  pack 0.3674 + gemm 3.1816 = 3.5490 ms  gemm 10.546 GFLOP/s total 9.455  cyc/k 161.3/169.5  Cmm=0 Pmm=0
rep1  pack 0.3613 + gemm 3.1848 = 3.5461 ms  gemm 10.536 total 9.462        cyc/k 161.3/169.6  Cmm=0 Pmm=0
rep2  pack 0.3555 + gemm 3.1839 = 3.5394 ms  gemm 10.539 total 9.480        cyc/k 161.3/169.6  Cmm=0 Pmm=0
rep3  pack 0.3563 + gemm 3.1830 = 3.5393 ms  gemm 10.542 total 9.480        cyc/k 161.3/169.5  Cmm=0 Pmm=0
rep4  pack 0.3566 + gemm 3.1859 = 3.5426 ms  gemm 10.532 total 9.472        cyc/k 161.3/169.7  Cmm=0 Pmm=0
rep5  pack 0.3606 + gemm 3.1811 = 3.5417 ms  gemm 10.548 total 9.474        cyc/k 161.3/169.4  Cmm=0 Pmm=0
clean 6/6
```

`cyc/k` = V3D EXECUTION_CYCLES per QPU per k / wall-clock-equivalent cycles per k
(wall x 300 MHz / (ceil(tiles/QPUs) x N)), the two columns `asm2` used.
The issue floor is 39 words x 4 clocks = **156 cycles/k**, so the k loop now runs
at 92 % (N=256) / 86 % (N=128, which also carries the 6-vs-5-tile imbalance of
64 tiles over 12 QPUs) of its instruction-issue limit. Memory is no longer the
limiter: the 12-QPU coalesced timing probe in `asm2` predicted 5.88 / 5.63
GFLOP/s as an upper bound for this loop; the kernel now does 9.05 / 10.55 with
the same instruction stream, because the A-panel request is one line instead of
sixteen.

Other reference points (same harness, same clocks):

| config | N=128 | N=256 |
|---|---|---|
| coalesced A, 1 QPU | 4.647 ms / 0.903 GFLOP/s, 0 mismatches | - |
| coalesced A, 12 QPUs, panel built on the ARM (dev path) | 0.432 ms / 9.71 | 3.107 ms / 10.80 |
| + QPU packer (this work, reported above) | 0.527 / 7.97 | 3.539 / 9.48 |
| `asm2` hand-written (strided A), 12 QPUs | 1.90 ms / 2.17 | 17.7 ms / 1.89 |
| compiled `v16` + 12-QPU padding | 2.668 | 3.085 |
