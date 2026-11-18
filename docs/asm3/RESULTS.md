# asm3 — results (stock clocks: V3D 300 MHz / core 400 MHz, untouched)

Work tree: `/home/raspi/work/asm3` (copy of `asm2`; `asm2` was not modified).
Technical detail, root cause and probes: `NOTES.md`. Raw per-rep rows:
`runs.jsonl`.

## Kernel

Hand-written QPU GEMM, 16x16 C tile per QPU request, 16 lanes = the 16 columns,
16 C accumulators in ra0..ra15, one k per 39-word loop iteration (156 cycles =
the issue floor), packed `fmul`+`fadd`, hardware rotate+replicate broadcast,
A read from a **k-major packed panel** (one 64 B line per request, `+64` per k),
B straight from row-major B, store 4 chunks x 4 rows (`units=4 depth=16
stride=(N-16)*4`), tile = `u5 + iter*nqpu`.

Two changes against `asm2`:

1. **A panel** (`pack.py`, run once per GEMM as a second launch in the same
   process): per 16x16 tile, VDR (VPM DMA load, pitch N*4) -> 16 VPM rows,
   then a **vertical** VDW (Depth=16 rows, Units=16 elements) which the VPM
   writes out transposed as one contiguous 1 KB block. 13 instructions + 2 DMA
   round trips per tile, <=4 QPUs (16 VPM rows each).
2. **TMU issue and TMU pop never share an instruction word** — this is the fix
   for the 12-QPU corruption (details in `NOTES.md` §1).

## Final numbers — 12 QPUs, 5 runs per size (6 measured, all quoted)

| N | runs clean | pack ms | GEMM ms | total ms | GFLOP/s total | GFLOP/s GEMM | cyc/k issued | cyc/k wall | mismatches |
|---|-----------|---------|---------|----------|---------------|--------------|--------------|-----------|------------|
| 128 | **6/6** | 0.063 | 0.464 (best 0.4635) | **0.527** | **7.97** | 9.05 | 150.0 | 181 | 0/16384 every run |
| 256 | **6/6** | 0.356 | 3.183 (best 3.1811) | **3.539** | **9.48** | 10.55 | 161.3 | 169.5 | 0/65536 every run |

Every output element of every run was compared against the double-precision
oracle (`check_gemm`: `mismatches=0`, max abs error 4.82e-05 = fp32 accumulation
noise, 89/16384 bit-identical to an fp32 accumulation in the same order). The
packed panel itself was compared **bit-exactly** against A every run
(`check_panel`: 0/16384 and 0/65536).

Spread over the 6 reps: N=128 total 0.5264-0.5340 ms; N=256 total 3.5393-3.5490 ms.
Best single rep observed across all campaigns: 0.5264 ms = 7.97 GFLOP/s (N=128),
3.5028 ms = 9.58 GFLOP/s (N=256).
The pack share is stated separately in the table because it is a second launch:
**12.0 % (N=128)** and **10.1 % (N=256)** of the reported total.

## Against the two bars

| path (stock clocks) | N=128 GFLOP/s | N=256 GFLOP/s |
|---|---|---|
| compiled, accepted stack (`v16` + 12-QPU padding) | 2.668 | 3.085 |
| `asm2` hand-written (strided A gather) | 2.17-2.20 | 1.87-1.90 |
| `asm2` coalesced-A timing probe (wrong results, upper bound) | 5.88 | 5.63 |
| **this work, GEMM only** | **9.05** | **10.55** |
| **this work, including the packer** | **7.97** | **9.48** |
| ratio vs compiled, including packer | **2.99x** | **3.07x** |
| ratio vs compiled, GEMM only | 3.39x | 3.42x |
| % of the 28.8 GFLOP/s structural peak (12 QPUs, 1 instr/4 clk) | 27.7 % | 32.9 % |

The coalesced-A probe's 5.88 / 5.63 was a *latency*-bound estimate made with a
2-deep TMU pipeline; the loop now keeps 3 requests in flight per TMU, which is
why the real kernel lands ~1.7x above the probe at N=256.

## Correctness evidence for the race fix

| kernel | head word pattern | 12-QPU clean runs |
|---|---|---|
| `asm2` strided A (as reported) | issue+pop in one word | 2/5 (N=128), 1/5 (N=256) |
| `asm2` strided A, `gemm.py` with the 5-word fix | separated | **5/5 (N=128), 5/5 (N=256)**, same speed |
| coalesced A, `variant 0` | issue+pop in one word | 0/2 clean, 3504-7150 bad elements |
| coalesced A, `variant 1` (used here) | separated | **6/6 (N=128), 6/6 (N=256)** |

Every 12-QPU run of the separated-head kernel that is quoted in this report was
clean: 6/6 at each size in the final configuration, plus 30+ clean 12-QPU runs
during the depth/variant/pack-qpu sweeps (N=128 and N=256, TMU depth 3 and 4,
pack on 1/2/4 QPUs) — against 2/5 and 1/5 for the same code before the head fix.

## Verdict: **KEEP**

* Correctness: 0 mismatches in every element of every run, at both sizes, at 1
  and 12 QPUs, with the panel itself verified bit-exactly. The intermittent
  corruption is gone and its cause is identified, reproduced deterministically
  and removed by a zero-cost instruction reordering (independently confirmed by
  fixing the old `asm2` kernel with that single change).
* Speed: 2.99x / 3.07x the compiled stack **including** the packer, 3.39x / 3.42x
  for the GEMM alone, with stock clocks. Both acceptance bars are cleared with a
  ~3x margin.
* The kernel is now instruction-issue bound (86-92 % of the 156-cycle/k loop
  floor), so the remaining headroom is the instruction mix, not memory:
  the obvious next steps are a 2-column-block tile (halves the A-panel traffic
  per flop) and shading the 6-vs-5 tile imbalance at N=128 (64 tiles / 12 QPUs).

## Reproduce

```sh
cd /home/raspi/work/asm3
gcc -O2 -Wall -o qrun_gemm qrun_gemm.c
gcc -O2 -o check_gemm check_gemm.c -lm
gcc -O2 -o check_panel check_panel.c
python3 run3.py 128 12 --reps 6 --pack-qpus 4     # pack + GEMM, verified
python3 run3.py 256 12 --reps 6 --pack-qpus 4
python3 run2.py 128 12 --cpanel --reps 2          # GEMM only, ARM-built panel
python3 gemm_run.py 128 12                        # asm2 kernel with the head fix
python3 probe_vpm.py 128 vert|vdrread             # the new VPM primitives
```

Artifacts: `gemm2.py` (coalesced GEMM), `pack.py` (packer), `vpmwords.py`
(field-checked VPM control words), `qrun_gemm.c` (two-launch harness),
`run2.py`/`run3.py`/`gemm_run.py` (drivers), `check_gemm.c`, `check_panel.c`,
`diag_gemm.c` + `an.py..an6.py` (error decomposition that found the race),
`probe_vpm.py` + `check_probe.py` (VPM primitive probes), `gemm.py` (asm2 kernel
with the head fix), `runs.jsonl` (raw rows).
