# VC4CL on Debian 13 arm64 (Raspberry Pi 3B)

Port notes for running VC4CL directly on the VideoCore IV V3D block of a
Raspberry Pi 3B under Debian 13 (trixie), without the legacy Raspberry Pi OS
userland stack and without VCSM.

Target:

- Raspberry Pi 3B, BCM2837, 1 GB RAM, VideoCore IV
- Debian 13 trixie arm64, kernel 6.18.x (`6.18.50+rpt-rpi-v8` at the time of the port)
- KMS `vc4` driver, `/dev/dri/card0`, `/dev/dri/renderD128`
- VC4CL at `/home/raspi/vc4cl/VC4CL`, VC4C at `/home/raspi/vc4cl/VC4C`

## What was patched and why

### 1. No `bcm_host_init` / VCHIQ at startup

The upstream library initializes the Raspberry Pi userland during
`SystemAccess` construction. That path calls `bcm_host_init`, which brings up
VCHIQ and the VideoCore GPU service. On this kernel the first hardware kernel
run ended in a D-state process:

```
do_exit -> __fput -> vchiq_release -> wait_for_completion
```

The process could not be killed and a reboot was required (shutdown was delayed
by the D-state task). Enumeration alone worked, which is why the hang appeared
only on the first real kernel submission.

The V3D constructor now deliberately does **not** call `bcm_host_init`, and the
peripheral base address is read from the device tree
(`/proc/device-tree/soc/ranges`) instead of
`bcm_host_get_peripheral_address()`. V3D therefore no longer depends on
libbcm_host for register access, and the VCHIQ descriptor is never opened by
that path. The VCHI executor is not initialized at all unless explicitly
selected.

Consequences:

- The VCHI GPU-service execution mode is effectively disabled; `VC4CL_NO_VCHI=1`
  is still recommended (and is set in the examples below) so the path is never
  selected.
- The mailbox path still requires the ARM mailbox driver (`/dev/vcio`), which is
  independent of VCHIQ.

### 2. Alignment-safe device copies

The V3D registers and the mailbox buffers are mapped as Device memory on arm64
(and the V3D register page is mapped through `/dev/mem`). Device memory only
permits naturally aligned accesses of the access size: a 16-byte or 8-byte
store to an address that is not 16/8-byte aligned is an alignment fault
(SIGBUS on Linux).

The original code used the default `memcpy`/`memset` implementations. glibc
uses 16-byte vector stores, so copying to/from device memory at an unaligned
address (which happens for kernel arguments and buffer offsets) could fault.

`src/hal/device_memory.h` now contains explicit copy helpers with this layout:

- unaligned head: byte accesses only
- aligned body: single 32-bit device accesses (volatile), with the 32-bit word
  assembled in normal host memory
- unaligned tail: byte accesses only

No 8-byte or 16-byte access is ever issued to device memory. This keeps the
register and buffer copy paths safe on arm64 regardless of the alignment of the
host pointer.

### 3. NO_VCSM

`libvcsm` disappeared with kernel 5.9+. CMake detects the missing library and
compiles `hal/no_vcsm_sources.list` with `-DNO_VCSM`. The VCSM/CMA memory modes
are therefore not available; the mailbox memory manager is used.

### 4. V3D power stays in system configuration

The library intentionally does not write the V3D power state. The platform
device path (`3fc00000.v3d`) is board/SoC specific and would be wrong on other
models. Instead, a small systemd unit keeps the power domain on:

```
/home/raspi/vc4cl/scripts/vc4cl-v3d-power.service
```

It is a oneshot unit ordered after `multi-user.target` that writes `on` to
`/sys/bus/platform/devices/3fc00000.v3d/power/control` and stays active
(`RemainAfterExit=yes`). This prevents the `vc4` driver from runtime-suspending
V3D between kernel runs, so register pokes do not land in a powered-down block.

## Build on aarch64 Debian 13

### Automated

```bash
cd /home/raspi/vc4cl
sudo ./scripts/setup-debian13.sh
```

The script is idempotent and documents every step. It installs the toolchain,
provides LLVM 14 (bookworm archive or `LLVM14_DEB_DIR`), installs the bookworm
Raspberry Pi userland packages with a narrow apt pin, creates the `/opt/vc/lib`
symlinks, builds VC4C and VC4CL, and installs/enables the power service.

Options:

- `--with-icd` builds with `BUILD_ICD=ON` and writes
  `/etc/OpenCL/vendors/vc4cl.icd`
- `--skip-build` only prepares packages, symlinks and the service
- `--no-service` skips installing the systemd unit

### Why LLVM 14

VC4C uses the LLVM 14 C++ API. LLVM 15+ removed/changed APIs that VC4C depends
on (including the switch to opaque pointers), so VC4C does not build against
the LLVM 19/20 that Debian 13 ships. LLVM 14 comes from Debian 12 bookworm; the
packages run on trixie because the required glibc/libstdc++ symbols are
available.

### Why the bookworm Raspberry Pi userland

VC4CL loads `libbcm_host.so`, `libvchiq_arm.so` and `libvcos.so`. Debian 13
dropped the `libraspberrypi0` / `libraspberrypi-dev` packages; Debian 12
bookworm arm64 still provides them. The interfaces are stable, and Debian
installs the real libraries into `/usr/lib/aarch64-linux-gnu`, so
`/opt/vc/lib` is recreated as a symlink directory for the traditional layout
and for CMake's library hints.

