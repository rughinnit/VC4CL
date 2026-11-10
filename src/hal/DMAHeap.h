/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */
#ifndef VC4CL_DMAHEAP_H
#define VC4CL_DMAHEAP_H

#include "hal.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace vc4cl
{
    /**
     * Memory backend allocating one dma-buf from /dev/dma_heap/linux,cma and handing out aligned sub-ranges of it.
     *
     * Rationale: the firmware/GPU mailbox heap of this board has stuck-at-1 bits in three words per 64 KB block. The
     * kernel CMA pool is clean at the same offsets, and /dev/dma_heap/linux,cma provides contiguous, cacheable
     * allocations from it. Because /proc/self/pagemap reports 0 for dma-buf VMAs, the physical address of the pool
     * cannot be obtained from the page tables. It is therefore found once at startup by writing a signature into the
     * pool and searching physical RAM through /dev/mem, optionally bounded to the CMA region announced in the kernel
     * log, and verified against a second marker at the end of the pool.
     *
     * The host mapping is cacheable. The CMA dma-buf exporter on this kernel does not implement CPU cache maintenance
     * for DMA_BUF_IOCTL_SYNC (verified: END|WRITE leaves dirty host lines in the cache), so cache coherence is
     * enforced with explicit ARM clean+invalidate over the accessed ranges: before every QPU launch (so the QPU sees
     * all host writes) and around host accesses (see flushCPUCache, prepareBufferForHostAccess and the hooks in
     * device_memory.h). No dirty host line can survive a launch, which is what makes invalidating before a host read
     * safe.
     */
    class DMAHeap
    {
    public:
        DMAHeap();
        DMAHeap(const DMAHeap&) = delete;
        DMAHeap(DMAHeap&&) = delete;
        ~DMAHeap();

        DMAHeap& operator=(const DMAHeap&) = delete;
        DMAHeap& operator=(DMAHeap&&) = delete;

        std::unique_ptr<DeviceBuffer> allocateBuffer(const std::shared_ptr<SystemAccess>& system, unsigned sizeInBytes,
            const std::string& name, CacheType cacheType);
        bool deallocateBuffer(const DeviceBuffer* buffer);

        bool flushCPUCache(const std::vector<const DeviceBuffer*>& buffers);

        /*
         * Brackets host accesses of a mapped buffer with DMA_BUF_IOCTL_SYNC. The dma-buf sync ioctl has no range
         * parameter, so the cache operation always covers the whole pool. Both are no-ops for non-mapped memory.
         */
        bool prepareBufferForHostAccess(const DeviceBuffer* buffer, bool forWriting);
        bool finishBufferForHostAccess(const DeviceBuffer* buffer, bool forWriting);

        bool readValue(SystemQuery query, uint32_t& output) noexcept;

        uint32_t getPhysicalBase() const
        {
            return physicalBase;
        }

        uint32_t getPoolSize() const
        {
            return static_cast<uint32_t>(poolSize);
        }

    private:
        int fd;
        void* hostBase;
        size_t poolSize;
        // physical address of the start of the dma-buf, found once at startup
        uint32_t physicalBase;
        // one entry per 4 KB block of the pool
        std::vector<uint8_t> usedBlocks;
        std::mutex allocationLock;
        // unique per-process marker written to the start and end of the pool, used to locate it in physical RAM
        char signature[64];

        size_t findFreeBlocks(size_t numBlocks) const;
        void markBlocks(size_t firstBlock, size_t numBlocks, bool used);

        bool isInsidePool(const DeviceBuffer* buffer) const;

        void fillSignature();
        void discoverPhysicalBase();
        bool verifyPoolBase(int memFd, uint32_t base) const;
        uint32_t scanForSignature(int memFd, uint32_t begin, uint32_t end) const;
    };

} /* namespace vc4cl */

#endif /* VC4CL_DMAHEAP_H */
