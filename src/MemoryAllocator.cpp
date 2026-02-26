#include "MemoryAllocator.hpp"

#include <thread>

namespace nexusalloc {

std::size_t MemoryAllocator::getThreadIndex() noexcept {
    static std::atomic<std::size_t> s_nextId{0};
    thread_local std::size_t t_id = s_nextId++;
    return t_id;
}

MemoryAllocator::MemoryAllocator(const Config& config)
    : m_buddy(BuddyAllocator::Options{config.poolSizeBytes, config.minBlockSizeBytes}) {}

MemoryAllocator::~MemoryAllocator() {
    std::size_t tid = getThreadIndex();
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (tid < m_caches.size()) {
        for (std::uint32_t order = 0; order <= kMaxCachedOrder; ++order) {
            for (void* ptr : m_caches[tid][order]) {
                m_buddy.deallocate(ptr);
            }
            m_caches[tid][order].clear();
        }
    }
}

std::size_t MemoryAllocator::bucketForOrder(std::uint32_t order) const noexcept {
    if (order <= kMaxCachedOrder) {
        return static_cast<std::size_t>(order);
    }
    return kMaxCachedOrder + 1;  // no cache
}

void MemoryAllocator::refillCache(std::size_t tid, std::uint32_t order) {
    std::size_t blockSize = m_buddy.blockSizeForOrder(order);
    std::size_t headerSize = m_buddy.blockHeaderSize();
    std::size_t allocSize = (blockSize > headerSize) ? (blockSize - headerSize) : 1;
    for (std::size_t i = 0; i < kRefillBatch; ++i) {
        void* ptr = m_buddy.allocate(allocSize);
        if (!ptr) {
            break;
        }
        m_caches[tid][order].push_back(ptr);
    }
}

void* MemoryAllocator::allocate(std::size_t size) {
    std::uint32_t order = m_buddy.orderForSize(size);
    if (order > kMaxCachedOrder) {
        return m_buddy.allocate(size);
    }

    std::size_t tid = getThreadIndex();
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (tid >= m_caches.size()) {
            m_caches.resize(tid + 1);
        }
        auto& bucket = m_caches[tid][order];
        if (!bucket.empty()) {
            void* ptr = bucket.back();
            bucket.pop_back();
            return ptr;
        }
        refillCache(tid, order);
        if (!bucket.empty()) {
            void* ptr = bucket.back();
            bucket.pop_back();
            return ptr;
        }
    }
    return m_buddy.allocate(size);
}

void MemoryAllocator::deallocate(void* ptr) {
    if (!ptr) {
        return;
    }
    std::uint32_t order = m_buddy.getBlockOrder(ptr);
    if (order > kMaxCachedOrder) {
        m_buddy.deallocate(ptr);
        return;
    }

    std::size_t tid = getThreadIndex();
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (tid >= m_caches.size()) {
        m_caches.resize(tid + 1);
    }
    auto& bucket = m_caches[tid][order];
    if (bucket.size() < kMaxCacheSize) {
        bucket.push_back(ptr);
        return;
    }
    m_buddy.deallocate(ptr);
}

void* MemoryAllocator::reallocate(void* ptr, std::size_t newSize) {
    return m_buddy.reallocate(ptr, newSize);
}

} // namespace nexusalloc