### Manual equivalent

```bash
cd /home/raspi/vc4cl
mkdir -p bin
ln -sfn /usr/bin/clang-14 bin/clang
ln -sfn /usr/bin/llvm-config-14 bin/llvm-config
export PATH=/home/raspi/vc4cl/bin:$PATH

cmake -S VC4C -B VC4C/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DLLVMLIB_FRONTEND=ON \
  -DCLANG_LIBRARY=OFF \
  -DVC4CL_STDLIB_DIR=/home/raspi/vc4cl/VC4CLStdLib/include \
  -DLLVM_DIR=/usr/lib/llvm-14/lib/cmake/llvm
cmake --build VC4C/build --parallel 2

cmake -S VC4CL -B VC4CL/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DINCLUDE_COMPILER=ON \
  -DBUILD_ICD=OFF \
  -DBUILD_TESTING=OFF \
  -DVC4C_HEADER_PATH=/home/raspi/vc4cl/VC4C/include/VC4C.h \
  -DVC4CC_LIBRARY=/home/raspi/vc4cl/VC4C/build/src/libVC4CC.so
cmake --build VC4CL/build --parallel 2

sudo install -m 0644 scripts/vc4cl-v3d-power.service \
  /etc/systemd/system/vc4cl-v3d-power.service
sudo systemctl daemon-reload
sudo systemctl enable --now vc4cl-v3d-power.service
```

Keep `-j2` on a 1 GB Pi 3B; higher parallelism can trip the OOM killer.

## Run requirements

1. Root for hardware execution. The default execution mode for euid 0 is
   `V3D_REGISTER_POKING`, which maps the V3D register page through `/dev/mem`
   and writes registers directly. Non-root users get no register access.

2. The V3D power service must be active:

   ```bash
   systemctl is-active vc4cl-v3d-power.service
   cat /sys/bus/platform/devices/3fc00000.v3d/power/control    # on
   cat /sys/bus/platform/devices/3fc00000.v3d/power/runtime_status  # active
   ```

   `power/control` must be `on`. With `auto`, V3D can be suspended between
   submissions.

3. Mailbox memory. Root defaults to `MAILBOX`; force it explicitly with
   `VC4CL_MEMORY_MAILBOX=1`. This allocates through the firmware mailbox
   property interface and is the only memory backend on this port.

4. NO_VCSM. The library is built without VCSM, so do not select
   `VC4CL_MEMORY_VCSM` or `VC4CL_MEMORY_CMA`. They are compiled out.

5. Disable the VCHI path explicitly:

   ```bash
   VC4CL_NO_VCHI=1
   ```

6. A practical hardware environment:

   ```bash
   LD_LIBRARY_PATH=/home/raspi/vc4cl/VC4CL/build/src:/home/raspi/vc4cl/VC4C/build/src
   VC4CL_NO_VCHI=1
   VC4CL_EXECUTE_REGISTER_POKING=1
   VC4CL_MEMORY_MAILBOX=1
   ```

## Validation

Always use line-buffered output and a timeout, and log to `/tmp`; a wedged
kernel submission must not leave an unattended process behind.

### CPU emulator (safe, no hardware)

```bash
cd /home/raspi/vc4cl
VC4CL_EMULATOR=1 \
LD_LIBRARY_PATH=/home/raspi/vc4cl/VC4CL/build/src:/home/raspi/vc4cl/VC4C/build/src \
timeout 30s stdbuf -oL ./test_vc4cl
```

Expected: platform/device enumeration, a successful program build, no
mismatches, `RESULT: PASS`.

### Hardware vector add (root)

```bash
cd /home/raspi/vc4cl
timeout 30s sudo -n env \
  LD_LIBRARY_PATH=/home/raspi/vc4cl/VC4CL/build/src:/home/raspi/vc4cl/VC4C/build/src \
  VC4CL_NO_VCHI=1 \
  VC4CL_EXECUTE_REGISTER_POKING=1 \
  VC4CL_MEMORY_MAILBOX=1 \
  stdbuf -oL ./test_vc4cl
```

Expected: `RESULT: PASS`, `mismatches=0/1024`.

If the process enters D state, do not retry. Reboot and inspect the last
`/tmp` log.

### ICD / clinfo (only if built with `--with-icd`)

```bash
cat /etc/OpenCL/vendors/vc4cl.icd
clinfo -l
clinfo | head -40
```

The current working build used `BUILD_ICD=OFF`, so no ICD file is registered
and `clinfo` will not see VC4CL. Direct linking against `libVC4CL.so` (as
`test_vc4cl` does) does not require the ICD.

### Service checks

```bash
systemctl status vc4cl-v3d-power.service
cat /sys/bus/platform/devices/3fc00000.v3d/power/control
cat /sys/bus/platform/devices/3fc00000.v3d/power/runtime_status
```

Reversibility test without a reboot:

```bash
sudo sh -c 'echo auto > /sys/bus/platform/devices/3fc00000.v3d/power/control'
sudo systemctl restart vc4cl-v3d-power.service
cat /sys/bus/platform/devices/3fc00000.v3d/power/control   # on again
```

