/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "V3D.h"

#include "hal.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>

using namespace vc4cl;

static const uint32_t V3D_BASE_OFFSET = 0x00c00000;

// The offsets are byte-offsets, but we need to convert them to 32-bit word offsets
static constexpr uint32_t V3D_IDENT0 = 0x0000 / sizeof(uint32_t);
static constexpr uint32_t V3D_IDENT1 = 0x0004 / sizeof(uint32_t);
static constexpr uint32_t V3D_L2CACTL = 0x00020 / sizeof(uint32_t);
static constexpr uint32_t V3D_SLCACTL = 0x00024 / sizeof(uint32_t);
// constexpr uint32_t V3D_IDENT2 = 0x0008 / sizeof(uint32_t);
static constexpr uint32_t V3D_QPU_RESERVATIONS0 = 0x0410 / sizeof(uint32_t);
// constexpr uint32_t V3D_QPU_RESERVATIONS1 = 0x0414 / sizeof(uint32_t);
static constexpr uint32_t V3D_SRQPC = 0x00430 / sizeof(uint32_t);
static constexpr uint32_t V3D_SRQUA = 0x00434 / sizeof(uint32_t);
static constexpr uint32_t V3D_SRQCS = 0x0043c / sizeof(uint32_t);
static constexpr uint32_t V3D_VPMBASE = 0x00504 / sizeof(uint32_t);
// The vc4 DRM driver writes V3D_VPMBASE = 0 on probe and on every runtime-PM resume, so the reservation has to be
// re-armed before user programs are queued. Bits 4:0 hold the VPM memory reserved for user programs in 256-byte
// units. HW-2253 limits user programs to the first 64 VPM rows, i.e. 64 * 16 * sizeof(uint32_t) = 4 KB = 16 units.
static constexpr uint32_t VPM_USER_WINDOW_SIZE = 64 * 16 * sizeof(uint32_t);
static constexpr uint32_t VPM_RESERVATION_UNIT = 256;
static constexpr uint32_t V3D_COUNTER_CLEAR = 0x0670 / sizeof(uint32_t);
static constexpr uint32_t V3D_COUNTER_ENABLE = 0x0674 / sizeof(uint32_t);
static constexpr uint32_t V3D_COUNTER_VALUE_BASE = 0x0680 / sizeof(uint32_t);
static constexpr uint32_t V3D_COUNTER_MAPPING_BASE = 0x0684 / sizeof(uint32_t);
static constexpr uint32_t V3D_ERRORS = 0x0F20 / sizeof(uint32_t);
// offset between two counter-enable, two counter-value or two counter-mapping registers
static constexpr uint32_t V3D_COUNTER_INCREMENT = 0x0008 / sizeof(uint32_t);
static constexpr uint32_t V3D_LENGTH = ((V3D_ERRORS - V3D_IDENT0) + 16) * sizeof(uint32_t);

// peripheral base address of the BCM2835/BCM2836/BCM2837 SoCs used by the Raspberry Pi 1, 2 and 3
static constexpr uint32_t DEFAULT_PERIPHERAL_BASE = 0x3F000000;

/**
 * Determines the base address of the SoC peripherals without using libbcm_host.
 *
 * The "ranges" property of the "soc" device-tree node contains a list of (child address, parent address, size) triples
 * of big-endian 32-bit cells. The first triple maps the peripherals into the ARM physical address space, so the second
 * cell of the property is the peripheral base address. bcm_host_get_peripheral_address() parses the same property.
 * If the property cannot be read (or contains implausible data), fall back to the address used by BCM283x.
 */
