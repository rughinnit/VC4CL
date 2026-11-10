/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "DMAHeap.h"

#include "V3D.h"
#include "device_memory.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>

using namespace vc4cl;

namespace
{
    constexpr const char* DMA_HEAP_PATH = "/dev/dma_heap/linux,cma";
    constexpr const char* DEV_MEM_PATH = "/dev/mem";

    constexpr uint32_t PERIPHERAL_BUS_OFFSET = 0xC0000000;
    // Start of the firmware/GPU mailbox heap on this board (physical ~0x3B400000-0x3F000000), whose memory must not be
    // scanned or used.
    constexpr uint32_t SCAN_START = 0x10000000;
    constexpr uint32_t SCAN_END = 0x3B400000;
    constexpr uint32_t SCAN_CHUNK_SIZE = 1u << 20;

    constexpr const char* CMA_POOL_LOG_PATTERN = "created CMA memory pool at ";
    constexpr const char* CMA_RESERVED_LOG_PATTERN = "CMA: reserved ";

    struct CmaRegion
    {
        uint32_t base;
        uint32_t size;
    };

    /*
     * Registration of the dma-buf mapping for the cache hooks in device_memory.h. The pool is a process-wide
     * singleton, so file-scope state is sufficient.
     */
    int hookFd = -1;
    uintptr_t hookBegin = 0;
    uintptr_t hookEnd = 0;

    void registerDmaBuf(int fd, void* base, size_t size)
    {
        hookFd = fd;
        hookBegin = reinterpret_cast<uintptr_t>(base);
        hookEnd = hookBegin + size;
    }

    void unregisterDmaBuf()
    {
        hookFd = -1;
        hookBegin = 0;
        hookEnd = 0;
    }

    bool dmaBufSync(int fd, uint64_t flags, const char* operation) noexcept
    {
        struct dma_buf_sync sync;
        std::memset(&sync, 0, sizeof(sync));
        sync.flags = flags;
        if(ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0)
            return true;
        static std::atomic<bool> warned{false};
        if(!warned.exchange(true))
            std::fprintf(stderr, "[VC4CL] DMA_BUF_IOCTL_SYNC (%s) failed: %s\n", operation, std::strerror(errno));
        return false;
    }

    uint32_t getRequestedPoolSize()
    {
        constexpr uint32_t DEFAULT_POOL_MB = 64;
        uint32_t poolMB = DEFAULT_POOL_MB;
        if(const char* env = std::getenv("VC4CL_DMAHEAP_POOL_MB"))
        {
            char* end = nullptr;
            const unsigned long value = std::strtoul(env, &end, 0);
            if(end != env && value > 0 && value <= 4096)
                poolMB = static_cast<uint32_t>(value);
        }
        return poolMB * 1024u * 1024u;
    }

    uint32_t getTotalCMAMemory()
    {
        // the value does not change on a running Linux kernel, so only need to query it once
        static const uint32_t cmaMemory = []() {
            std::ifstream fis("/proc/meminfo");
            std::string line;
            while(std::getline(fis, line))
            {
                if(line.find("CmaTotal") != std::string::npos)
                {
                    // line looks like: CmaTotal:            xyz kB
                    auto start = line.find_first_of("0123456789");
                    if(start != std::string::npos)
                        return static_cast<uint32_t>(strtoul(line.data() + start, nullptr, 0)) * 1024 /* kB -> B*/;
                    break;
                }
            }
            return uint32_t{0};
        }();
        return cmaMemory;
    }

    bool parseHexValue(const std::string& log, size_t& pos, unsigned long& value)
    {
        if(pos + 2 > log.size() || log.compare(pos, 2, "0x") != 0)
            return false;
        pos += 2;
        size_t end = pos;
        while(end < log.size() && std::isxdigit(static_cast<unsigned char>(log[end])))
            ++end;
        if(end == pos)
            return false;
        value = std::strtoul(log.substr(pos, end - pos).c_str(), nullptr, 16);
        pos = end;
        return true;
    }