## Performance

Measured on this Pi 3B with the hardware vector add, 1024 `float` elements:

- GPU execution: about 277 us per submission
- Wall clock for enqueue + `clFinish` in the harness: about 2.1 ms

The wall time is dominated by host/firmware round trips (mailbox allocation
setup, cache maintenance, completion polling), not by the arithmetic. Do not
read the wall number as compute throughput; at this size the kernel is far too
small to amortize the submission overhead.

## Known limits

- VCHI path disabled. The legacy GPU service execution mode is not usable on
  this port because VCHIQ initialization is what caused the D-state hang.
  `VC4CL_EXECUTE_VCHI=1` should not be used.

- Non-root execution is mailbox only. Without euid 0, only the mailbox
  execution mode is available, and that depends on `/dev/vcio` permissions.
  It is also constrained by the kernel mailbox cap of about 3 seconds: kernels
  that run longer are aborted or time out. Long kernels need the root
  register-poking path.

- Mailbox wedge. A stalled or timed-out mailbox transaction can leave the
  firmware mailbox unresponsive. There is no reliable reset from userspace; a
  reboot is usually required. Treat any timeout as terminal for that boot and
  inspect the log before retrying.

- No VCSM. There is no VCSM/CMA allocator. Only the mailbox memory backend is
  compiled in; CMA and VCSM modes are unavailable.

- GPU memory is the firmware heap. Allocations are carved from the firmware
  heap sized by `gpu_mem`, shared with firmware and any active display. On a
  1 GB board, large buffers fail quickly. VC4CL does not manage this split and
  this port does not change `config.txt`.

- No concurrent V3D user. Do not run a display client (X, Wayland, KMS/DRM
  compositor, `v3d`-using GL/Vulkan app) while VC4CL is poking V3D registers.
  Both sides program the same block and the result is at best corruption, at
  worst a GPU hang and a stuck process. Stop the display stack first.

- Security. The direct register path programs physical addresses and this port
  has no GPU MMU isolation for those buffers. Root-only is not a formality:
  an arbitrary kernel and its buffers can reach physical memory outside the
  intended allocation. Do not expose this to untrusted users or run untrusted
  OpenCL code.

## Files added by this workstream

- `scripts/vc4cl-v3d-power.service` - keeps V3D powered on after boot
- `scripts/setup-debian13.sh` - reproducible setup for this exact port
- `README.debian13.md` - this document

The library and build files are unchanged by this workstream; the patches in
`src/hal/` are owned by the source workstream.

## VPMBASE fix (2026-11-08)

The vc4 DRM driver writes `V3D_VPMBASE = 0` at probe and on every runtime-PM
resume, which reserves no VPM for user (SRQ) programs. VC4CL now re-arms
`VPMBASE` to 16 units (16 * 256 = 4096 bytes, the HW-2253 user window) at the
top of `V3D::executeQPU`, before every launch.

Validated on a clean boot with `bench_vc4cl all`:

- `VPMBASE & 0x1F == 16` after every launch
- `ERRSTAT & 0x30 == 0` after the full matrix (the VPM write/read range errors
  are gone; `0x1000` is the normal VCD-idle bit)
- No `VPM_ALLOCATING_WHILE_BUSY`

This fixes the VPM range errors but does **not** fix the deterministic
lane-scoped mismatch (global IDs congruent to 1 mod 4) seen for vector 65536,
vector 1048576 and matmul 64. That remaining defect points at per-lane
uniform/QPU code generation or cache handling, not VPM sizing.

## Resolution (2026-11-10): hardware defect and dma-heap backend

The lane-scoped mismatch described above was **not** code generation. This unit's
firmware/GPU carveout (top 76 MiB, physical ~0x3B400000-0x3F000000) has
stuck-at-1 memory cells.

Evidence gathered on this board:

- a buffer round trip with **no kernel** corrupts 3 words per 64 KB block at block
  offsets 0x3F24, 0x3F34, 0x3F44 (bits 19/21/23, masks 0x00A00000, 0x00A80000,
  0x00080000)
- a 4 MB scan shows 192 bad words, i.e. every 64 KB block of the window is affected
- the bad addresses are fixed in physical space and move with the allocation base
- a buffer that was never written already reads 1s there; writing zeros leaves them
  set and the values stay frozen for minutes (stuck-at-1), while writing ones looks
  clean
- kernel CMA memory at the same low-16 offsets is clean (0 mismatches in 4 MB and
  16 MB scans)

Consequently the mailbox backend cannot produce correct results on this unit: the
earlier figures (23/65536 for vector add, 110/4096 for matmul) are the hardware
defect, not VC4CL or VC4C.

### dma-heap memory backend (required on this board)

    sudo env VC4CL_MEMORY_DMAHEAP=1 VC4CL_EXECUTE_REGISTER_POKING=1 <application>

- `VC4CL_MEMORY_DMAHEAP=1` allocates from `/dev/dma_heap/linux,cma` instead of the
  firmware heap: one contiguous pool (`VC4CL_DMAHEAP_POOL_MB`, default 64 MiB,
  4 KB granularity), physical base discovered once from the kernel log CMA region
  and verified by a signature scan, GPU bus address `0xC0000000 | phys`.
  Requires root (`/dev/mem`, `/dev/dma_heap`).