static uint32_t getPeripheralBaseAddress()
{
    static const char* rangesPath = "/proc/device-tree/soc/ranges";
    const int fd = open(rangesPath, O_RDONLY);
    if(fd < 0)
    {
        DEBUG_LOG(DebugLevel::SYSCALL,
            std::cout << "[VC4CL] Could not open " << rangesPath << ", assuming peripheral base 0x" << std::hex
                      << DEFAULT_PERIPHERAL_BASE << std::dec << std::endl)
        return DEFAULT_PERIPHERAL_BASE;
    }

    // only the first triple is of interest: child address, parent (peripheral) address, size
    uint8_t cells[3 * sizeof(uint32_t)] = {};
    const auto bytesRead = read(fd, cells, sizeof(cells));
    close(fd);
    if(bytesRead != static_cast<ssize_t>(sizeof(cells)))
    {
        DEBUG_LOG(DebugLevel::SYSCALL,
            std::cout << "[VC4CL] Could not read " << rangesPath << ", assuming peripheral base 0x" << std::hex
                      << DEFAULT_PERIPHERAL_BASE << std::dec << std::endl)
        return DEFAULT_PERIPHERAL_BASE;
    }

    // the cells are stored in big-endian byte order, the peripheral base is the second cell
    const uint32_t peripheralBase = (static_cast<uint32_t>(cells[4]) << 24) |
        (static_cast<uint32_t>(cells[5]) << 16) | (static_cast<uint32_t>(cells[6]) << 8) |
        static_cast<uint32_t>(cells[7]);
    if(peripheralBase == 0 || peripheralBase == 0xFFFFFFFF)
    {
        DEBUG_LOG(DebugLevel::SYSCALL,
            std::cout << "[VC4CL] Implausible peripheral base 0x" << std::hex << peripheralBase
                      << " from device-tree, assuming 0x" << DEFAULT_PERIPHERAL_BASE << std::dec << std::endl)
        return DEFAULT_PERIPHERAL_BASE;
    }

    DEBUG_LOG(DebugLevel::SYSCALL,
        std::cout << "[VC4CL] Peripheral base from device-tree: 0x" << std::hex << peripheralBase << std::dec
                  << std::endl)
    return peripheralBase;
}

V3D::V3D()
{
    // NOTE: bcm_host_init() is intentionally not called here. The arm64 build of libbcm_host.so implements it by
    // opening /dev/vchiq (vchiq_initialise() + vchi_connect()), but its counterpart bcm_host_deinit() is a no-op in
    // that library, so the VCHIQ file descriptor is never closed. On process exit the kernel then blocks forever in
    // vchiq_release() -> wait_for_completion(), leaving the process unkillable in D state.
    // The peripheral base address is read from the device-tree instead of via bcm_host_get_peripheral_address(), so
    // this no longer depends on libbcm_host at all.
    v3dBasePointer = static_cast<uint32_t*>(
        mapmem(busAddressToPhysicalAddress(getPeripheralBaseAddress() + V3D_BASE_OFFSET), V3D_LENGTH));
    DEBUG_LOG(DebugLevel::SYSCALL, std::cout << "[VC4CL] V3D base: " << v3dBasePointer << std::endl)
}

V3D::~V3D()
{
    // No bcm_host_deinit() here, see the constructor comment above: it would not close the VCHIQ descriptor anyway.
    unmapmem(v3dBasePointer, V3D_LENGTH);
}

uint32_t V3D::getSystemInfo(const SystemInfo key) const
{
    switch(key)
    {
    case SystemInfo::VPM_MEMORY_SIZE:
        if((v3dBasePointer[V3D_IDENT1] >> 28) == 0)
            // 0 => 16K
            return 16 * 1024;
        return (v3dBasePointer[V3D_IDENT1] >> 28) * 1024;
    case SystemInfo::VPM_USER_MEMORY_SIZE:
        //"Contains the amount of VPM memory reserved for all user programs, in multiples of 256 bytes (4x 16-way 32-bit
        // vectors)."
        return (v3dBasePointer[V3D_VPMBASE] & 0x1F) * 256;
    case SystemInfo::SEMAPHORES_COUNT:
        // FIXME (similar to error with QPU count), this returns 0 but should return 16
        // return (v3dBasePointer[V3D_IDENT1) >> 16) & 0xFF;
        return 16;
    case SystemInfo::SLICE_TMU_COUNT:
        return (v3dBasePointer[V3D_IDENT1] >> 12) & 0xF;
    case SystemInfo::SLICE_QPU_COUNT:
        return (v3dBasePointer[V3D_IDENT1] >> 8) & 0xF;
    case SystemInfo::SLICES_COUNT:
        return (v3dBasePointer[V3D_IDENT1] >> 4) & 0xF;
    case SystemInfo::QPU_COUNT:
        return 12;
        // FIXME somehow from time to time (e.g. every first call after reboot??) this returns 196, but why??
        // return ((*(uint32_t*)(mmap_ptr + V3D_IDENT1) >> 4) & 0xF) * ((*(uint32_t*)(mmap_ptr + V3D_IDENT1) >> 8) &
        // 0xF);
    case SystemInfo::HDR_SUPPORT:
        return (v3dBasePointer[V3D_IDENT1] >> 24) & 0x1;
    case SystemInfo::V3D_REVISION:
        return v3dBasePointer[V3D_IDENT1] & 0xF;
    case SystemInfo::USER_PROGRAMS_COMPLETED_COUNT:
        return (v3dBasePointer[V3D_SRQCS] >> 16) & 0xFF;
    case SystemInfo::USER_REQUESTS_COUNT:
        return (v3dBasePointer[V3D_SRQCS] >> 8) & 0xFF;
    case SystemInfo::PROGRAM_QUEUE_FULL:
        return (v3dBasePointer[V3D_SRQCS] >> 7) & 0x1;
    case SystemInfo::PROGRAM_QUEUE_LENGTH:
        return v3dBasePointer[V3D_SRQCS] & 0x3F;
    }
    return 0;
}

