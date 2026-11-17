# M21 — VC4 GEMM performance: findings, measurements and the lane-block store feature

Working report, 2026-11-18. Board: Raspberry Pi 3B (BCM2837, 4x Cortex-A53 @1.2 GHz,
VideoCore IV V3D), Debian 13 trixie arm64. VC4CL `0185a1e`, VC4C `15bb418` unless stated.

Everything here was measured on the board, one GPU process at a time, with
`read_v3d_status` after each batch (`STATE: LIVE`, SRQCS clean, no wedge). All results
were verified element-by-element against a double-precision CPU oracle.

---

## 1. Goal and prior state

The workstream is "make VC4CL's GEMM as fast as the hardware allows". The prior session's
brief is a numbered task list with reference timings; M19 labels its sections by task item:

| item | content | state before this run |
|---|---|---|
| 1 | register allocator emitted silently wrong code | fixed in M20 (fail-closed) |
| 2 | kernel-shape search | done: `v16` (16x1) is the best expressible shape |
| 3 | `__local` staging | done: dead end (1 QPU per work-group costs ~7x) |
| 4 | launch geometry | done: pad the grid to 12 QPUs, 1.22-1.27x |
| 5 | what blocks real data reuse | identified: the merge gate rejects 2D tiles |

Target implied by the gate contract: ~2.6 GFLOP/s at N=128, i.e. match the four A53 cores.
The blocked 4x4 tile is modelled at 1.77x fewer instructions/MAC.

## 2. CPU reference (new)

`~/m21/cpu_gemm.c`, `gcc -O3 -mcpu=cortex-a53 -fopenmp`, row-major jki sgemm, best of 5,
verified against a double oracle (0 mismatches):

| N | 4 threads | 1 thread |
|---|---|---|
| 32 | 0.059 GFLOP/s (1.107 ms) | - |
| 64 | 0.242 GFLOP/s (2.166 ms) | - |
| 96 | 2.667 GFLOP/s (0.664 ms) | - |
| 128 | 2.696 GFLOP/s (1.556 ms) | 0.674 GFLOP/s (6.226 ms) |
| 256 | 2.725 GFLOP/s (12.313 ms) | - |

The 4 cores scale 3.9x over one core at N=128. Note: a CPU number in
`README.debian13.md` (matmul 64 = 0.513 ms) could not be reproduced with this harness
(2.166 ms); that figure came from a harness that was not committed.

## 3. GPU state and the CPU comparison

Best correct GPU configuration today is `v16` with the M19 12-QPU padding:

| N | GPU | CPU 4T | GPU/CPU |
|---|---|---|---|
| 96 | 0.9592 ms / 1.8448 GFLOP/s | 2.667 | 0.69x |
| 128 | 2.3409 ms / 1.7918 GFLOP/s | 2.696 | 0.66x |
| 256 | 16.9171 ms / 1.9835 GFLOP/s | 2.725 | 0.73x |

The VC4 is ~2.7x faster than one A53 core and ~1.4x slower than all four. The historic
"49-86x slower" figure in the port notes predates merging and 12-QPU geometry and is
obsolete (verified against git history: introduced 2026-11-10, revised 2026-11-11).

Baseline re-run this run confirms the measurement path: `gemm_v16` N=128 unpadded,
8 QPUs = 2.8604 ms / 1.4663 GFLOP/s, 0 mismatches. The static reference reproduces the
port log exactly: 533 instructions / 64 MACs, 8 TMU requests, 60 rotations, 12 NOPs.

## 4. M21's planned workaround — tested and rejected on silicon

M21's plan was to avoid the compiler feature by making the stores satisfy the merge gate:
lanes own interleaved columns (`col = gid + 16j`) so the store advances one element/lane.

- First implementation had no column-block offset and raced past column 64
  (6141 mismatches at N=128). Fixed in `gen21c.py` (lane in x, block+row in y, 3D-safe
  mapping); the corrected kernel is fully correct.
- Correct result: **3.7938 ms / 1.1056 GFLOP/s** vs `v16` 2.8604 ms / 1.4663 — the
  workaround is **1.33x slower** despite fewer instructions/MAC.