- `VC4CL_EXECUTE_REGISTER_POKING=1` writes the SRQ registers directly. Do not use
  `VC4CL_EXECUTE_MAILBOX=1` on this kernel: it can deadlock the firmware mailbox
  (`rpi_firmware_property_list` stuck in D state).

### Cache-flush fix found along the way

`V3D::v3dBasePointer` was a plain `uint32_t*`, so the compiler merged the adjacent
`L2CACTL` and `SLCACTL` writes into one 64-bit store, which the 32-bit register
block drops. Neither cache was invalidated, and with a reused buffer pool every
launch executed with the previous launch's cached code and uniforms, one launch
behind. The pointer is now `volatile uint32_t*`, the two writes are separate
32-bit stores followed by `dsb sy`, and the SRQCS reset is verified (queue empty,
completed 0, no error) before the requests are queued.

### Validated matrix

dma-heap backend, register poking, `bench_vc4cl all`, two consecutive runs, 5/5
each, zero mismatches:

| case | kernel ms |
|---|---|
| vector 1024 | 0.26 (merged) |
| vector 65536 | 1.21-1.25 (merged) |
| vector 1048576 | 16.8 (merged) |
| matmul 32 | 0.39-0.40 (merged) |
| matmul 64 | 1.71 (merged) |

Through the ICD loader, without `LD_LIBRARY_PATH`: `clinfo` sees the platform, and
`probe_copy 65536` reports 0 mismatches with dma-heap versus exactly 12 with the
mailbox default (the hardware defect, reproduced).

### Performance versus the A53 CPU

gcc 14.2, `-O3`, best of 5:

| workload | GPU dma-heap | CPU -O3 | CPU no-vectorize |
|---|---|---|---|
| add 65536 | 1.21 ms | 0.203 ms | 0.563 ms |
| matmul 64 | 1.71 ms | 0.513 ms | 1.195 ms |

With work-item merging enabled the GPU is 1.1-6 times slower on these kernels; before
merging it was 22-86 times slower. What remains is memory bandwidth, the hardware mutex
on loads, and the fact that only 8 of the 12 QPUs are used for power-of-two global sizes.


### Store staging (2026-11-11)

Global DMA stores used to take the shared hardware mutex, because every QPU staged
through the same VPM scratch row. Each QPU now stages in its own row (one row per QPU,
offset `qpu_number * 64`, the same layout the register-spill path already used), and the
mutex is gone from the staging store.

Measured on this board:

| workload | before | after |
|---|---|---|
| vector add 65536 | 18.0 ms | 11.4 ms |
| vector add 1048576 | 276 ms | 176 ms |
| elementwise 1M | 274 ms | 181 ms |
| reduction 1M | 76 ms | 67 ms |
| GEMM 64/128/256 | 26 / 220 / 1695 ms | unchanged |
| 3x3 conv 64x64x32 | 3448 ms | 3425 ms |

These are the store-staging numbers only; work-item merging (below) supersedes them.
GEMM and convolution are load-bound and do not benefit from store staging alone. DMA reads keep the mutex: two
back-to-back staged reads from the same per-QPU row return wrong data (reproducible at
`-O0`, values depend on the row position), so the read path stays locked until a
two-row alternating scheme is validated. The remaining large lever is work-item
merging, which would fill all 16 lanes of a QPU instead of one.


### Work-item merging (2026-11-11)

Enabled with `VC4C_MERGE_WORK_ITEMS=16`. Sixteen work-items share a QPU, one per SIMD
lane, instead of one work-item per QPU. The runtime then chooses a work-group size that
is a multiple of 16 (128 for a 65536-element launch) and launches one work-group while
the kernel iterates the rest internally.

Measured, same board, auto local size:

| workload | before | after |
|---|---|---|
| vector 1024 | 0.44 ms | 0.26 ms |
| vector 65536 | 11.3 ms | 1.21-1.25 ms |
| vector 1048576 | 177 ms | 16.8 ms |
| matmul 32 / 64 | 3.55 / 25.1 ms | 0.39 / 1.71 ms |
| GEMM 64 / 128 / 256 | 25.3 / 217.9 / 1688.6 ms | 1.85 / 13.4 / 100.6 ms |
| elementwise 1M | 177 ms | 16.6 ms |
| 3x3 conv 64x64x32, clamping | 2992 ms | 174.6 ms |
| 3x3 conv 64x64x32, branching | 3448 ms | unchanged (rejected, factor 1) |

A kernel is merged only when all of the following hold; otherwise it falls back to one
work-item per QPU and logs the reason:

- no thread-control signal
- every conditional branch has a condition which is provably independent of the
  work-item ID, since a branch inside a merged kernel is executed once for the whole QPU
- every lane-dependent memory address is either read via TMU (one address per SIMD lane,
  a gather) or advances exactly one element per work-item (contiguous)
- every store address advances exactly one element per work-item and every value written
  per work-item is a single 32-bit element
- local work size is a multiple of 16

The address rule also closed a latent miscompile: kernels with stride-2 accesses, gathers,
reversed indices or per-work-item vector stores could previously be merged and produce
wrong values; they are now rejected and run correctly at factor 1.