bool V3D::setCounter(uint8_t counterIndex, const CounterType type)
{
    if(static_cast<uint8_t>(counterIndex) > 15)
        return false;
    if(static_cast<uint8_t>(counterIndex) > 29)
        return false;

    // 1. enable counter
    // http://maazl.de/project/vc4asm/doc/VideoCoreIV-addendum.html states (section 10):
    //"You need to set bit 31 (allegedly reserved) to enable performance counters at all."
    v3dBasePointer[V3D_COUNTER_ENABLE] |= (1u << 31) | (1u << counterIndex);
    // 2. set mapping
    v3dBasePointer[V3D_COUNTER_MAPPING_BASE + counterIndex * V3D_COUNTER_INCREMENT] =
        static_cast<uint32_t>(type) & 0x1F;
    // 3. reset counter
    // TODO difference between reset here and reset in resetCounterValue??
    v3dBasePointer[V3D_COUNTER_VALUE_BASE + counterIndex * V3D_COUNTER_INCREMENT] = 0;

    return true;
}

void V3D::resetCounterValue(uint8_t counterIndex)
{
    v3dBasePointer[V3D_COUNTER_CLEAR] = (1 << counterIndex);
}

// https://github.com/jonasarrow/vc4top: "Detected by observation, if the GPU is gated, deadbeef is returned"
// Confirmed this detection
static const uint32_t POWEROFF_VALUE = 0xdeadbeef;

int64_t V3D::getCounter(uint8_t counterIndex) const
{
    uint32_t val = static_cast<uint32_t>(v3dBasePointer[V3D_COUNTER_VALUE_BASE + counterIndex * V3D_COUNTER_INCREMENT]);
    if(val == POWEROFF_VALUE)
        return static_cast<int64_t>(-1);
    return static_cast<int64_t>(val);
}

void V3D::disableCounter(uint8_t counterIndex)
{
    resetCounterValue(counterIndex);
    // TODO correct?? the or?
    v3dBasePointer[V3D_COUNTER_ENABLE] |= 0xFFFF ^ (1 << counterIndex);
}

bool V3D::setReservation(const uint8_t qpu, const QPUReservation val)
{
    if(qpu > 16)
        return false;
    // 8 reservation settings are in one 32-bit register (4 bit per setting)
    uint32_t registerOffset = qpu / 8;
    uint32_t bitOffset = (qpu % 8) * 4;
    uint32_t writeVal = (static_cast<uint8_t>(val) & 0xFu) << bitOffset;
    // clear old values
    v3dBasePointer[V3D_QPU_RESERVATIONS0 + registerOffset] &= ~(0xFu << bitOffset);
    // set new values
    v3dBasePointer[V3D_QPU_RESERVATIONS0 + registerOffset] |= writeVal;

    return true;
}

QPUReservation V3D::getReservation(const uint8_t qpu) const
{
    uint32_t registerOffset = qpu / 8;
    uint32_t bitOffset = (qpu % 8) * 4;
    uint32_t rawValue = v3dBasePointer[V3D_QPU_RESERVATIONS0 + registerOffset] >> bitOffset;
    return static_cast<QPUReservation>(rawValue & 0xF);
}

bool V3D::hasError(const ErrorType type) const
{
    // read bit
    uint32_t val = v3dBasePointer[V3D_ERRORS] >> static_cast<uint8_t>(type);
    // reset bit
    v3dBasePointer[V3D_ERRORS] = 1 << static_cast<uint8_t>(type);
    return val == 1;
}

/**
 * Completes all outstanding writes to the memory-mapped V3D registers.
 *
 * The registers are mapped as normal non-cacheable memory (see mapmem()), so a register read can be observed while
 * earlier writes are still in flight. This is measurable: reading SRQCS directly after queueing the user programs can
 * return the value of the preceding reset write instead of the queued state. Waiting for the writes to complete before
 * looking at the queue state is therefore required for the checks below to be meaningful.
 */