- Why: lane-interleaved B loads are per-lane gathers, and the compiler never groups
  gathers (`Memory.cpp:801-803`: `perLaneAddresses` -> skip). The 4x4 then issues
  8 TMU requests per 16 MACs (2.0 MACs/request) instead of 8 per 64 (8.0).
- Measured static costs: int kb1 = 6.94 instr/MAC @ 2.0 MACs/TMU; int kb2 = 6.44 @ 2.0;
  `v16` = 8.33 @ 8.0; `nat` kb4 unmerged = 5.69 @ 3.76.

Conclusion: the workaround cannot reach the target; M21's note estimated 4.0 MACs/TMU,
the real number is 2.0-3.2.

## 5. `vstore4` shortcut — passes the address gate, fails the value analysis

Probe: a `float4` built from a constant and stored per lane with `vstore4`
(`c + gid*4`) **merges** and emits `vdw_setup(memory stride: 0 bytes)`. But when the
vector is built from lane-dependent scalars the compile fails with:

```
calculates a lane-dependent value of more than one 32-bit element: <4 x f32> %vecinit
```

and 3-7 allocator errors. So a per-lane 4-element value is not representable in the
current normalizer; a kernel-side escape does not exist.

## 6. Compiler feature: what was implemented and where it stops

Branch `feat/lane-block-stores` (VC4C), one file changed, **env-gated** by
`VC4C_LANE_BLOCK_STORES` so default behavior is unchanged:

`src/analysis/WorkItemAnalysis.cpp`, `checkLaneDependentMemoryAccesses()`:
- a write whose per-lane address step is a known multiple of the element size (or cannot
  be proven) is accepted instead of rejected.

Result with the flag: `nat` 4x4 kernels **merge** (16 work-items) with **0 allocator
errors** for `r4c4k1`, `r4c4k2`, `r4c2k4`, `r2c4k4`, `r4c2k2`, `r2c4k2` (N=128).
`r4c4k4` is still refused (4 allocator errors) by the M20 fail-closed path.

The store lowering is the blocker. Tracing it end to end:

1. **VPM area allocation** — each of the C stores is lowered into the *same* single
   per-QPU staging row (`VPM.cpp:1787-1815`, `canBeStagedInSingleRow` `:322`). The
   stores overwrite one row and are DMA'd immediately, one at a time.
2. **Grouping never fires** — `findGroupOfVPMAccess` (`Memory.cpp:358`) requires a
   constant `workGroupConstantOffset` per access. The frontend folds the column offset
   into the *dynamic* offset (`or ..., 1/2/3`, all reported as `work-group uniform
   offset 0`), so `group.stride` resolves to 0 and the pass breaks. No
   "Combining ... writes" line appears; the asm contains 16 horizontal
   `vdw_setup(memory stride: 0 bytes)`.
3. **Wrong layout if it did** — horizontal mode writes value-major
   `[lane0 v_j][lane1 v_j]...`; a C-element per-lane block needs lane-major.
   The hardware can transpose this: VDW **vertical** mode with `Depth=C`,
   `Units=16`, `Horizontal=0`, `stride=0` yields
   `[lane0 v0..vC-1][lane1 v0..vC-1]...`.
   `insertWriteDMASetup` already performs exactly this swap for the 64-bit path
   (`VPM.cpp:944-961`).

So the remaining work is (a) recognise the C consecutive stores as one multi-row access
before lowering (or give each store a distinct VPM row), (b) let the group pass derive the
stride from the dynamic-offset delta, (c) select vertical for `group.stride == element
size`. (a) is a normalizer change; doing (b)/(c) alone would produce silently wrong
kernels.

## 7. Hardware facts that constrain the feature (VideoCoreIV-AG100-R)

- VPM: 32-bit words, 16 wide x up to 64 rows; QPU reads/writes 16-way vectors; vertical
  32-bit vectors must start on a row multiple of 16 (p. 53).
- VDW DMA store, `VPMVCD_WR_SETUP` ID=2: `UNITS` = rows in memory, `DEPTH` = row length,
  `VPMBASE = {Y[6:0], X[3:0]}` (Table 34). Stride setup ID=3: `BLOCKMODE` plus `STRIDE`
  = bytes from the end of one row to the start of the next (Table 35).