    /*
     * Reads the kernel log and extracts the reserved CMA regions. The dma-buf allocation lives inside the linux,cma
     * region, so a known base lets the physical scan be bounded to that region instead of all of RAM. The allocation
     * itself can still be at an offset inside the pool (other CMA users may allocate first), which is why the base is
     * only used as a search window and the signature is still searched for.
     */
    std::vector<CmaRegion> readCmaRegionsFromKernelLog()
    {
        std::vector<CmaRegion> regions;
#ifdef SYS_syslog
        std::string log;
        long bufferSize = syscall(SYS_syslog, 10 /* SYSLOG_ACTION_SIZE_BUFFER */, nullptr, 0);
        if(bufferSize <= 0)
            bufferSize = 1 << 20;
        std::vector<char> buffer(static_cast<size_t>(bufferSize) + 1);
        const long read =
            syscall(SYS_syslog, 3 /* SYSLOG_ACTION_READ_ALL */, buffer.data(), static_cast<int>(buffer.size()));
        if(read <= 0)
            return regions;
        log.assign(buffer.data(), static_cast<size_t>(read));

        size_t pos = 0;
        while((pos = log.find(CMA_POOL_LOG_PATTERN, pos)) != std::string::npos)
        {
            size_t current = pos + std::strlen(CMA_POOL_LOG_PATTERN);
            unsigned long base = 0;
            if(!parseHexValue(log, current, base))
            {
                ++pos;
                continue;
            }
            unsigned long size = 0;
            const size_t sizePos = log.find("size ", current);
            if(sizePos != std::string::npos && sizePos < current + 16)
            {
                size = std::strtoul(log.c_str() + sizePos + 5, nullptr, 10);
                const size_t unitPos = log.find("MiB", sizePos);
                const size_t gibPos = log.find("GiB", sizePos);
                if(unitPos != std::string::npos && unitPos < sizePos + 16)
                    size *= 1024u * 1024u;
                else if(gibPos != std::string::npos && gibPos < sizePos + 16)
                    size *= 1024u * 1024u * 1024u;
                else
                    size = 0;
            }
            if(size != 0 && base <= 0xFFFFFFFFull)
                regions.push_back(CmaRegion{static_cast<uint32_t>(base), static_cast<uint32_t>(size)});
            pos = current;
        }

        pos = 0;
        while((pos = log.find(CMA_RESERVED_LOG_PATTERN, pos)) != std::string::npos)
        {
            size_t current = pos + std::strlen(CMA_RESERVED_LOG_PATTERN);
            const unsigned long size = std::strtoul(log.c_str() + current, nullptr, 10);
            const size_t atPos = log.find(" at 0x", current);
            if(size == 0 || atPos == std::string::npos || atPos > current + 16)
            {
                ++pos;
                continue;
            }
            current = atPos + 4;
            unsigned long base = 0;
            if(!parseHexValue(log, current, base))
            {
                ++pos;
                continue;
            }
            const size_t unitPos = log.find("MiB", pos);
            if(unitPos != std::string::npos && unitPos < atPos)
            {
                if(base <= 0xFFFFFFFFull)
                    regions.push_back(
                        CmaRegion{static_cast<uint32_t>(base), static_cast<uint32_t>(size * 1024u * 1024u)});
            }
            pos = current;
        }
#else
        (void) regions;
#endif
        // deduplicate identical regions, keep the kernel log order
        regions.erase(std::unique(regions.begin(),
                          regions.end(),
                          [](const CmaRegion& left, const CmaRegion& right) {
                              return left.base == right.base && left.size == right.size;
                          }),
            regions.end());
        return regions;
    }
} // namespace

