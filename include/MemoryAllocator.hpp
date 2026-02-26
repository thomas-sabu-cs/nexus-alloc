#pragma once

#include "BuddyAllocator.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace nexusalloc {

// High-level facade around BuddyAllocator with per-thread caching for hot sizes (64/128/256B).
class MemoryAllocator {
public:
    struct Config {
        std::size_t poolSizeBytes = 256ull * 1024 * 1024;
        std::size_t minBlockSizeBytes = 64;
    };

    explicit MemoryAllocator(const Config& config = {256ull * 1024 * 1024, 64});
    ~MemoryAllocator();

    MemoryAllocator(const MemoryAllocator&) = delete;
    MemoryAllocator& operator=(const MemoryAllocator&) = delete;
    MemoryAllocator(MemoryAllocator&&) = delete;
    MemoryAllocator& operator=(MemoryAllocator&&) = delete;

    void* allocate(std::size_t size);
    void deallocate(void* ptr);
    void* reallocate(void* ptr, std::size_t newSize);

    std::size_t totalSize() const noexcept { return m_buddy.totalSize(); }
    std::size_t allocatedSize() const noexcept { return m_buddy.allocatedSize(); }
    std::size_t freeSize() const noexcept { return m_buddy.freeSize(); }
    double fragmentation() const noexcept { return m_buddy.fragmentation(); }

private:
    static constexpr std::uint32_t kMaxCachedOrder = 2;  // cache orders 0, 1, 2 (e.g. 64B, 128B, 256B)
    static constexpr std::size_t kMaxCacheSize = 32;
    static constexpr std::size_t kRefillBatch = 8;

    static std::size_t getThreadIndex() noexcept;

    std::size_t bucketForOrder(std::uint32_t order) const noexcept;
    void refillCache(std::size_t tid, std::uint32_t order);

    BuddyAllocator m_buddy;
    mutable std::mutex m_cacheMutex;
    std::vector<std::array<std::vector<void*>, kMaxCachedOrder + 1>> m_caches;
};

} // namespace nexusalloc