- Vertical mode transposes: `Depth` = VPM rows, `Units` = elements per row; the worked
  example is documented in `src/periphery/VPM.h:130-210`.
- Separately: `V3D_VPMBASE` reserves VPM for user programs in 256-byte units
  (Table 69), relevant to per-QPU VPM sizing.
- Board health: dmesg clean (vc4 probe normal); `get_throttled=0x50000` means
  under-voltage and throttling have occurred since boot (not active). Check the PSU
  before long GPU runs.

## 8. Repository audit (context for the report)

- `~/vc4cl/VC4CL`: `perf/2d-split`, clean, HEAD `0185a1e`. Two build trees:
  `build/` (`86f2931`, Nov 12 22:15) and `build-icd/` (`e8a85fa`, Nov 12 07:15).
  The installed `/usr/local/lib/libVC4CL.so.0.4.9999` and the active ICD come from
  `build-icd`; it is 6 commits behind HEAD but `git diff e8a85fa..HEAD -- src include`
  is empty (docs only), so the runtime is functionally current.
- `~/vc4cl/VC4C`: `perf/gather-reads`, HEAD `15bb418`; binaries newer than all sources;
  `build/src/git_commit.h` names `6fb14c4` (one commit behind, cosmetic).
- `~/vc4top`: clean, up to date; requires root (`/dev/mem` + `mmap`); one stale comment
  in `vc4_defines.h:145` still calls VPMBASE a "base address".
- Both VC4CL and VC4C are configured with `BUILD_TESTING=OFF` and `ctest -N` = 0 tests,
  despite 25 test translation units in VC4C's tree. The recent register-allocation work
  is therefore unverified by the project's own suite.

## 9. Artefacts (all under `/home/raspi/m21/`)

- `cpu_gemm.c` - CPU sgemm reference (section 2).
- `gemm21.c` - GEMM harness for lane-interleaved grids; `gemm19` (m19) for `nat` grids.
- `gen21b.py` - parameterised R x C tile generator (map nat/int, vec/sca).
- `gen21c.py` - lane-interleaved tile with explicit column blocks (correct on hardware).
- `gen21d.py` - `vstore4` variant (blocked by the value analysis).
- `src/`, `asm/`, `logs/` - generated kernels, dumps, compile logs.
- `NOTES-lane-block-stores.md` - short version of section 6 with rebuild/revert commands.
- `stat.sh`, `asmanalyze.py` - static instruction/TMU accounting (from m19).

## 10. Reproduction

```sh
# static cost of a kernel
cd ~/m21 && ./stat.sh src/gemm_vst4_vb1_n128.cl vst4 64

# GPU run (serialized; uses m10 ICD + DMA-heap + register poking)
./run21.sh ~/m19/gemm19 ~/m10/src/gemm_v16.cl gemm_v16 128 16 1 10

# CPU reference
gcc -O3 -mcpu=cortex-a53 -fopenmp -o cpu_gemm cpu_gemm.c -lm && ./cpu_gemm 128 5 4

# compiler experiment (branch feat/lane-block-stores)
cd ~/vc4cl/VC4C/build && cmake --build . --parallel 2
VC4C_MERGE_WORK_ITEMS=16 VC4C_LANE_BLOCK_STORES=1 \
  ./src/vc4c --verbose --asm --no-kernel-info ~/m21/src/nat_r4c4k2.cl -o /tmp/x.asm
```

## 11. Bottom line

- The M21 workaround is dead: measured 1.106 GFLOP/s vs 1.466 for `v16`.
- The best correct GEMM remains `v16` + 12-QPU padding: 1.79-1.98 GFLOP/s,
  i.e. 1.4x slower than the four A53 cores and 2.7x faster than one.
- The only remaining route to ~2.6 GFLOP/s is the lane-block store feature. The gate is
  open and merging works; the work left is the multi-row VPM normalizer change plus
  vertical DMA selection, followed by an on-hardware A/B.
- Nothing in this report changed the installed runtime or the compiler of record;
  the experimental compiler change is on a branch and disabled unless the environment
  variable is set.