Read-only memory at a lane-dependent address is lowered to a per-lane TMU address vector
(a gather), so strided and reversed reads merge too: stride-2 at 65536 elements takes
1.24 ms instead of 32.8 ms, a clamped 3x3 convolution 8.8 ms instead of 131.8 ms.

The lane-uniform branch rule allowed the loop conditions of matmul and of the
channel-looped convolution to merge. The condition is resolved from the branch flags and
its operands are walked back through their definitions, including the flags of
conditionally executed writers; a condition derived from a work-item ID, the element
number or a per-lane load is still rejected, so the branching dnnbench convolution stays
at factor 1.

Merged kernels without cross-work-item dependencies now run VC4C's work-group loop, so one
submission covers all work-groups; that is what took vector 65536 from 4.0 ms to 1.2 ms.

### Work-group barriers in merged kernels (2026-11-11)

`barrier()` no longer prevents merging. The 16 work-items on the SIMD lanes of one QPU
execute in lockstep, so the whole QPU arrives at the barrier once and the semaphore
rendezvous only needs one arrival per QPU. The barrier is therefore indexed by the QPU
slot within the work-group and the number of participants is
`ceil(local size / merge factor)`:

- the index is derived from the linear local work-item ID (`local ID >> log2(factor)`),
  which is `factor * slot + lane` for merged work-items, so all lanes yield the same slot
  and the slots are exactly `0..numParticipants-1`;
- it is deliberately not the hardware QPU number (`vc4cl_qpu_number()`), because the QPUs
  of a work-group are not numbered from zero: a work-group spread over 4 QPUs runs on the
  QPUs numbered 8..11, while the semaphore protocol requires the primary participant to be
  index 0;
- the work-group loop's own synchronization block is used for merged kernels too, it is
  lowered by the same code.

Two further fixes were needed for the `__local` buffers that reductions depend on:

- a work-group buffer is kept in RAM for merged kernels instead of being placed in a
  per-QPU VPM area, which cannot hold the elements of the whole work-group;
- the lane-invariant operands of an instruction which also has a per-lane operand are now
  replicated to all 16 lanes. Previously such a mix (e.g. a value read from a
  work-group-uniform `__local` address added to a per-work-item value) only reached the
  first lane, which made every reduction read the wrong values.

The strict fallback is kept: a kernel which reads the QPU number and has a barrier stays
unmerged (its barriers could be inside control flow which diverges between the QPUs), as
does a barrier kernel with a compile-time work-group size (`__attribute__((reqd_work_group_size))`),
because merged kernels with a known work-group size are broken independently of barriers
(no barrier at all: the same kernel with and without the attribute takes 2.1 ms vs 0.42 ms
and only the first SIMD lane reaches memory, the VPM staging area is allocated at rows
60..63 instead of 52..63). That is the pre-merging behaviour for such kernels and needs to
be fixed in the VPM allocation.

Measured, same board, `VC4C_MERGE_WORK_ITEMS=16`, auto local size unless noted:

| workload | factor 1 | factor 16 |
|---|---|---|
| reduction 1M, tree over `__local` (local 8 vs 128) | 193 ms | 23.3 ms |
| reduction 1M, read whole group array (local 8 vs 128) | 142 ms | 34.8 ms |
| dnnbench reduction 1M (local 8, per-lane tree) | 85.5 ms | rejected, stays at factor 1 |
| vector 65536 / 1048576 | 11.3 / 177 ms | 1.24 / 16.6 ms |
| matmul 32 / 64 | 3.55 / 25.1 ms | 0.42 / 2.15 ms |
| GEMM 128 | 217.9 ms | 13.4 ms |
| elementwise 1M | 177 ms | 16.8 ms |
| 3x3 conv 64x64x32, clamping | 2992 ms | 174.6 ms |

The dnnbench reduction kernel is rejected because its loop condition (`i < n` with a
per-work-item `i`) and its `if (lid < k)` steps are lane-dependent, not because of the
barrier; a tree reduction written with lane-parallel steps (masking by integer division in
a uniform loop) merges and is correct for every local size from 32 to 192 work-items.

matmul moved from 0.39/1.71 ms to 0.42/2.15 ms, the price of replicating the lane-invariant
operand of the mixed multiply-add in its inner loop; the other workloads are unchanged.

### Multi-dimensional work-group splitting (2026-11-12)

Local-size selection can now spread a merged work-group over dimension 1 as well as
dimension 0. For a tiled GEMM whose dimension 0 is the 16-column axis this matters: at
N=96 the runtime picks `16x12` and uses all 12 QPUs, running `gemm_vec4a` in 1.24 ms
versus 2.28 ms with the six-QPU `96x1` split (1.84x). At N=128/256 a 12-QPU split is
geometrically impossible for these tile shapes (the group would have to divide `(128,32)`
with a multiple-of-16 column block), so those stay at 8 QPUs with unchanged timings.

### Per-QPU VPM areas and the hardware QPU numbering (2026-11-12)

