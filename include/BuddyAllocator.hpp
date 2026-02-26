#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace nexusalloc {

// Low-level buddy allocator managing a contiguous memory pool.
// Thread safe: lock-free free-list (CAS-based pop/push); coalesce uses drain-and-merge.
class BuddyAllocator {
public:
    struct Options {
        std::size_t poolSizeBytes = 256ull * 1024 * 1024; // 256 MiB
        std::size_t minBlockSizeBytes = 64;               // minimum block size (power-of-two base)
    };

    explicit BuddyAllocator(const Options& options = {256ull * 1024 * 1024, 64});
    ~BuddyAllocator();

    BuddyAllocator(const BuddyAllocator&) = delete;
    BuddyAllocator& operator=(const BuddyAllocator&) = delete;
    BuddyAllocator(BuddyAllocator&&) = delete;
    BuddyAllocator& operator=(BuddyAllocator&&) = delete;

    void* allocate(std::size_t size);
    void deallocate(void* ptr);
    void* reallocate(void* ptr, std::size_t newSize);

    std::size_t totalSize() const noexcept { return m_poolSizeEffective; }
    std::size_t allocatedSize() const noexcept { return m_allocatedBytes.load(std::memory_order_relaxed); }
    std::size_t freeSize() const noexcept { return totalSize() - allocatedSize(); }

    // Fragmentation estimate in [0, 1]. 0 = no fragmentation, 1 = fully free.
    double fragmentation() const noexcept;

    // Returns the order of the block containing ptr (for cache bucketing). Undefined if ptr is invalid.
    std::uint32_t getBlockOrder(void* ptr) const noexcept;

    // Order needed for a given allocation size. Used by MemoryAllocator cache.
    std::uint32_t orderForSize(std::size_t size) const noexcept;

    // Block size for a given order (minBlockSize << order). Used by MemoryAllocator cache.
    std::size_t blockSizeForOrder(std::uint32_t order) const noexcept {
        return m_minBlockSize << order;
    }

    std::size_t blockHeaderSize() const noexcept { return sizeof(BlockHeader); }

private:
    struct BlockHeader {
        std::uint32_t order;  // which free list this block belongs to
        std::uint32_t isFree; // 1 if block is on a free list
    };

    struct FreeBlock {
        BlockHeader header;
        FreeBlock* next;
    };

    static constexpr std::size_t alignUp(std::size_t value, std::size_t alignment) noexcept {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static constexpr bool isPowerOfTwo(std::size_t value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    void initializeFreeLists();

    // Lock-free free-list: CAS-based pop/push.
    FreeBlock* popFreeBlock(std::uint32_t order);
    void pushFreeBlock(std::uint32_t order, FreeBlock* block);
    // Drain entire list for an order (used by coalesce). Returns head; list head becomes nullptr.
    FreeBlock* drainFreeList(std::uint32_t order);

    void splitBlockDown(FreeBlock* block, std::uint32_t fromOrder, std::uint32_t toOrder);
    FreeBlock* coalesce(FreeBlock* block);

    BlockHeader* headerFromUserPtr(void* ptr) const noexcept;
    void* userPtrFromHeader(BlockHeader* header) const noexcept;

    void* m_base{nullptr};
    std::size_t m_poolSizeRequested{0};
    std::size_t m_poolSizeEffective{0};
    std::size_t m_minBlockSize{0};
    std::uint32_t m_maxOrder{0};

    // One atomic head per order (lock-free stack). Uses raw array because std::atomic is not copyable.
    std::unique_ptr<std::atomic<FreeBlock*>[]> m_freeListHeads;
    std::atomic<std::size_t> m_allocatedBytes{0};
};

} // namespace nexusalloc