bool vc4cl::isDmaBufBackedMemory(const void* ptr, std::size_t bytes) noexcept
{
    const auto address = reinterpret_cast<uintptr_t>(ptr);
    return hookFd >= 0 && address >= hookBegin && address <= hookEnd && bytes <= hookEnd - address;
}

void vc4cl::beginDmaBufHostAccess(void* ptr, std::size_t bytes, bool write) noexcept
{
    if(!isDmaBufBackedMemory(ptr, bytes))
        return;
    // Invalidate the host caches before the CPU writes (and discards any stale line left over from an earlier GPU
    // write), so a partially written cache line cannot later be written back over GPU results.
    dmaBufSync(hookFd, DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ),
        write ? "host-write-start" : "host-read-start");
}

void vc4cl::endDmaBufHostAccess(void* ptr, std::size_t bytes, bool write) noexcept
{
    if(!isDmaBufBackedMemory(ptr, bytes))
        return;
    // Clean the host writes back to memory (or close the read access) before the buffer is handed back to the QPU.
    dmaBufSync(hookFd, DMA_BUF_SYNC_END | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ),
        write ? "host-write-end" : "host-read-end");
}

DMAHeap::DMAHeap() :
    fd(-1), hostBase(nullptr), poolSize(0), physicalBase(0), usedBlocks(), allocationLock(), signature()
{
    size_t size = getRequestedPoolSize();
    const uint32_t cmaTotal = getTotalCMAMemory();
    if(cmaTotal != 0 && size > cmaTotal)
    {
        std::cout << "[VC4CL] Requested DMA heap pool (" << (size / (1024 * 1024)) << " MiB) exceeds the CMA pool ("
                  << (cmaTotal / (1024 * 1024)) << " MiB), clamping" << std::endl;
        size = cmaTotal;
    }
    size &= ~static_cast<size_t>(PAGE_ALIGNMENT - 1);
    if(size < PAGE_ALIGNMENT)
        throw std::runtime_error("Invalid DMA heap pool size!");

    const int heapFd = open(DMA_HEAP_PATH, O_RDWR | O_CLOEXEC);
    if(heapFd < 0)
        throw std::system_error(errno, std::system_category(), std::string("Failed to open ") + DMA_HEAP_PATH);
    struct dma_heap_allocation_data allocation;
    std::memset(&allocation, 0, sizeof(allocation));
    allocation.len = size;
    allocation.fd_flags = static_cast<__u32>(O_RDWR | O_CLOEXEC);
    allocation.heap_flags = 0;
    if(ioctl(heapFd, DMA_HEAP_IOCTL_ALLOC, &allocation) != 0)
    {
        const int error = errno;
        close(heapFd);
        throw std::system_error(
            error, std::system_category(), std::string("Failed to allocate dma-buf from ") + DMA_HEAP_PATH);
    }
    fd = static_cast<int>(allocation.fd);
    close(heapFd);
    poolSize = size;

    hostBase = mmap(nullptr, poolSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(hostBase == MAP_FAILED)
    {
        hostBase = nullptr;
        const int error = errno;
        close(fd);
        fd = -1;
        throw std::system_error(error, std::system_category(), "Failed to map dma-buf pool");
    }
    usedBlocks.assign(poolSize / PAGE_ALIGNMENT, 0);

    fillSignature();
    // Write the signature at both ends of the pool and make sure it reaches memory, so the scan through /dev/mem can
    // find (and verify) it.
    std::memcpy(hostBase, signature, sizeof(signature));
    std::memcpy(static_cast<char*>(hostBase) + poolSize - sizeof(signature), signature, sizeof(signature));
    dmaBufSync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE, "pool signature");

    try
    {
        discoverPhysicalBase();
    }
    catch(...)
    {
        munmap(hostBase, poolSize);
        hostBase = nullptr;
        close(fd);
        fd = -1;
        throw;
    }

    DEBUG_LOG(DebugLevel::MEMORY,
        std::cout << "[VC4CL] Allocated DMA heap pool of " << (poolSize / (1024 * 1024)) << " MiB: physical address "
                  << std::hex << "0x" << physicalBase << ", bus address 0x"
                  << (physicalBase | PERIPHERAL_BUS_OFFSET) << ", host address " << hostBase << std::dec << std::endl)
    registerDmaBuf(fd, hostBase, poolSize);
}

