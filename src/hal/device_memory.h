/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef VC4CL_DEVICE_MEMORY_H
#define VC4CL_DEVICE_MEMORY_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

/*
 * Helpers for accessing device memory that is mapped into the host address space (e.g. the mailbox memory mapped
 * through /dev/mem).
 *
 * On arm64 such mappings usually carry "Device" memory attributes, which only allow naturally aligned accesses of the
 * access size. The default glibc memcpy/memset implementations use 16-byte (and 8-byte) stores, so unaligned
 * 16-byte stores to this memory raise SIGBUS.
 *
 * Therefore, all copies into or out of device memory are done with at most 32-bit accesses: 32-bit volatile word
 * accesses for the aligned body and byte accesses for the unaligned head and tail. No 8-byte or 16-byte accesses are
 * ever emitted for device memory.
 */

namespace vc4cl
{
    namespace detail
    {
        inline bool isAligned32(const void* ptr) noexcept
        {
            return (reinterpret_cast<std::uintptr_t>(ptr) & 3u) == 0;
        }

        inline unsigned char readVolatileByte(const void* ptr) noexcept
        {
            return *static_cast<const volatile unsigned char*>(ptr);
        }

        inline void writeVolatileByte(void* ptr, unsigned char value) noexcept
        {
            *static_cast<volatile unsigned char*>(ptr) = value;
        }

        inline std::uint32_t readVolatileWord(const void* ptr) noexcept
        {
            return *static_cast<const volatile std::uint32_t*>(ptr);
        }

        inline void writeVolatileWord(void* ptr, std::uint32_t value) noexcept
        {
            *static_cast<volatile std::uint32_t*>(ptr) = value;
        }
    } // namespace detail

    /*
     * ARM (host) cache maintenance for device memory shared with the GPU.
     *
     * The QPU does not snoop the ARM data caches. Host writes to a device buffer can therefore stay dirty in the ARM
     * caches and be invisible to the GPU, while GPU writes can be hidden by stale ARM cache lines when the host reads
     * the buffer back. Both operations below are no-ops on non-aarch64 hosts.
     */

    constexpr std::size_t ARM_CACHE_LINE_SIZE = 64;

    /*
     * Cleans (writes back) and invalidates the ARM data cache lines covering [addr, addr + bytes).
     *
     * Used after the host wrote into device memory, so that the GPU observes the new content and the host does not
     * later read back stale data from its own cache.
     */
    inline void cleanInvalidateDataCache(void* addr, std::size_t bytes) noexcept
    {
#if defined(__aarch64__)
        if(addr == nullptr || bytes == 0)
            return;
        auto line = reinterpret_cast<std::uintptr_t>(addr) & ~static_cast<std::uintptr_t>(ARM_CACHE_LINE_SIZE - 1);
        const auto end = reinterpret_cast<std::uintptr_t>(addr) + bytes;
        for(; line < end; line += ARM_CACHE_LINE_SIZE)
            asm volatile("dc civac, %0" : : "r"(line) : "memory");
        asm volatile("dsb sy" : : : "memory");
#else
        (void)addr;
        (void)bytes;
#endif
    }

    /*
     * Invalidates the ARM data cache lines covering [addr, addr + bytes) without writing them back.
     *
     * Used before the host reads device memory the GPU wrote into, so that stale host cache lines cannot shadow the
     * GPU results.
     *
     * NOTE: Linux/arm64 does not let user space execute "dc ivac". The kernel runs EL0 with SCTLR_EL1.UCI == 0 and
     * emulates only DC CVAC, DC CIVAC, DC CVAU and IC IVAU in do_user_cache_maint(); every other cache maintenance
     * instruction is turned into SIGILL (verified on this target with kernel 6.18). Therefore the default
     * implementation falls back to the strongest operation that is actually available at EL0, DC CIVAC:
     *
     * - Mailbox buffers are mapped through /dev/mem with O_SYNC, i.e. as Normal Non-cacheable, so no host cache line
     *   for them exists and there is nothing to write back; the operation reduces to the DSB ordering barrier.
     * - For a cacheable mapping, cleaning would write stale host lines back over the GPU results, which is why the
     *   non-cacheable mailbox mapping is required for this fallback to be safe.
     *
     * Set VC4CL_EL0_DC_IVAC to 1 only when the target kernel really supports EL0 DC IVAC.
     */
#ifndef VC4CL_EL0_DC_IVAC
#define VC4CL_EL0_DC_IVAC 0
#endif