static void finishRegisterWrites()
{
#if defined(__aarch64__) || defined(__arm__)
    asm volatile("dsb sy" : : : "memory");
#else
    __sync_synchronize();
#endif
}

ExecutionHandle V3D::executeQPU(
    unsigned numQPUs, std::pair<uint32_t*, unsigned> addressPairs, bool flushBuffer, std::chrono::milliseconds timeout)
{
    // The vc4 driver zeroes V3D_VPMBASE on probe and on every runtime-PM resume, while VC4CL runs its kernels as SRQ
    // user programs that rely on this reservation. Re-arm it before queueing. HW-2253 caps the window at 4 KB (16 units).
    if((v3dBasePointer[V3D_VPMBASE] & 0x1F) != (VPM_USER_WINDOW_SIZE / VPM_RESERVATION_UNIT))
        v3dBasePointer[V3D_VPMBASE] = VPM_USER_WINDOW_SIZE / VPM_RESERVATION_UNIT;

    // see
    // https://github.com/raspberrypi/userland/blob/master/host_applications/linux/apps/hello_pi/hello_fft/gpu_fft_base.c,
    // function gpu_fft_base_exec_direct
    // TODO interrupts?? not in Broadcom spec
    // https://vc4-notes.tumblr.com/post/125039428234/v3d-registers-not-on-videocore-iv-3d-architecture
    // see errata: https://elinux.org/VideoCore_IV_3D_Architecture_Reference_Guide_errata

    // clear cache (if set)
    // FIXME when the buffer-flush is disabled (for any consecutive execution), the updated UNIFORM-values are not used,
    // but the old ones!
    //-> which results in incorrect executions (except the first one)
    // XXX can this be re-enabled, if we allocate the host-pointer (Mailbox.cpp) with direct access-mode?
    // if(flushBuffer)
    {
        // clear L2 cache
        v3dBasePointer[V3D_L2CACTL] = 1 << 2;
        // clear uniforms and instructions caches (TMU too)
        v3dBasePointer[V3D_SLCACTL] = 0xFFFFFFFF;
    }

    // reset the user program state: clear the request queue (QPURQL, bit 0), the queue error flag (QPURQERR,
    // bit 7), the request counter (QPURQCM, bit 8) and the completed counter (QPURQCC, bit 16). Clearing the queue
    // is required, otherwise requests left over from a previous (e.g. timed out) launch would be executed as well.
    v3dBasePointer[V3D_SRQCS] = (1 << 0) | (1 << 7) | (1 << 8) | (1 << 16);

    // write uniforms and instructions addresses for all QPUs
    uint32_t* addressBase = addressPairs.first;
    for(unsigned i = 0; i < numQPUs; ++i)
    {
        v3dBasePointer[V3D_SRQUA] = addressBase[0];
        v3dBasePointer[V3D_SRQPC] = addressBase[1];
        addressBase += 2;
    }

    finishRegisterWrites();

    // QPURQERR is set when a request could not be queued (e.g. the request queue was full). Such a program will
    // never run, so report the failure immediately instead of spinning until the timeout expires.
    const uint32_t srqcsQueued = v3dBasePointer[V3D_SRQCS];
    if((srqcsQueued & (1 << 7)) != 0)
    {
        std::cout << "[VC4CL] V3D queue error after queueing " << numQPUs << " user programs: SRQCS = 0x"
                  << std::hex << srqcsQueued << std::dec << " (queue length " << (srqcsQueued & 0x3F)
                  << ", queue error " << ((srqcsQueued >> 7) & 1) << ")" << std::endl;
        return ExecutionHandle{false};
    }

    const auto start = std::chrono::high_resolution_clock::now();
    auto basePointer = v3dBasePointer;
    auto checkFunc = [basePointer, numQPUs, start, timeout]() -> bool {
        // wait for completion
        while(true)
        {
            const uint32_t srqcs = basePointer[V3D_SRQCS];
            // the launch only completed if all QPUs are done, the request queue is empty and no queue error is
            // signaled. The completed counter alone is not sufficient: it may still be rising while requests are
            // queued and it can already contain the results of a previous launch.
            if(((srqcs >> 16) & 0xFF) == numQPUs && (srqcs & 0x3F) == 0 && (srqcs & 0x80) == 0)
                return true;
            if(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::high_resolution_clock::now() - start) > timeout)
            {
                // clear the request queue, so the timed out (possibly stuck) launch cannot poison the next one
                basePointer[V3D_SRQCS] = 1 << 0;
                std::cout << "[VC4CL] V3D execution timed out after " << timeout.count() << " ms: SRQCS = 0x"
                          << std::hex << srqcs << std::dec << " (completed " << ((srqcs >> 16) & 0xFF) << " of "
                          << numQPUs << ", queue length " << (srqcs & 0x3F) << ", queue error "
                          << ((srqcs >> 7) & 1) << "), cleared request queue" << std::endl;
                return false;
            }
            // TODO sleep some time?? so CPU is not fully used for waiting
            // e.g. sleep for the theoretical execution time of the kernel (e.g. #instructions / QPU clock) and then
            // begin active waiting
        }
    };
    return ExecutionHandle{checkFunc};
}