DMAHeap::~DMAHeap()
{
    unregisterDmaBuf();
    if(hostBase != nullptr)
        munmap(hostBase, poolSize);
    if(fd >= 0)
        close(fd);
}

void DMAHeap::fillSignature()
{
    std::memset(signature, 0, sizeof(signature));
    static const char prefix[] = "VC4CL-DMAHEAP-POOL";
    std::memcpy(signature, prefix, sizeof(prefix) - 1);
    // a per-process cookie keeps the scan from matching the pool of another process
    uint64_t cookie = 0;
    const int randomFd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if(randomFd >= 0)
    {
        if(read(randomFd, &cookie, sizeof(cookie)) != static_cast<ssize_t>(sizeof(cookie)))
            cookie = 0;
        close(randomFd);
    }
    if(cookie == 0)
        cookie = (static_cast<uint64_t>(getpid()) << 32) ^ static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t size = poolSize;
    std::memcpy(signature + 24, &cookie, sizeof(cookie));
    std::memcpy(signature + 32, &size, sizeof(size));
}

bool DMAHeap::verifyPoolBase(int memFd, uint32_t base) const
{
    char check[sizeof(signature)];
    if(pread(memFd, check, sizeof(check), static_cast<off_t>(base)) != static_cast<ssize_t>(sizeof(check)))
        return false;
    if(std::memcmp(check, signature, sizeof(signature)) != 0)
        return false;
    if(pread(memFd, check, sizeof(check), static_cast<off_t>(base + poolSize - sizeof(signature))) !=
        static_cast<ssize_t>(sizeof(check)))
        return false;
    return std::memcmp(check, signature, sizeof(signature)) == 0;
}

uint32_t DMAHeap::scanForSignature(int memFd, uint32_t begin, uint32_t end) const
{
    std::unique_ptr<unsigned char[]> chunk(new unsigned char[SCAN_CHUNK_SIZE + sizeof(signature)]);
    for(uint64_t address = begin; address < end; address += SCAN_CHUNK_SIZE)
    {
        // read a bit past the chunk, so a candidate close to the chunk end is still fully covered
        const size_t length = static_cast<size_t>(
            std::min<uint64_t>(SCAN_CHUNK_SIZE + sizeof(signature), static_cast<uint64_t>(end) - address));
        const ssize_t readBytes = pread(memFd, chunk.get(), length, static_cast<off_t>(address));
        if(readBytes <= 0)
            continue;
        for(size_t i = 0; i + sizeof(signature) <= static_cast<size_t>(readBytes); i += PAGE_ALIGNMENT)
        {
            if(std::memcmp(chunk.get() + i, signature, sizeof(signature)) != 0)
                continue;
            const uint64_t candidate = address + i;
            if(candidate + poolSize > 0xFFFFFFFFull)
                continue;
            if(verifyPoolBase(memFd, static_cast<uint32_t>(candidate)))
                return static_cast<uint32_t>(candidate);
        }
    }
    return 0;
}

