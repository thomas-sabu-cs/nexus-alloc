#include "BuddyAllocator.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <new>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#else
#error "BuddyAllocator currently supports only POSIX (Linux/macOS) via mmap."
#endif

namespace nexusalloc {

namespace {

std::size_t nextPowerOfTwo(std::size_t value) {
    if (value == 0) {
        return 1;
    }
    if ((value & (value - 1)) == 0) {
        return value;
    }
    std::size_t p = 1;
    while (p < value) {
        p <<= 1;
    }
    return p;
}

} // namespace

BuddyAllocator::BuddyAllocator(const Options& options)
    : m_base(nullptr)
    , m_poolSizeRequested(options.poolSizeBytes)
    , m_minBlockSize(std::max<std::size_t>(options.minBlockSizeBytes, sizeof(FreeBlock)))
{
    if (!isPowerOfTwo(m_minBlockSize)) {
        m_minBlockSize = nextPowerOfTwo(m_minBlockSize);
    }

    if (m_poolSizeRequested < m_minBlockSize) {
        m_poolSizeRequested = m_minBlockSize;
    }

    // Effective pool size is the largest power-of-two multiple of minBlockSize <= requested.
    std::size_t blocks = m_poolSizeRequested / m_minBlockSize;
    if (blocks == 0) {
        blocks = 1;
    }
    std::size_t powerOfTwoBlocks = nextPowerOfTwo(blocks);
    if (powerOfTwoBlocks > blocks) {
        powerOfTwoBlocks >>= 1;
    }
    if (powerOfTwoBlocks == 0) {
        powerOfTwoBlocks = 1;
    }
    m_poolSizeEffective = m_minBlockSize * powerOfTwoBlocks;

    std::size_t maxOrderBlocks = powerOfTwoBlocks;
    m_maxOrder = 0;
    while ((std::size_t(1) << m_maxOrder) < maxOrderBlocks) {
        ++m_maxOrder;
    }
    if ((std::size_t(1) << m_maxOrder) != maxOrderBlocks) {
        --m_maxOrder;
    }

    m_freeListHeads.reset(new std::atomic<FreeBlock*>[m_maxOrder + 1]());

    void* base = mmap(nullptr, m_poolSizeEffective, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        throw std::bad_alloc();
    }

    m_base = base;
    initializeFreeLists();
}

BuddyAllocator::~BuddyAllocator() {
    if (m_base != nullptr) {
        munmap(m_base, m_poolSizeEffective);
        m_base = nullptr;
    }
}

void BuddyAllocator::initializeFreeLists() {
    auto* initial = static_cast<FreeBlock*>(m_base);
    initial->header.order = m_maxOrder;
    initial->header.isFree = 1;
    initial->next = nullptr;
    m_freeListHeads[m_maxOrder].store(initial, std::memory_order_release);
    m_allocatedBytes.store(0, std::memory_order_relaxed);
}

std::uint32_t BuddyAllocator::orderForSize(std::size_t size) const noexcept {
    std::size_t totalSize = size + sizeof(BlockHeader);
    std::size_t blockSize = m_minBlockSize;
    std::uint32_t order = 0;

    while (blockSize < totalSize && order < m_maxOrder) {
        blockSize <<= 1;
        ++order;
    }

    if (blockSize < totalSize) {
        return m_maxOrder + 1;
    }
    return order;
}

BuddyAllocator::FreeBlock* BuddyAllocator::popFreeBlock(std::uint32_t order) {
    std::atomic<FreeBlock*>& headRef = m_freeListHeads[order];
    FreeBlock* head = headRef.load(std::memory_order_acquire);
    while (head != nullptr) {
        FreeBlock* next = head->next;
        if (headRef.compare_exchange_weak(head, next, std::memory_order_release)) {
            head->header.isFree = 0;
            head->next = nullptr;
            return head;
        }
    }
    return nullptr;
}

void BuddyAllocator::pushFreeBlock(std::uint32_t order, FreeBlock* block) {
    block->header.order = order;
    block->header.isFree = 1;
    std::atomic<FreeBlock*>& headRef = m_freeListHeads[order];
    FreeBlock* head = headRef.load(std::memory_order_acquire);
    for (;;) {
        block->next = head;
        if (headRef.compare_exchange_weak(head, block, std::memory_order_release)) {
            return;
        }
    }
}

BuddyAllocator::FreeBlock* BuddyAllocator::drainFreeList(std::uint32_t order) {
    return m_freeListHeads[order].exchange(nullptr, std::memory_order_acq_rel);
}

void BuddyAllocator::splitBlockDown(FreeBlock* block, std::uint32_t fromOrder, std::uint32_t toOrder) {
    std::uint32_t order = fromOrder;
    std::size_t blockSize = blockSizeForOrder(order);

    while (order > toOrder) {
        --order;
        blockSize >>= 1;

        auto* left = block;
        auto* right = reinterpret_cast<FreeBlock*>(reinterpret_cast<std::uint8_t*>(left) + blockSize);

        left->header.order = order;
        left->header.isFree = 0;
        right->header.order = order;
        right->header.isFree = 1;
        pushFreeBlock(order, right);

        block = left;
    }

    block->header.order = toOrder;
    block->header.isFree = 0;
}

BuddyAllocator::BlockHeader* BuddyAllocator::headerFromUserPtr(void* ptr) const noexcept {
    if (!ptr) {
        return nullptr;
    }
    auto* bytePtr = static_cast<std::uint8_t*>(ptr);
    return reinterpret_cast<BlockHeader*>(bytePtr - sizeof(BlockHeader));
}

void* BuddyAllocator::userPtrFromHeader(BlockHeader* header) const noexcept {
    if (!header) {
        return nullptr;
    }
    auto* bytePtr = reinterpret_cast<std::uint8_t*>(header);
    return static_cast<void*>(bytePtr + sizeof(BlockHeader));
}

void* BuddyAllocator::allocate(std::size_t size) {
    if (size == 0) {
        return nullptr;
    }

    std::uint32_t order = orderForSize(size);
    if (order > m_maxOrder) {
        return nullptr;
    }

    std::uint32_t searchOrder = order;
    FreeBlock* block = nullptr;
    while (searchOrder <= m_maxOrder && !block) {
        block = popFreeBlock(searchOrder);
        if (!block) {
            ++searchOrder;
        }
    }

    if (!block) {
        return nullptr;
    }

    if (searchOrder > order) {
        splitBlockDown(block, searchOrder, order);
    } else {
        block->header.order = order;
        block->header.isFree = 0;
    }

    std::size_t finalBlockSize = blockSizeForOrder(order);
    m_allocatedBytes.fetch_add(finalBlockSize, std::memory_order_relaxed);

    return userPtrFromHeader(&block->header);
}

BuddyAllocator::FreeBlock* BuddyAllocator::coalesce(FreeBlock* block) {
    std::uint8_t* base = static_cast<std::uint8_t*>(m_base);
    std::size_t offset = static_cast<std::size_t>(reinterpret_cast<std::uint8_t*>(block) - base);
    std::uint32_t order = block->header.order;
    std::size_t blockSize = blockSizeForOrder(order);

    while (order < m_maxOrder) {
        std::size_t buddyOffset = offset ^ blockSize;
        if (buddyOffset >= m_poolSizeEffective) {
            break;
        }

        auto* buddy = reinterpret_cast<FreeBlock*>(base + buddyOffset);

        // Drain the free list for this order so we can remove buddy.
        FreeBlock* listHead = drainFreeList(order);
        FreeBlock* found = nullptr;
        FreeBlock* cur = listHead;
        while (cur != nullptr) {
            FreeBlock* next = cur->next;
            if (cur == buddy) {
                found = buddy;
                cur->header.isFree = 0;
                cur->next = nullptr;
            } else {
                pushFreeBlock(order, cur);
            }
            cur = next;
        }

        if (!found || found->header.order != order) {
            // Buddy not in list (or wrong order); return block for caller to push.
            block->header.order = order;
            block->header.isFree = 1;
            return block;
        }

        std::size_t combinedOffset = std::min(offset, buddyOffset);
        block = reinterpret_cast<FreeBlock*>(base + combinedOffset);
        block->header.order = order + 1;
        block->header.isFree = 1;

        offset = combinedOffset;
        blockSize <<= 1;
        ++order;
    }

    return block;
}

void BuddyAllocator::deallocate(void* ptr) {
    if (!ptr) {
        return;
    }

    BlockHeader* header = headerFromUserPtr(ptr);
    if (!header) {
        return;
    }

    std::uint32_t order = header->order;
    if (order > m_maxOrder) {
        return;
    }

    std::size_t blockSize = blockSizeForOrder(order);
    m_allocatedBytes.fetch_sub(blockSize, std::memory_order_relaxed);

    auto* block = reinterpret_cast<FreeBlock*>(header);
    block->header.isFree = 1;

    FreeBlock* merged = coalesce(block);
    pushFreeBlock(merged->header.order, merged);
}

void* BuddyAllocator::reallocate(void* ptr, std::size_t newSize) {
    if (!ptr) {
        return allocate(newSize);
    }
    if (newSize == 0) {
        deallocate(ptr);
        return nullptr;
    }

    BlockHeader* header = headerFromUserPtr(ptr);
    if (!header) {
        return nullptr;
    }

    std::uint32_t currentOrder = header->order;
    std::size_t currentBlockSize = blockSizeForOrder(currentOrder);
    std::size_t usable = currentBlockSize - sizeof(BlockHeader);

    if (newSize <= usable) {
        return ptr;
    }

    void* newPtr = allocate(newSize);
    if (!newPtr) {
        return nullptr;
    }

    std::memcpy(newPtr, ptr, usable);
    deallocate(ptr);
    return newPtr;
}

double BuddyAllocator::fragmentation() const noexcept {
    std::size_t total = totalSize();
    if (total == 0) {
        return 0.0;
    }
    std::size_t allocated = allocatedSize();
    double usedRatio = static_cast<double>(allocated) / static_cast<double>(total);
    return 1.0 - usedRatio;
}

std::uint32_t BuddyAllocator::getBlockOrder(void* ptr) const noexcept {
    BlockHeader* h = headerFromUserPtr(ptr);
    return h ? h->order : 0;
}

} // namespace nexusalloc

