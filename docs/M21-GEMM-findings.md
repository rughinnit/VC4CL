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
- `gen21e.py`, `gen21f.py` - nat-map compute with gate-compatible interleaved stores.
- `launch.c` - launch-overhead microbenchmark (0.129 ms per empty enqueue+finish).
- `v3dclk.c` - runtime clock setter via the firmware mailbox (section 11).
- `vc4-clock.service` - persistence unit (gated by `/etc/vc4-clock.conf`).
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

# clock (root; V3D id 5, core id 4)
sudo ~/m21/v3dclk 5 500 && sudo ~/m21/v3dclk 4 550
vcgencmd measure_clock v3d

# compiler experiment (branch feat/lane-block-stores)
cd ~/vc4cl/VC4C/build && cmake --build . --parallel 2
VC4C_MERGE_WORK_ITEMS=16 VC4C_LANE_BLOCK_STORES=1 \
  ./src/vc4c --verbose --asm --no-kernel-info ~/m21/src/nat_r4c4k2.cl -o /tmp/x.asm
```

## 11. Clock scaling — the dominant lever

The V3D clock was 300 MHz (core 400). It can be raised at runtime through the
firmware mailbox (`SET_CLOCK_RATE`), with no reboot and no config edit. Measured on
the padded `r16g` GEMM (12 QPUs), N=128 unless stated, element-verified:

| V3D / core (MHz) | N=128 GFLOP/s | N=256 GFLOP/s | result |
|---|---|---|---|
| 300 / 400 (stock) | 1.792 | 1.984 | PASS |
| 400 / 500 | 1.905 | - | PASS |
| 450 / 500 | 2.096 | - | PASS |
| 500 / 500 | 2.289 | - | PASS |
| **500 / 550** | **2.725** | **3.090** | PASS, 10/10 reps |
| 550 / 550 | 2.473 | - | **FAIL**, 3382 mismatches |
| 525 / 600 | - | - | **GPU hang**, board needed a reboot |

The speedup tracks the clock almost exactly (1.33x clock -> 1.30x speed), so the
kernel is clock-bound, not memory-bound. 500 MHz V3D / 550 MHz core is the stable
maximum found: 1.52x at N=128 and 1.56x at N=256 over stock, crossing the ~2.6
GFLOP/s target (2.73 and 3.09).

Correctness at the overclock, shipped harnesses, zero mismatches:
`bench_vc4cl all` 5/5 (matmul 64 = 1.114 ms, was ~1.72 ms), `dnnbench conv`
2612 ms (was 4125 ms), `dnnbench convclamp` 109.5 ms (was 174.5 ms), padded GEMM
10/10 reps at N=128 and 5/5 at N=256. Temperature 58-61 C, `get_throttled=0x0`
after the reboot (the earlier under-voltage flags were from the previous boot).

Persistence: `/etc/systemd/system/vc4-clock.service` (oneshot, after
`multi-user.target`) runs `v3dclk 5 500` and `v3dclk 4 550`; it is gated by
`ConditionPathExists=/etc/vc4-clock.conf`, so removing that file restores stock
clocks at the next boot. 550 MHz V3D produced wrong results and 525/600 hung the
GPU, so 500 is the cap used.

## 12. Compiler-level headroom (investigated 2026-11-18)

The 64-bit QPU word carries one add-ALU and one mul-ALU operation, and VC4C models
both (`ALUInstruction::Addition`/`Multiplication`). Measurements on `v16` (merged,
12 QPUs):

| build | loop body | packed words in loop | TMU |
|---|---|---|---|
| `-O0` | 1055 | 0 | 20 |
| `-O2` (runtime default) | **517** | **0** | 8 |
| `-O3` | allocator refuses (`rc=6`) | - | - |
| `-O3 --fno-schedule-instructions` | 517 (identical to `-O2`) | 0 | 8 |

- `-O3` adds only `schedule-instructions`; that pass is what pushes the allocator
  over the edge, so the scheduling that would create packable pairs cannot run.
- A post-register-mapping combiner already exists (`combineRegisterMappedOperations`,
  `Peephole.h:44`) but only merges **adjacent** instructions. The loop is a dependent
  chain (`fmul acc`, delay `or`, `fadd acc`), so the pair that could co-issue is never
  adjacent.
- Pass-toggle sweep at `-O2`: no `--fno-*` flag reduces the 517 words; `reorder` is
  essential (674 words without it), everything else is neutral.
- `-O3` shape sweep: the best alternative (8 rows, k-block 4) compiles but reaches
  9.00 instr/MAC and 5.33 MACs/TMU, both worse than `v16` (8.08 / 8.00).

Conclusion: the remaining headroom is in codegen, not in flags or kernel shapes. The
two bounded options are (a) a windowed post-RA packer reusing `MERGE_CONDITIONS` and
safe swaps (cannot change register assignments), or (b) a lookahead bound on
`schedule-instructions` so it produces the packing order inside the register budget.

### Final measured ceiling (500 MHz V3D / 550 MHz core)

| N | kernel | best ms | GFLOP/s | result |
|---|---|---|---|---|
| 64 | `v16` | 0.3340 | 1.570 | PASS |
| 96 | `v16` | 0.6366 | 2.780 | PASS |
| 128 | `r16g_128`, padded | 1.5386 | 2.726 | PASS |
| 256 | `r16g_256`, padded | 10.8891 | 3.082 | PASS |

10 reps each, zero mismatches; 63.4 C; `get_throttled=0x0`.

## 14. Measured hardware ceiling (hand-written QPU code, 2026-11-18)

Correction to earlier sections: the QPU retires **one instruction per 4 clocks**
(confirmed twice, by the V3D cycle counter and wall clock, at 1k and 1M loop
iterations). The structural peak is therefore

```
12 QPUs x 16 lanes x 2 ALU ops x 1/4 instr/clock x 300 MHz = 28.8 GFLOP/s
```

not the 192 GFLOP/s that assumes 1 instruction/clock. At 500 MHz the peak is 48.

- Hand-written 12-QPU packed mul+add loop (register operands, deep unroll):
  **28.36 GFLOP/s = 98.5 % of the structural peak**, with perfect 12x scaling.
- Same loop with one TMU request per iteration, 2-deep pipelined: **0.32 GFLOP/s**.
  The cycle counter is identical to the no-memory run, so the loss is `load_tmu0`
  stall time: latency-bound, not bandwidth-bound (~129 MB/s in flight).
- Compiled GEMM at 500 MHz is 3.08 GFLOP/s = **6.4 % of the 48 GFLOP/s peak** and
  ~9.6 % of the mix-limited bound (124 mul-slot vs 380 add-slot ops per iteration
  cap it at 1.33 ops/slot ~ 66 % of peak). ~90 % of the gap is stalls.
- Open item: VPM->DRAM readback in the hand-written harness never completed (one
  attempt wedged `vpw_wait`), so those numbers rest on instruction counts plus the
  independent cycle counter, not on reading QPU output.

## 15. Compiler tracks (2026-11-18)

| track | outcome |
|---|---|
| A allocator spill/free-and-retry | **accepted**: 122 -> 130 kernels compile, 8 previously refused now correct, 0 regressions, all 122 prior kernels byte-identical, v16 unchanged (2.93 ms / 1.43 GFLOP/s, 0 mismatch), bench_vc4cl 5/5. Branch `perf/spill-retry` (bda5bc6). |
| B windowed post-RA packer | rejected: correct but unencodable. One `raddr_a` per word means `fadd` and `fmul` (both file-A readers) can never share a word; the only encodable pair (`fmul` + `or r0,rb0,rb0`) trades 1:1 with a delay NOP (517 words before and after). Loop is add-ALU bound (380 add vs 124 mul ops) and delay-tight. |
| C software pipelining | rejected: registered before `group-memory`, so it sees 16 pre-group requests, exceeds the 4-deep FIFO and is inert on the GEMM; where it fires it hoists a request past the instruction that fills its address register -> bench_vc4cl 3/5 with thousands of mismatches. |
| D hand-written QPU ceiling | 28.36 GFLOP/s ALU-only (98.5 % of peak); 0.32 GFLOP/s with a 2-deep memory pipeline. |

Remaining opening from B: give the product its own accumulator so `fmul->fadd`
distance drops to 1, collapsing a 5-word group to 4 (517 -> ~453, ~12 %). That is
a pre-register-allocation change, not a peephole.

## 16. Product-accumulator allocation (accepted)

Change: bias the allocation of a multiply result that has a single ADD-ALU use
within 4 slots into an accumulator (r0-r3), and delete the copy that used to
materialize the product. `fmul->fadd` distance drops from 2 to 1, the group goes
from 5 words to 4. It is a preference only: on failure the graph is recoloured
without it, so compilability never regresses.

| metric | before | after |
|---|---|---|
| v16 k-loop words | 517 | **453** (-12.4%) |
| slots / nops | 526 / 13 | 462 / 9 |
| `or` slots | 176 | 116 |
| whole kernel words | 1056 | 992 |
| corpus (129 kernels) | - | 50 smaller, 0 bigger, 0 compile regressions |

GPU, stock clocks (300/400), one job at a time, zero mismatches:

| N | before | after | gain |
|---|---|---|---|
| 96 | 1.845 | **2.169** | +17.6% |
| 128 (padded) | 1.792 | **2.061** | +15.0% |
| 256 (padded) | 1.984 | **2.311** | +16.5% |

`bench_vc4cl all` 5/5. Branch `perf/product-acc` (c32d3d8).

Note: literal packed `fadd ...; fmul ...` words remain impossible - a word has one
`raddr_a` and one `raddr_b`, `fmul` reads `rep` (file B) and `fadd` reads the
running sum (file A). The 12.4% comes from deleting the copy, not from packing it.

Idle-board CPU reference for comparison (4x A53, `-O3 -fopenmp`): 2.632 / 2.681 /
2.724 GFLOP/s at N=96/128/256. The patched VC4 at stock clocks is therefore 0.77-0.85x
the four cores; at the 500 MHz setting (currently off) it would be ~1.3-1.4x.

## 17. Corrected TMU pipelining (accepted)

Registered after `lower-memory-access` and `group-memory` (the previous attempt ran
before grouping and was inert). Uses both TMU0 and TMU1, at most 2 requests in
flight per TMU, moves each request together with the address-calculation slice that
produces it, keeps issue and pop order, verifies reaching definitions of locals and
fixed registers plus flags, and reverts the whole block if any invariant fails.
Off by default: `VC4C_PIPELINE_LOADS=1..4`.

Static: k-loop stays 453 words (pure re-order). Request->pop distances change from
1,1,1,1 (TMU0) and 2,2,2,3 (TMU1) to 78,141,167,129 and 7,10,8,42, with max 2 in
flight per TMU.

GPU, stock clocks, 0 mismatches everywhere:

| N | off | on | gain |
|---|---|---|---|
| 96 | 2.170 | **2.506** | +15.5% |
| 128 padded | 2.070 | **2.579** | +24.6% |
| 256 padded | 2.311 | 2.312 | none |

`bench_vc4cl all` 5/5, including matmul N=32/64 (the cases the first attempt broke).
Branch `perf/pipeline2` (2720dae, parent c32d3d8). An earlier N=256 mismatch report
was a runner artifact: with the correct environment N=256 is clean both on and off.

Idle CPU reference: 2.632 / 2.681 / 2.724. At stock clocks the VC4 now reaches
0.95x / 0.96x / 0.85x of the four A53 cores.

## 18. Pipelining refinements and the N=256 "no gain" (accepted)

Branch `perf/pipeline3` (dfb5b45, parent 2720dae), clone `/home/raspi/work/pipe3`.

### Why N=256 looked flat

The pass is applied to `r16g_256` exactly like to `r16g_128`: same decision log
(`Pipelined the TMU loads of loop body '%for.body': 8 requests, at most 2 in flight per TMU`),
same 456-word loop body, same request->pop distances (TMU0 78/141/167/129,
TMU1 7/10/8/42), same 4 + 4 requests over both TMUs. Loop selection, trip count
(64 > 1) and TMU counts are therefore all fine, and latency is *more* of a limiter
at N=256, not less:

| | issue floor | measured | stall |
|---|---|---|---|
| N=128, pass on | 1.17 ms | 1.55 ms | 0.38 ms |
| N=256, pass on | 9.34 ms | 11.15 ms | 1.96 ms |
| N=256, pass off | 9.34 ms | 14.52 ms | 5.18 ms |

(issue floor = k-loop words x 4 clocks x iterations / 300 MHz; `EXECUTION_CYCLES`
counts issuing cycles only, so the difference to the wall time is stall.)

The old datapoint (2.311 -> 2.312, "0 mismatches") cannot be reproduced with any
command of the accepted tree: with `GEMM19_PADY=18`, local 64x3 and
`VC4C_PIPELINE_LOADS=2` the same kernel runs 14.52 -> 11.29 ms (2.311 -> 2.973
GFLOP/s) on `pipe2`, reproducible over repeated A/B pairs. Two mechanisms produce
exactly the observed "flat + 0 mismatches + unchanged word count":

- a run without the patched library or without the forwarded variable: the stock
  runner (`m21/run21.sh`, `m19/run19.sh`) puts the installed VC4C first on
  `LD_LIBRARY_PATH` and does not forward `VC4C_PIPELINE_LOADS` at all - that gives
  1.79 / 1.98 GFLOP/s, i.e. the pre-product-acc code, and
- the pass's own fail-closed revert: when a block fails validation the pass keeps
  the original order, which is indistinguishable from "off" in the timings and
  cannot produce a mismatch. The pass logs that at DEBUG level, the acceptance run
  did not capture the log.

Whatever it was, the "no gain at N=256" column is stale: N=256 has the largest
gain of the three shapes.

### Refinement (a): a pop may cross flag setters

The compare chain that builds the per-lane byte offsets of a gather
(`or.setf`/`xor.setf` + `or.ifz`/`ldi.ifz`, up to 32 instructions per request) is a
side effect, and `canMovePopAcross()` rejected every side effect, so the pops of
the scalar (b) loads stopped 8 and 42 instructions behind their requests although
their values are first used ~130 instructions later. A pop is a pure read of its
TMU's receive FIFO, it never reads the flags, and it stays directly in front of its
read of r4, so it may cross them; the flag source of every conditional and the
reaching definition of every value the pop and its r4 read use are still verified
after every single move, and the whole block is validated again at the end.

    TMU1 request->pop:  7 10  8 42   ->   7 10 118 114
    TMU0 request->pop: 78 141 167 129 -> 78 141 167 129  (unchanged)

### Refinement (b): a single gather base out of the fixed r5 (opt-in)

The base address of a gather is broadcast through the fixed per-quad register r5.
r5 is also the register the vector rotations use to extract single vector elements
and the MACs read it every few instructions, so a request that must move together
with its r5 fill can never cross a MAC block and therefore cannot be issued a
section earlier. When the base is one single address for all lanes
(`isSingleBaseAddress()`: a pointer, and not the pointer-per-lane encoding) the
broadcast is unnecessary; `VC4C_PIPELINE_GATHER_BASE=1` lets the address
calculation read it directly. The k-loop shrinks from 457 to 452 words and the r5
fills from 68 to 64 per iteration.

It is not on by default: the base then occupies one general-purpose register in
file A for its live range, and the largest kernels of the corpus are exactly at the
register limit (0 free A registers, 6 free B), so `r16k8_96`, `r16k8_128`,
`r16k8_256`, `r16gc_128` and `r20k4_120` become unallocatable ("repairing it would
copy a vector or pointer local into a temporary register"). Default stays
fail-closed with (a) only; the switch is +1% where it fits.

### GPU, stock clocks (300/400), 0 mismatches, `STATE: LIVE` after every run

| N | off | pass on (a) | + gather base (a+b) |
|---|---|---|---|
| 96 (local 32x6) | 2.172 | **2.566** | 2.575 |
| 128 (pady 9, local 64x3) | 2.072 | **2.644** | 2.677 |
| 256 (pady 18, local 64x3) | 2.313 | **3.053** | 3.085 |

GFLOP/s, 10 reps, best of run, two rounds. Against accepted `perf/pipeline2`
(2.518 / 2.591 / 2.973): +1.9% / +2.0% / +2.7% for (a) and +2.3% / +3.3% / +3.8%
for (a+b). Against the product-accumulator baseline: +18.1% / +27.6% / +32.0% for
(a), +18.6% / +29.2% / +33.4% for (a+b). `gemm_v16` N=128 unpadded: 1.687 -> 2.099
(+24.4%, was 1.996 on `perf/pipeline2`). `bench_vc4cl all` 5/5 with the pass off,
with (a) and with (a+b), including matmul N=32/64 (0.40/1.69 ms).

Corpus sweep (80 kernels that `perf/pipeline2` could compile), default settings:
0 compilation regressions, 75 reordered, 5 identical, word-count delta at most +1
(delay-slot bundling), 2 kernels one word smaller.

Remaining residual at N=256 is 1.96 ms out of 11.0 ms (18%). Deeper TMU queues do
not help: `VC4C_PIPELINE_LOADS=4` moves the (b) pops to 11/160/159/155 and gains
0.7%. What is left is the first a-gather pop of each iteration, which cannot be
moved further without crossing the loop back edge - i.e. cross-iteration
pipelining with a prologue/epilogue, the next structural step.

## 19. Bottom line

- The M21 workaround is dead: measured 1.106 GFLOP/s vs 1.466 for `v16`.
- Best correct VC4 GEMM at stock clocks: `v16` + 12-QPU padding, 1.79-1.98 GFLOP/s.
- With the stable clock setting (500 MHz V3D / 550 MHz core) the same kernels reach
  **2.73 GFLOP/s at N=128 and 3.09 GFLOP/s at N=256**, verified end to end,
  which is above the original ~2.6 target. Clock scaling is linear and is the
  largest single lever found.
- Measured hardware peak is 28.8 GFLOP/s at 300 MHz (one instruction per 4 clocks);
  at 500 MHz the compiled GEMM reached 2.73 (N=128) / 3.08 (N=256), ~6.4 % of peak.
- Hand-written QPU code reaches 98.5 % of peak with register-only operands, but only
  0.32 GFLOP/s with a shallow memory pipeline - latency, not bandwidth, is the wall.
- At fixed clock the remaining headroom is codegen: the k-loop uses 517 words with
  zero add+mul packing, `-O3` cannot allocate, and the post-RA combiner only merges
  adjacent instructions (section 13). A windowed post-RA packer or a bounded
  scheduler is the next step; kernel shapes and compiler flags are exhausted.
- The only remaining route to ~2.6 GFLOP/s is the lane-block store feature. The gate is
  open and merging works; the work left is the multi-row VPM normalizer change plus
  vertical DMA selection, followed by an on-hardware A/B.
- Nothing in this report changed the installed runtime or the compiler of record;
  the experimental compiler change is on a branch and disabled unless the environment
  variable is set.