void DMAHeap::discoverPhysicalBase()
{
    const int memFd = open(DEV_MEM_PATH, O_RDONLY | O_SYNC);
    if(memFd < 0)
        throw std::system_error(
            errno, std::system_category(), std::string("Failed to open ") + DEV_MEM_PATH + " for DMA heap scan");

    const auto started = std::chrono::steady_clock::now();
    std::string method;
    const auto regions = readCmaRegionsFromKernelLog();
    for(const auto& region : regions)
    {
        if(region.size < poolSize)
            continue;
        const uint64_t end = static_cast<uint64_t>(region.base) + region.size;
        physicalBase = scanForSignature(
            memFd, region.base, static_cast<uint32_t>(std::min<uint64_t>(end, 0xFFFFFFFFull)));
        if(physicalBase != 0)
        {
            char regionDescription[64];
            std::snprintf(regionDescription, sizeof(regionDescription), "kernel log (CMA region 0x%08x)", region.base);
            method = regionDescription;
            break;
        }
    }
    if(physicalBase == 0)
    {
        // fallback: scan the plausible RAM window below the firmware/GPU mailbox heap
        physicalBase = scanForSignature(memFd, SCAN_START, SCAN_END);
        if(physicalBase != 0)
            method = "physical RAM scan";
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);

    if(physicalBase == 0)
    {
        close(memFd);
        throw std::runtime_error(
            "Failed to find the physical base of the DMA heap pool (no readable signature in physical RAM)");
    }

    const uint32_t busAddress = physicalBase | PERIPHERAL_BUS_OFFSET;
    if(V3D::busAddressToPhysicalAddress(busAddress) != physicalBase)
    {
        close(memFd);
        throw std::runtime_error("DMA heap pool bus address does not round-trip to its physical address!");
    }
    close(memFd);

    std::cout << "[VC4CL] DMA heap pool physical base: 0x" << std::hex << physicalBase << std::dec << " (bus 0x"
              << std::hex << busAddress << std::dec << "), found via " << method << " in "
              << (static_cast<double>(elapsed.count()) / 1000.0) << " ms" << std::endl;
}

size_t DMAHeap::findFreeBlocks(size_t numBlocks) const
{
    size_t run = 0;
    for(size_t block = 0; block < usedBlocks.size(); ++block)
    {
        if(usedBlocks[block] != 0)
        {
            run = 0;
            continue;
        }
        if(++run == numBlocks)
            return block + 1 - numBlocks;
    }
    return usedBlocks.size();
}

void DMAHeap::markBlocks(size_t firstBlock, size_t numBlocks, bool used)
{
    for(size_t block = firstBlock; block < firstBlock + numBlocks; ++block)
        usedBlocks[block] = used ? 1 : 0;
}

std::unique_ptr<DeviceBuffer> DMAHeap::allocateBuffer(
    const std::shared_ptr<SystemAccess>& system, unsigned sizeInBytes, const std::string& name, CacheType cacheType)
{
    // The dma-heap mapping is always cacheable; coherence is handled via DMA_BUF_IOCTL_SYNC, so the requested cache
    // type does not change the mapping.
    (void) cacheType;
    if(sizeInBytes == 0)
        return nullptr;
    const size_t numBlocks = (sizeInBytes + PAGE_ALIGNMENT - 1) / PAGE_ALIGNMENT;

    size_t firstBlock = 0;
    {
        std::lock_guard<std::mutex> guard(allocationLock);
        firstBlock = findFreeBlocks(numBlocks);
        if(firstBlock == usedBlocks.size())
        {
            DEBUG_LOG(DebugLevel::MEMORY,
                std::cout << "[VC4CL] DMA heap pool exhausted, cannot allocate " << sizeInBytes << " bytes for '"
                          << name << "'" << std::endl)
            return nullptr;
        }
        markBlocks(firstBlock, numBlocks, true);
    }

    const size_t offset = firstBlock * PAGE_ALIGNMENT;
    const uint32_t busAddress = (physicalBase + static_cast<uint32_t>(offset)) | PERIPHERAL_BUS_OFFSET;
    auto* hostPointer = static_cast<char*>(hostBase) + offset;
    DEBUG_LOG(DebugLevel::MEMORY,
        std::cout << "Allocated " << sizeInBytes << " bytes of DMA heap buffer '" << name << "': pool offset "
                  << offset << ", device address " << std::hex << "0x" << busAddress << ", host address " << std::dec
                  << static_cast<void*>(hostPointer) << std::endl)
    // The buffers returned by the other backends are not zeroed either, so do not zero here (the allocation is
    // reused from earlier CL buffers).
    return std::unique_ptr<DeviceBuffer>{new DeviceBuffer(
        system, static_cast<uint32_t>(offset + 1), DevicePointer{busAddress}, hostPointer, sizeInBytes)};
}

