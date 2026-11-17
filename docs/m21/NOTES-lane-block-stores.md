# M21b - lane-block stores: gate opened, lowering still blocks (2026-11-18)

## What was changed (branch feat/lane-block-stores, VC4C)
src/analysis/WorkItemAnalysis.cpp, checkLaneDependentMemoryAccesses():
- behind VC4C_LANE_BLOCK_STORES, a write whose per-lane address step is a known
  multiple of the element size (or unprovable) is accepted instead of rejected.
- Without the env var the compiler behaves exactly as before (all changes gated).

## Result
- `nat` 4x4 kernels now MERGE (16 work-items) for these shapes, 0 allocator errors:
  r4c4k1, r4c4k2, r4c2k4, r2c4k4, r4c2k2, r2c4k2  (N=128, scalar A/B loads)
- r4c4k4 still fails: 4 allocator errors (fail-closed path, unchanged).

## Blocker
groupVPMWrites (src/optimization/Memory.cpp:561) never groups the C stores:
no "Combining ... writes" log line, and the asm contains one horizontal
`vdw_setup(memory stride: 0 bytes)` per store. Horizontal mode writes
[lane0 v_j][lane1 v_j]... (value-major), i.e. the wrong layout for a C-element
per-lane block. The VPM rows are value-major, so the DMA must be vertical:

  Depth = C (VPM rows), Units = 16 (lanes), Horizontal = 0, stride = 0
  -> RAM [lane0 v0..vC-1][lane1 v0..vC-1]...  (lane-major, correct)

VDW vertical semantics: periphery/VPM.h:130-210 (VPWDMASetup doc block, worked
example). insertWriteDMASetup already does exactly this swap for the 64-bit path
(VPM.cpp:944-961). So the remaining work is:
1. make the stores group (findBaseAndOffset/workGroupConstantOffset currently does
   not yield a constant stride between the C stores), then
2. emit vertical for the grouped case (swap depth/units, horizontal=0, stride=0).

## Measured before this attempt (for reference)
- v16 + 12-QPU padding: N=128 2.3409 ms / 1.7918 GFLOP/s (best correct)
- CPU 4x A53 sgemm:      N=128 2.6963 GFLOP/s (cpu_gemm.c, -O3 -fopenmp)
- int-map 4x4 workaround: 3.7938 ms / 1.1056 GFLOP/s (correct, loses)
- nat 4x4, unmerged:     24.2 ms (m19) -> merging is worth ~10x, hence the gate work

## Rebuild / revert
  cmake --build ~/vc4cl/VC4C/build --parallel 2     # env-gated, safe to keep
  git -C ~/vc4cl/VC4C switch perf/gather-reads      # to leave the experiment