Merged kernels keep per-QPU VPM areas: one row per QPU for global store staging and, under
register pressure, four spill areas. These are addressed by the **hardware** QPU number,
and that number is not the launch index: a 12-QPU launch reports 11..0, an 8-QPU launch
11..4, a 4-QPU launch 11,10,9,8. The V3D fills the highest-numbered QPUs first.

The areas were previously sized by the number of instances in the work-group, so a kernel
compiled with `reqd_work_group_size(64)` reserved 4 rows (60..63) while the running QPUs
addressed rows 71..68; the QPU-side setup wraps at 6 bits and wrote rows 7..4 while the
DMA setup read 71..68, so stores landed from the wrong row. That is the previously recorded
"staging at 60..63 instead of 52..63" miscompile, and it also explains the wedge seen with
high-register-pressure kernels.

Per-QPU areas now reserve one row per hardware QPU (12) whatever the work-group size, a
compile-time check refuses any area that would leave the 64-row user window, and the
allocator prints the row ranges. `reqd_work_group_size` 16/32/64/128 are all correct now,
256 is rejected with a clear message, and the 8- and 16-accumulator GEMM variants run
instead of wedging: 23.2 ms and 22.5 ms at N=256 (1.45 and 1.49 GFLOP/s) versus 29.0 ms
for 4 accumulators.

### Redundant lane-invariant replication (2026-11-12)

Merging needs a lane-invariant scalar broadcast into all 16 lanes before a divergent
instruction can use it. `replicateLaneInvariantOperands()` was doing that once per *use
site*, so the lane-invariant divisor of the four run-time division expansions was copied
separately inside each of the ~63 clamp sequences: 236 `rep_all` in one conv kernel, 190
of them the identical `or rep_all|r5, rb3, rb3`.

That cost 8.8% on the clamped 3x3 convolution: 189.99 ms against 174.62 ms with replication
disabled, measured interleaved (7 pairs, clock fixed at 1.200 GHz, `throttled=0x0`). It was
not the VPM layout change, which produces an identical area map for this kernel.

Replications are now emitted once per basic block per source local, invalidated at block
boundaries and by any write to the source; special registers are never cached. Only values
that can be lane-partial are replicated at all - scalar memory reads, vector element
extraction, lane-dependent special registers and their transitive closure - because
everything else is already all-lane identical and replication is the identity. Register
allocation cannot do this itself: each replication is a distinct special-register
definition that overwrites the previous one.

The conv kernel returns to 10392 B / 1273 instructions / 15 `rep_all`, and the change also
recovered the merged matmul: 2.14 ms to 1.70 ms at N=64, with GEMM N=256 at 28.34 ms.

### Nondeterministic miscompile in scalar-local grouping (2026-11-13)

The convolution benchmark failed roughly one run in five with **every** output element
wrong. It was not a race between host and GPU: the failing process ran a different binary
(18304 B instead of 12024 B), and that binary was produced by about four compilations in
thirty. Replaying the saved binaries was deterministic - the small one passed every time,
the large one failed every time - so the run failure rate was the bad-compile rate.

`groupScalarLocals()` packs scalar locals into SIMD elements. The element extraction was
arithmetically correct, but the `xor.setf elem_num, k` mask that selects the element was
inserted directly after the local's last read, which could be **between a comparison and
its conditional consumers**. In the failing IR the flag register from `udiv.remainder` was
overwritten before `udiv.quotient`'s conditional select consumed it, so the selects read
`elem_num == 4`; the loop-bound comparison, the `continue` conditions and the accumulator
updates all followed it, leaving `sum == 0` for all 16384 work-group iterations. The store
addresses, output pointer and uniforms were correct throughout.

Separately, compilation was **nondeterministic**: pointer-ordered container iteration
changed LICM hoist order, which changed register pressure, which changed which fixup ran.
Under `setarch -R` the compiler was already bit-for-bit reproducible, so every variation
came from memory addresses.

Both are fixed:

- Iteration order in `groupScalarLocals()`, `reserveGroupSpace()`, `moveLoopInvariantCode()`,
  `reorderBasicBlocks()`, `GraphColoring` and the `LocalUtilization` tie-break now follows a
  stable key (`Local::id`, or a block's position in the method) instead of pointers.
- `flagsAreLive()` / `findFlagsDeadPosition()` move the element insertion behind readers of
  the flags, or decline to group the local when that is not possible.
- `verifyElementInsertions()` runs on every grouped compile and throws rather than emitting
  code if an insertion is not preceded by its own `elem_num == k` mask, if the mask element
  differs, if any instruction between the insertion and the next flag setter reads flags, if
  a write to a group vector is not an element insertion, or if a read is not an
  insert/copy/rotation/phi.

Evidence: `conv64.cl` compiled 50 times yields one size and one md5 (`186fa62272a9caa249
af527601ff4f97`, 12120 B); a 25-source sweep that previously differed on 20 of 25 now
differs on 0 of 25; `dnnbench conv` 20/20 pass; installed-path conv 10/10. No measured
regression: convclamp 174.6 ms, matmul 64 1.72 ms, GEMM N=256 28.8 ms.

Note for anyone measuring on this port: because the compiler used to be nondeterministic,
a single passing run was never evidence of correctness. Per-run checks are mandatory.

### Testing notes

- The ICD file points at `/usr/local/lib/libVC4CL.so`, so installing is required
  for loader-based tools; testing an uninstalled build needs a temporary ICD
  directory plus `OCL_ICD_VENDORS`.
- `bench_vc4cl` links the library directly and uses `LD_LIBRARY_PATH`.
- `build/` is `BUILD_ICD=OFF`, `build-icd/` is `BUILD_ICD=ON`; loading the non-ICD
  library through the ICD loader crashes in `clGetPlatformIDs`.
- Previous installed libraries are kept in `/usr/local/lib/vc4cl-backup-*`.

## Appendix: how the last 0.03 percent was found

This is the long form of the investigation, because the bug that remained after the port
worked was misleading in almost every way, and the method that finally found it is more
useful than the fix.

### 1. The symptom

After the port was functional, `bench_vc4cl` reported:

```
vector 65536 :  23 wrong of 65536     (0.035 %)
matmul 64    : 110 wrong of  4096     (2.7 %)
```

The failures were sparse, deterministic within a run, and always at local ids 1 and 5 with
local size 8. Since the device has 4 QPUs per slice, ids 1 and 5 are the second QPU of each
of the two slices in use. The wrong values looked like results computed from a neighbouring
work-item's data, and a store-only kernel (`c[i] = i`) also corrupted, so it was not the
load path.

That shape - sparse, lane-scoped, deterministic - looked exactly like a race in QPU code
generation or a cache-coherence bug. It was neither.

### 2. The falsification log

Every one of these was a reasonable hypothesis. Each was implemented, measured, and
rejected on evidence. Keeping the measurements is what eventually made the real cause
obvious.

**VPMBASE.** The vc4 driver zeroes `V3D_VPMBASE` on probe and on runtime-PM resume, so user
programs had no reserved VPM. Re-arming it to 16 units (4096 bytes, the HW-2253 window)
removed `ERRSTAT` bits 4 and 5 permanently. The mismatch count did not move. A real bug,
unrelated to the symptom.

**The work-group loop.** VC4C's `loop-work-groups` pass folds the whole kernel into one
launch that iterates work-groups internally. Disabling it should have isolated the loop, and
it did change behaviour - for the worse:

```
loop enabled  : 23 / 65536 wrong
loop disabled : 57344-65517 / 65536 wrong, plus hangs
```

Every launch after the first then ran with the previous launch's uniforms. That is a real
defect in the per-launch path, but it proved the loop was not the source of the sparse
errors.

**Uniform ordering and alignment.** A source audit showed the uniform pointer reset sits in
one basic block and the next iteration's uniform reads in another, with no dependency edge
between them; the Broadcom two-instruction rule was satisfied only by accident. Emitting two
`WAIT_UNIFORM` nops (69 -> 71 instructions, verified in the disassembly) changed nothing:
23 wrong, identical indices. Padding the per-QPU uniform blocks to a 64-byte boundary, and
separately aligning the uniform area base, also changed nothing.

**Optimization level.** `-O0`, `-O1` and `-O0 -cl-opt-disable` produced the same 23 wrong
results at the same indices. Register allocation and instruction scheduling were eliminated.

**SRQ protocol hygiene.** The runtime never cleared the request queue, never checked
`QPURQERR`, and could be satisfied by stale completion counts. All of it was fixed: queue
clear plus `dsb sy`, error check, "queue empty and no error" in the predicate, timeout
draining, and per-QPU uniform blocks addressed through recorded pointers instead of a
metadata-derived stride. The baseline stayed at 23 / 110.

**Local size.** Requesting local sizes 4, 2 and 1 produced half, three quarters and seven
eighths of the output missing, with writes running past the end of the buffer. That turned
out to be a separate contract bug: the runtime accepted a local size that did not match the
kernel's compile-time `reqd_work_group_size`. Fixed by a per-dimension check.

By this point every software candidate had been measured and rejected, and the numbers had
not moved by a single index.

### 3. The pivot: test the data path without the GPU

The step that broke the case was to stop testing kernels and test the buffer path alone:
create a buffer, fill it with a pattern where each 32-bit word encodes its index in both
halves, read it back. No kernel, no QPU, no TMU.

It corrupted 12 of 65536 words, identically on four consecutive readbacks.

```
i=10189  expected 0x674d27cd  got 0x67ed27cd
```

The low halfword was always correct and the upper halfword belonged to another index. That
also explained an earlier red herring: the corrupted values looked like results from "the
next work-group" because the mask, viewed as a float index offset, numerically resembled a
640-byte shift.

A store-only kernel, a load-only kernel, and a full add all failed at the same indices with
the same values - the kernels were reading and writing correct data that was already wrong.

### 4. Proving that the memory itself is defective

The fault turned out to be hardware, and it took five independent measurements to be sure:

1. **Periodicity.** Three words per 64 KB block, at block offsets 0x3F24, 0x3F34, 0x3F44,
   with masks 0x00A00000, 0x00A80000, 0x00080000 (data bits 19, 21, 23). A 4 MB scan found
   192 bad words: every 64 KB block of the window, not a local island.
2. **Address binding.** Shifting the allocation moved the bad buffer offsets by exactly the
   same amount, so the bad addresses are fixed in physical space. Three different bus aliases
   of the same physical memory failed identically.
3. **Set-only behaviour.** A buffer that was never written already read ones at those words.
   Writing zeros left them set, writing ones looked clean, and rewriting never cleared them.
   The values stayed frozen for minutes.
4. **Frequency.** The fault only appeared above the kernel's usable RAM, in the firmware/GPU
   carveout (top 76 MiB). Anonymous memory and the kernel CMA pool were clean at the same
   low-16 offsets.
5. **No allocator workaround.** Reserving 0 to 16 MB at startup never moved the failures,
   because the mailbox heap is anchored below the peripheral window and grows downward; the
   faulty block is always the first one handed out. Lowering `gpu_mem` would have given the
   bad range to Linux instead, which is worse.

Conclusion: this specific board has stuck-at-1 cells in its GPU memory. The earlier figures
(23/65536, 110/4096) are the hardware, not VC4CL or VC4C.

### 5. Working around it: a dma-heap backend

The 256 MB CMA pool at physical 0x1E000000 is in ordinary RAM and was clean in 4 MB and
16 MB scans, including the exact fault offsets. So the fix was to stop using the firmware
heap:

- allocate one contiguous pool from `/dev/dma_heap/linux,cma` (default 64 MiB)
- map it once for the host and sub-allocate 4 KB-aligned buffers from it
- derive the GPU bus address as `0xC0000000 | physical`

The interesting obstacle: `/proc/self/pagemap` returns zero for dma-buf mappings even as
root, because they are PFNMAP VMAs, so the physical base cannot be looked up. The base is
therefore discovered once by writing a signature at both ends of the pool and locating it
through a read-only `/dev/mem` scan, bounded by the CMA region reported in the kernel log
(11 ms in practice).

### 6. Two bugs the new backend exposed

**The QPU was never enabled.** The `Mailbox` constructor is what issues the firmware
`ENABLE_QPU` call. With the dma-heap backend no mailbox object was created, so requests were
queued and nothing ever executed. Fixed by constructing the mailbox for both backends while
still allocating memory from the pool.

**`DMA_BUF_IOCTL_SYNC` is a no-op on this kernel's CMA exporter.** After the sync, 4080 of
4096 bytes were still invisible through an uncached view of the same physical page, while
`dc civac` made all 4096 visible. The QPU was fetching stale code and uniforms. Buffers are
now explicitly cleaned and invalidated before each launch.

### 7. The best bug: a compiler-merged MMIO store

With both of those fixed, the full benchmark matrix still failed 2-4 of 5 cases per run, and
the signature was odd: `matmul 32` finished in 4 ms instead of 25 ms and its output buffer
was untouched, still holding the previous case's results. Delaying the completion check by
100 ms changed nothing, so it was not an early return. A dump of the kernel buffer matched
the host view. The data was correct in DRAM.

A two-kernel probe - two different programs, same code address - showed the truth:

```
phase 2, kernel A expected 0x1001: read 0x1000 0x1001 0x1002
phase 3, kernel A expected 0x2002: read 0x1000 0x1001 0x1002
```

The QPU was executing the previous program with the previous uniforms. Every launch ran one
launch behind.

The cause was in the disassembly of `executeQPU`:

```
ldr d31, [x0, #2856]    ; one 64-bit constant 0xFFFFFFFF00000004
str d31, [x1, #32]      ; L2CACTL and SLCACTL in a single 64-bit store
```

`v3dBasePointer` was a plain `uint32_t*`, so the compiler merged the two adjacent cache-flush
writes into one 64-bit store. The V3D register block is 32-bit; that store is dropped, and
neither the L2 nor the slice caches were ever invalidated. With a reused buffer pool, the
previous launch's code and uniforms stayed cached at exactly the reused addresses.

The fix: `volatile uint32_t*` for the register pointer, two separate 32-bit stores followed
by `dsb sy`, and an unconditional SRQ reset verification (queue empty, completed 0, no error)
with bounded retries before queueing.

### 8. Where it stands

With the dma-heap backend:

```
vector 1024      0 / 1024        0.5 ms
vector 65536     0 / 65536      17.5 ms
vector 1048576   0 / 1048576   276   ms
matmul 32        0 / 1024        3.6 ms
matmul 64        0 / 4096       25.1 ms
```

Two consecutive full matrices, 5/5 each, zero mismatches, repeated after the final commit.

Performance is a separate matter: the GPU is 49-86 times slower than the four Cortex-A53
cores on these kernels, because the port runs one work-item per QPU instead of 16 lanes,
uses 8 of 12 QPUs, and serialises every global store through the hardware mutex.

### 9. Lessons that transferred

- Sparse and deterministic does not imply "race in the GPU". Test the data path in isolation
  before believing any theory about the execution units.
- A failing test that changes when you change unrelated things (optimization level, loop
  structure) may be changing memory layout, not logic.
- When a value looks like another work-item's data, check whether it is a copy or a bit
  pattern. Halfword contamination and a neighbouring index can look identical in floating
  point.
- Compiler-merged stores to adjacent MMIO registers are real, silent, and catastrophic.
  `volatile` on the register pointer is not optional.
- Reserved hardware memory can be defective. A board that passes RAM tests can still have
  broken cells in the GPU carveout, and the only reliable test is a pattern round trip with
  no kernel involved.