    // whether the target kernel provides EL0 "dc ivac", i.e. the only invalidate without write-back
    constexpr bool hasEl0DcIvac = VC4CL_EL0_DC_IVAC != 0;

    inline void invalidateDataCache(void* addr, std::size_t bytes) noexcept
    {
#if defined(__aarch64__)
        if(addr == nullptr || bytes == 0)
            return;
        auto line = reinterpret_cast<std::uintptr_t>(addr) & ~static_cast<std::uintptr_t>(ARM_CACHE_LINE_SIZE - 1);
        const auto end = reinterpret_cast<std::uintptr_t>(addr) + bytes;
#if VC4CL_EL0_DC_IVAC
        for(; line < end; line += ARM_CACHE_LINE_SIZE)
            asm volatile("dc ivac, %0" : : "r"(line) : "memory");
#else
        // DC CIVAC also writes dirty host cache lines back, which is only harmless when they cannot exist, i.e. for a
        // non-cacheable host mapping. Warn once, so a cacheable mapping is not silently corrupted.
        if(!hasEl0DcIvac)
        {
            static std::atomic<bool> warnedAboutCivacFallback{false};
            if(!warnedAboutCivacFallback.exchange(true))
                std::fprintf(stderr,
                    "[VC4CL] Warning: EL0 DC IVAC is not available (VC4CL_EL0_DC_IVAC=0), using DC CIVAC to "
                    "invalidate device memory. DC CIVAC cleans dirty host cache lines back to memory, which is only "
                    "safe for non-cacheable host mappings such as the /dev/mem mailbox buffers (O_SYNC). For a "
                    "cacheable mapping this writes stale host data over the GPU results.\n");
        }
        for(; line < end; line += ARM_CACHE_LINE_SIZE)
            asm volatile("dc civac, %0" : : "r"(line) : "memory");
#endif
        asm volatile("dsb sy" : : : "memory");
#else
        (void)addr;
        (void)bytes;
#endif
    }

    /*
     * Copies from normal host memory into device memory.
     */
    inline void deviceMemcpy(void* dst, const void* src, std::size_t bytes)
    {
        if(dst == nullptr || src == nullptr || bytes == 0)
            return;
        auto* dstBytes = static_cast<unsigned char*>(dst);
        const auto* srcBytes = static_cast<const unsigned char*>(src);
        std::size_t i = 0;
        // unaligned head: byte accesses only, 32-bit word stores require 4-byte alignment in device memory
        while(i < bytes && !(detail::isAligned32(dstBytes + i) && detail::isAligned32(srcBytes + i)))
        {
            detail::writeVolatileByte(dstBytes + i, srcBytes[i]);
            ++i;
        }
        // aligned body: read the word from host memory, then store it with a single 32-bit device access
        for(; i + sizeof(std::uint32_t) <= bytes; i += sizeof(std::uint32_t))
        {
            std::uint32_t word;
            std::memcpy(&word, srcBytes + i, sizeof(word));
            detail::writeVolatileWord(dstBytes + i, word);
        }
        // unaligned tail: byte accesses only
        for(; i < bytes; ++i)
            detail::writeVolatileByte(dstBytes + i, srcBytes[i]);

        // the GPU does not snoop the ARM caches, make the host writes visible to the QPU
        cleanInvalidateDataCache(dst, bytes);
    }