bool V3D::readValue(SystemQuery query, uint32_t& output) noexcept
{
    switch(query)
    {
    case SystemQuery::CURRENT_QPU_CLOCK_RATE_IN_HZ:
    case SystemQuery::MAXIMUM_QPU_CLOCK_RATE_IN_HZ:
    case SystemQuery::CURRENT_ARM_CLOCK_RATE_IN_HZ:
    case SystemQuery::MAXIMUM_ARM_CLOCK_RATE_IN_HZ:
    case SystemQuery::QPU_TEMPERATURE_IN_MILLI_DEGREES:
    case SystemQuery::TOTAL_ARM_MEMORY_IN_BYTES:
    case SystemQuery::TOTAL_GPU_MEMORY_IN_BYTES:
        return false;
    case SystemQuery::NUM_QPUS:
        output = getSystemInfo(SystemInfo::QPU_COUNT);
        return true;
    case SystemQuery::TOTAL_VPM_MEMORY_IN_BYTES:
        // Only the first 64 VPM rows are usable by user programs (HW-2253), so report the user-visible window
        output = std::min(getSystemInfo(SystemInfo::VPM_MEMORY_SIZE), VPM_USER_WINDOW_SIZE);
        return true;
    }
    return false;
}

uint32_t V3D::busAddressToPhysicalAddress(uint32_t busAddress)
{
    return busAddress & ~0xC0000000;
}

MemoryMapping vc4cl::getMemoryMapping(uintptr_t base, size_t size)
{
    const auto offset = base % V3D::MEMORY_PAGE_SIZE;
    return MemoryMapping{base - offset, size + offset, offset};
}

void* vc4cl::mapmem(unsigned base, unsigned size)
{
    // TODO this is called once to initialize V3D and every time a buffer is mapped.
    // Can't we cache the memory fd?
    int mem_fd;
    // map the pages containing the requested base and return the shifted pointer, so that both the mapping length
    // and the returned pointer cover the requested range
    const auto mapping = getMemoryMapping(base, size);
    /* open /dev/mem */
    if((mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0)
    {
        std::cout << "[VC4CL] can't open /dev/mem" << std::endl;
        std::cout << "[VC4CL] This program should be run as root. Try prefixing command with: sudo" << std::endl;
        throw std::system_error(errno, std::system_category(), "Failed to open /dev/mem");
    }
    void* mem = mmap(nullptr, mapping.mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED /*|MAP_FIXED*/, mem_fd,
        static_cast<off_t>(mapping.alignedBase));
    DEBUG_LOG(DebugLevel::SYSCALL, printf("[VC4CL] base=0x%llx, mappedSize=0x%zx, offset=0x%zx, mem=%p\n",
                           static_cast<unsigned long long>(mapping.alignedBase), mapping.mappedSize,
                           mapping.offset, mem))
    if(mem == MAP_FAILED)
    {
        std::cout << "[VC4CL] mmap error " << mem << std::endl;
        perror("[VC4CL] Error in mapmem");
        close(mem_fd);
        throw std::system_error(errno, std::system_category(), "Error in mapmem");
    }
    close(mem_fd);
    return reinterpret_cast<char*>(reinterpret_cast<uintptr_t>(mem) + mapping.offset);
}

void vc4cl::unmapmem(void* addr, unsigned size)
{
    // reconstruct the page-aligned base and the full mapping length from the shifted pointer, since munmap() must be
    // called on the base address returned by mmap() with the length passed to mmap()
    const auto mapping = getMemoryMapping(reinterpret_cast<uintptr_t>(addr), size);
    int s = munmap(reinterpret_cast<void*>(mapping.alignedBase), mapping.mappedSize);
    if(s != 0)
    {
        std::cout << "munmap error " << s << std::endl;
        perror("Error in unmapmem");
        throw std::system_error(errno, std::system_category(), "Error in unmapmem");
    }
}