bool DMAHeap::deallocateBuffer(const DeviceBuffer* buffer)
{
    if(buffer == nullptr || buffer->hostPointer == nullptr)
        return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(buffer->hostPointer);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(hostBase);
    if(address < begin || address - begin >= poolSize || (address - begin) % PAGE_ALIGNMENT != 0)
    {
        DEBUG_LOG(DebugLevel::MEMORY, std::cout << "[VC4CL] Cannot deallocate buffer not owned by the DMA heap pool" << std::endl)
        return false;
    }
    const size_t firstBlock = (address - begin) / PAGE_ALIGNMENT;
    const size_t numBlocks = (buffer->size + PAGE_ALIGNMENT - 1) / PAGE_ALIGNMENT;
    if(firstBlock + numBlocks > usedBlocks.size())
        return false;
    {
        std::lock_guard<std::mutex> guard(allocationLock);
        markBlocks(firstBlock, numBlocks, false);
    }
    DEBUG_LOG(DebugLevel::MEMORY,
        std::cout << "Deallocated " << buffer->size << " bytes of DMA heap buffer: device address " << std::hex << "0x"
                  << static_cast<uint32_t>(buffer->qpuPointer) << ", host address " << buffer->hostPointer << std::dec
                  << std::endl)
    return true;
}

bool DMAHeap::isInsidePool(const DeviceBuffer* buffer) const
{
    if(buffer == nullptr || buffer->hostPointer == nullptr)
        return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(buffer->hostPointer);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(hostBase);
    return address >= begin && address <= begin + poolSize && buffer->size <= begin + poolSize - address;
}

bool DMAHeap::syncBuffer(const DeviceBuffer* buffer, uint64_t direction, bool start)
{
    if(!isInsidePool(buffer))
        return false;
    return dmaBufSync(fd, (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | direction, "buffer");
}

bool DMAHeap::prepareBufferForHostAccess(const DeviceBuffer* buffer, bool forWriting)
{
    return syncBuffer(buffer, forWriting ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ, true);
}

bool DMAHeap::finishBufferForHostAccess(const DeviceBuffer* buffer, bool forWriting)
{
    return syncBuffer(buffer, forWriting ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ, false);
}

bool DMAHeap::flushCPUCache(const std::vector<const DeviceBuffer*>& buffers)
{
    for(const auto* buffer : buffers)
    {
        if(buffer != nullptr && !isInsidePool(buffer))
            return false;
    }
    /*
     * DMA_BUF_IOCTL_SYNC applies to the whole dma-buf, it has no offset/length. Boxing individual buffers with
     * START|WRITE/END|WRITE here would invalidate the entire pool on the first START, dropping host writes that are
     * still dirty in the caches but not committed yet (e.g. the raw UNIFORM stores in executor.cpp). Therefore the
     * host-to-device hand-over first commits the dirty lines with END|WRITE, and only then runs the regular
     * START|WRITE/END|WRITE pair, which is safe once all dirty data is in memory.
     */
    bool status = dmaBufSync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE, "flush commit");
    status = dmaBufSync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE, "flush start") && status;
    status = dmaBufSync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE, "flush end") && status;
    return status;
}

bool DMAHeap::readValue(SystemQuery query, uint32_t& output) noexcept
{
    switch(query)
    {
    case SystemQuery::TOTAL_GPU_MEMORY_IN_BYTES:
        output = static_cast<uint32_t>(poolSize);
        return true;
    default:
        return false;
    }
}
