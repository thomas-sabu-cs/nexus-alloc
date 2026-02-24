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

    m_freeLists.resize(m_maxOrder + 1, nullptr);

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
    m_freeLists[m_maxOrder] = initial;
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
    FreeBlock* head = m_freeLists[order];
    if (!head) {
        return nullptr;
    }
    m_freeLists[order] = head->next;
    head->header.isFree = 0;
    head->next = nullptr;
    return head;
}

void BuddyAllocator::pushFreeBlock(std::uint32_t order, FreeBlock* block) {
    block->header.order = order;
    block->header.isFree = 1;
    block->next = m_freeLists[order];
    m_freeLists[order] = block;
}

void BuddyAllocator::removeFreeBlock(std::uint32_t order, FreeBlock* block) {
    FreeBlock* prev = nullptr;
    FreeBlock* cur = m_freeLists[order];
    while (cur && cur != block) {
        prev = cur;
        cur = cur->next;
    }
    if (!cur) {
        return;
    }
    if (prev) {
        prev->next = cur->next;
    } else {
        m_freeLists[order] = cur->next;
    }
    cur->header.isFree = 0;
    cur->next = nullptr;
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
        right->next = m_freeLists[order];
        m_freeLists[order] = right;

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

    std::lock_guard<std::mutex> lock(m_mutex);

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
        if (!buddy->header.isFree || buddy->header.order != order) {
            break;
        }

        removeFreeBlock(order, buddy);

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

    std::lock_guard<std::mutex> lock(m_mutex);

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

    std::lock_guard<std::mutex> lock(m_mutex);

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

    std::lock_guard<std::mutex> unlockGuard(m_mutex, std::adopt_lock);
    (void)unlockGuard;

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

} // namespace nexusalloc