    /*
     * Copies from device memory into normal host memory.
     */
    inline void deviceMemcpyToHost(void* dst, const void* deviceSrc, std::size_t bytes)
    {
        if(dst == nullptr || deviceSrc == nullptr || bytes == 0)
            return;

        // drop stale host cache lines first, the GPU does not snoop the ARM caches
        invalidateDataCache(const_cast<void*>(deviceSrc), bytes);

        auto* dstBytes = static_cast<unsigned char*>(dst);
        const auto* srcBytes = static_cast<const unsigned char*>(deviceSrc);
        std::size_t i = 0;
        // unaligned head: byte accesses only, 32-bit word loads require 4-byte alignment in device memory
        while(i < bytes && !(detail::isAligned32(dstBytes + i) && detail::isAligned32(srcBytes + i)))
        {
            dstBytes[i] = detail::readVolatileByte(srcBytes + i);
            ++i;
        }
        // aligned body: load the word with a 32-bit volatile access, then store it to normal host memory
        for(; i + sizeof(std::uint32_t) <= bytes; i += sizeof(std::uint32_t))
        {
            const std::uint32_t word = detail::readVolatileWord(srcBytes + i);
            std::memcpy(dstBytes + i, &word, sizeof(word));
        }
        // unaligned tail: byte accesses only
        for(; i < bytes; ++i)
            dstBytes[i] = detail::readVolatileByte(srcBytes + i);
    }

    /*
     * Copies from device memory into device memory.
     */
    inline void deviceMemcpyDevice(void* dst, const void* deviceSrc, std::size_t bytes)
    {
        auto* dstBytes = static_cast<unsigned char*>(dst);
        const auto* srcBytes = static_cast<const unsigned char*>(deviceSrc);
        std::size_t i = 0;
        // unaligned head: byte accesses only, 32-bit word accesses require 4-byte alignment in device memory
        while(i < bytes && !(detail::isAligned32(dstBytes + i) && detail::isAligned32(srcBytes + i)))
        {
            detail::writeVolatileByte(dstBytes + i, detail::readVolatileByte(srcBytes + i));
            ++i;
        }
        // aligned body: 32-bit volatile load followed by 32-bit volatile store
        for(; i + sizeof(std::uint32_t) <= bytes; i += sizeof(std::uint32_t))
        {
            const std::uint32_t word = detail::readVolatileWord(srcBytes + i);
            detail::writeVolatileWord(dstBytes + i, word);
        }
        // unaligned tail: byte accesses only
        for(; i < bytes; ++i)
            detail::writeVolatileByte(dstBytes + i, detail::readVolatileByte(srcBytes + i));
    }

    /*
     * Fills device memory with the given byte value (same semantics as memset()).
     */
    inline void deviceMemset(void* dst, int value, std::size_t bytes)
    {
        if(dst == nullptr || bytes == 0)
            return;
        auto* dstBytes = static_cast<unsigned char*>(dst);
        const auto byteValue = static_cast<unsigned char>(value);
        std::size_t i = 0;
        // unaligned head: byte accesses only, 32-bit word stores require 4-byte alignment in device memory
        while(i < bytes && !detail::isAligned32(dstBytes + i))
        {
            detail::writeVolatileByte(dstBytes + i, byteValue);
            ++i;
        }
        // aligned body: replicate the byte across a 32-bit word and store it with a 32-bit device access
        const auto word = static_cast<std::uint32_t>(byteValue) * 0x01010101u;
        for(; i + sizeof(std::uint32_t) <= bytes; i += sizeof(std::uint32_t))
            detail::writeVolatileWord(dstBytes + i, word);
        // unaligned tail: byte accesses only
        for(; i < bytes; ++i)
            detail::writeVolatileByte(dstBytes + i, byteValue);

        // the GPU does not snoop the ARM caches, make the host writes visible to the QPU
        cleanInvalidateDataCache(dst, bytes);
    }
} // namespace vc4cl

#endif /* VC4CL_DEVICE_MEMORY_H */
