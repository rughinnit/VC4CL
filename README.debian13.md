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
| vector 1024 | 0.51-0.54 |
| vector 65536 | 17.46-17.48 |
| vector 1048576 | 275.77-275.81 |
| matmul 32 | 3.56 |
| matmul 64 | 25.08 |

Through the ICD loader, without `LD_LIBRARY_PATH`: `clinfo` sees the platform, and
`probe_copy 65536` reports 0 mismatches with dma-heap versus exactly 12 with the
mailbox default (the hardware defect, reproduced).

### Performance versus the A53 CPU

gcc 14.2, `-O3`, best of 5:

| workload | GPU dma-heap | CPU -O3 | CPU no-vectorize |
|---|---|---|---|
| add 65536 | 17.4 ms | 0.203 ms | 0.563 ms |
| matmul 64 | 25.1 ms | 0.513 ms | 1.195 ms |

The GPU is 49-86 times slower on these kernels. The causes are structural: one
work-item per QPU instead of 16 lanes, 8 of 12 QPUs used, and the hardware mutex
around every global store. This is a correctness milestone, not a performance one.

### Testing notes

- The ICD file points at `/usr/local/lib/libVC4CL.so`, so installing is required
  for loader-based tools; testing an uninstalled build needs a temporary ICD
  directory plus `OCL_ICD_VENDORS`.
- `bench_vc4cl` links the library directly and uses `LD_LIBRARY_PATH`.
- `build/` is `BUILD_ICD=OFF`, `build-icd/` is `BUILD_ICD=ON`; loading the non-ICD
  library through the ICD loader crashes in `clGetPlatformIDs`.
- Previous installed libraries are kept in `/usr/local/lib/vc4cl-backup-*`.
