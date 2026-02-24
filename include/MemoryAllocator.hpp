#pragma once

#include "BuddyAllocator.hpp"

namespace nexusalloc {

// High-level facade around BuddyAllocator that exposes the main API.
class MemoryAllocator {
public:
    struct Config {
        std::size_t poolSizeBytes = 256ull * 1024 * 1024;
        std::size_t minBlockSizeBytes = 64;
    };

    explicit MemoryAllocator(const Config& config = Config{})
        : m_buddy(BuddyAllocator::Options{config.poolSizeBytes, config.minBlockSizeBytes}) {}

    MemoryAllocator(const MemoryAllocator&) = delete;
    MemoryAllocator& operator=(const MemoryAllocator&) = delete;
    MemoryAllocator(MemoryAllocator&&) = delete;
    MemoryAllocator& operator=(MemoryAllocator&&) = delete;

    ~MemoryAllocator() = default;

    void* allocate(std::size_t size) { return m_buddy.allocate(size); }
    void deallocate(void* ptr) { m_buddy.deallocate(ptr); }

    void* reallocate(void* ptr, std::size_t newSize) { return m_buddy.reallocate(ptr, newSize); }

    std::size_t totalSize() const noexcept { return m_buddy.totalSize(); }
    std::size_t allocatedSize() const noexcept { return m_buddy.allocatedSize(); }
    std::size_t freeSize() const noexcept { return m_buddy.freeSize(); }
    double fragmentation() const noexcept { return m_buddy.fragmentation(); }

private:
    BuddyAllocator m_buddy;
};

} // namespace nexusalloc

