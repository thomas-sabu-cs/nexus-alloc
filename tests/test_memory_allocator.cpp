#include "MemoryAllocator.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using nexusalloc::MemoryAllocator;

TEST(MemoryAllocatorBasic, AllocateAndDeallocate) {
    MemoryAllocator allocator;

    void* p1 = allocator.allocate(64);
    void* p2 = allocator.allocate(128);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);

    EXPECT_GT(allocator.allocatedSize(), 0u);

    allocator.deallocate(p1);
    allocator.deallocate(p2);
    EXPECT_LE(allocator.allocatedSize(), allocator.totalSize());
}

TEST(MemoryAllocatorBasic, ReallocateLargerPreservesData) {
    MemoryAllocator allocator;

    const std::size_t initialSize = 64;
    const std::size_t biggerSize = 1024;

    auto* p = static_cast<std::uint8_t*>(allocator.allocate(initialSize));
    ASSERT_NE(p, nullptr);

    for (std::size_t i = 0; i < initialSize; ++i) {
        p[i] = static_cast<std::uint8_t>(i & 0xFF);
    }

    auto* p2 = static_cast<std::uint8_t*>(allocator.reallocate(p, biggerSize));
    ASSERT_NE(p2, nullptr);

    for (std::size_t i = 0; i < initialSize; ++i) {
        EXPECT_EQ(p2[i], static_cast<std::uint8_t>(i & 0xFF));
    }

    allocator.deallocate(p2);
}

TEST(MemoryAllocatorBasic, ExhaustsPoolGracefully) {
    MemoryAllocator::Config cfg;
    cfg.poolSizeBytes = 1024 * 1024;
    cfg.minBlockSizeBytes = 64;
    MemoryAllocator allocator(cfg);

    std::vector<void*> blocks;
    for (;;) {
        void* p = allocator.allocate(4096);
        if (!p) {
            break;
        }
        blocks.push_back(p);
    }

    EXPECT_FALSE(blocks.empty());

    for (void* p : blocks) {
        allocator.deallocate(p);
    }

    EXPECT_LE(allocator.allocatedSize(), allocator.totalSize());
}

TEST(MemoryAllocatorConcurrency, MultiThreadedAllocFree) {
    MemoryAllocator allocator;

    const std::size_t threadCount = 8;
    const std::size_t itersPerThread = 5000;

    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (std::size_t t = 0; t < threadCount; ++t) {
        threads.emplace_back([&allocator, itersPerThread]() {
            std::vector<void*> locals;
            locals.reserve(256);
            for (std::size_t i = 0; i < itersPerThread; ++i) {
                void* p = allocator.allocate(128);
                if (p) {
                    locals.push_back(p);
                }
                if (!locals.empty() && (i % 2 == 0)) {
                    allocator.deallocate(locals.back());
                    locals.pop_back();
                }
            }
            for (void* p : locals) {
                allocator.deallocate(p);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_LE(allocator.allocatedSize(), allocator.totalSize());
}

