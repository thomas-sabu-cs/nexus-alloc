#include "MemoryAllocator.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace {

struct IAllocator {
    virtual ~IAllocator() = default;
    virtual void* allocate(std::size_t size) = 0;
    virtual void deallocate(void* ptr) = 0;
};

struct SystemAllocator : IAllocator {
    void* allocate(std::size_t size) override { return std::malloc(size); }
    void deallocate(void* ptr) override { std::free(ptr); }
};

struct CustomAllocatorAdapter : IAllocator {
    explicit CustomAllocatorAdapter(nexusalloc::MemoryAllocator& alloc)
        : m_alloc(alloc) {}

    void* allocate(std::size_t size) override { return m_alloc.allocate(size); }
    void deallocate(void* ptr) override { m_alloc.deallocate(ptr); }

    nexusalloc::MemoryAllocator& m_alloc;
};

struct BenchmarkConfig {
    std::size_t threads = std::thread::hardware_concurrency() > 0
                              ? std::thread::hardware_concurrency()
                              : 4;
    std::size_t opsPerThread = 500000;
    std::size_t minAllocSize = 16;
    std::size_t maxAllocSize = 4096;
};

struct ThreadStats {
    std::size_t allocations = 0;
    std::size_t deallocations = 0;
};

struct BenchmarkResult {
    double seconds = 0.0;
    std::size_t totalOps = 0;
    double throughputOpsPerSec = 0.0;
    double meanLatencyNs = 0.0;  // mean nanoseconds per (alloc or free) operation
};

void worker(IAllocator& allocator,
            const BenchmarkConfig& cfg,
            std::uint64_t seed,
            ThreadStats& stats) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> opDist(0, 1);
    std::uniform_int_distribution<std::size_t> sizeDist(cfg.minAllocSize, cfg.maxAllocSize);

    std::vector<void*> live;
    live.reserve(1024);

    for (std::size_t i = 0; i < cfg.opsPerThread; ++i) {
        bool doAlloc = live.empty() || opDist(rng) == 0;
        if (doAlloc) {
            std::size_t size = sizeDist(rng);
            void* ptr = allocator.allocate(size);
            if (ptr) {
                live.push_back(ptr);
                ++stats.allocations;
            }
        } else {
            std::uniform_int_distribution<std::size_t> indexDist(0, live.size() - 1);
            std::size_t idx = indexDist(rng);
            void* ptr = live[idx];
            allocator.deallocate(ptr);
            live[idx] = live.back();
            live.pop_back();
            ++stats.deallocations;
        }
    }

    for (void* ptr : live) {
        allocator.deallocate(ptr);
        ++stats.deallocations;
    }
}

BenchmarkResult runBenchmark(const std::string& name,
                             IAllocator& allocator,
                             const BenchmarkConfig& cfg) {
    std::cout << "Running benchmark for: " << name << "\n";

    std::vector<std::thread> threads;
    std::vector<ThreadStats> stats(cfg.threads);

    auto start = high_resolution_clock::now();

    for (std::size_t i = 0; i < cfg.threads; ++i) {
        threads.emplace_back(worker,
                             std::ref(allocator),
                             std::cref(cfg),
                             static_cast<std::uint64_t>(i + 1),
                             std::ref(stats[i]));
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = high_resolution_clock::now();
    double seconds = duration_cast<duration<double>>(end - start).count();

    std::size_t totalOps = 0;
    std::size_t totalAlloc = 0;
    std::size_t totalFree = 0;
    for (const auto& s : stats) {
        totalOps += s.allocations + s.deallocations;
        totalAlloc += s.allocations;
        totalFree += s.deallocations;
    }

    double throughput = (seconds > 0.0) ? (totalOps / seconds) : 0.0;
    double meanLatencyNs = (totalOps > 0 && seconds > 0.0)
        ? (seconds * 1e9 / static_cast<double>(totalOps))
        : 0.0;

    std::cout << "  Time: " << seconds << " s\n";
    std::cout << "  Operations: " << totalOps << " (alloc=" << totalAlloc
              << ", free=" << totalFree << ")\n";
    std::cout << "  Throughput: " << throughput << " ops/s\n";
    std::cout << "  Mean latency: " << meanLatencyNs << " ns/op\n\n";

    return BenchmarkResult{seconds, totalOps, throughput, meanLatencyNs};
}

BenchmarkConfig parseArgs(int argc, char** argv) {
    BenchmarkConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 < argc) {
                return argv[++i];
            }
            return "";
        };

        if (arg == "--threads") {
            cfg.threads = static_cast<std::size_t>(std::stoul(next()));
        } else if (arg == "--ops-per-thread") {
            cfg.opsPerThread = static_cast<std::size_t>(std::stoul(next()));
        } else if (arg == "--min-size") {
            cfg.minAllocSize = static_cast<std::size_t>(std::stoul(next()));
        } else if (arg == "--max-size") {
            cfg.maxAllocSize = static_cast<std::size_t>(std::stoul(next()));
        } else if (arg == "--profile") {
            std::string p = next();
            if (p == "small") {
                cfg.minAllocSize = 64;
                cfg.maxAllocSize = 256;
            } else if (p == "mixed") {
                cfg.minAllocSize = 16;
                cfg.maxAllocSize = 4096;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: nexusalloc_bench [options]\n"
                         "  --threads N          Number of worker threads\n"
                         "  --ops-per-thread N   Operations per thread\n"
                         "  --min-size BYTES     Minimum allocation size\n"
                         "  --max-size BYTES     Maximum allocation size\n"
                         "  --profile PROFILE    Preset: mixed (16-4096 B) or small (64-256 B, cache-friendly)\n";
            std::exit(0);
        }
    }
    return cfg;
}

} // namespace

int main(int argc, char** argv) {
    BenchmarkConfig cfg = parseArgs(argc, argv);

    std::cout << "Threads: " << cfg.threads
              << ", ops/thread: " << cfg.opsPerThread
              << ", size range: " << cfg.minAllocSize << "-" << cfg.maxAllocSize << " bytes\n\n";

    SystemAllocator sysAlloc;
    auto sysResult = runBenchmark("System malloc/free", sysAlloc, cfg);

    nexusalloc::MemoryAllocator::Config allocCfg;
    allocCfg.poolSizeBytes = 1024ull * 1024 * 1024;  // 1 GiB so 8 threads × 500k ops don't exhaust
    allocCfg.minBlockSizeBytes = 64;

    nexusalloc::MemoryAllocator custom(allocCfg);
    CustomAllocatorAdapter customAdapter(custom);
    auto customResult = runBenchmark("NexusAlloc buddy allocator", customAdapter, cfg);

    std::cout << "=== Summary (multi-threaded workload) ===\n";
    std::cout << "  Allocator        | Throughput (ops/s) | Mean latency (ns/op)\n";
    std::cout << "  -----------------|--------------------|----------------------\n";
    std::cout << "  glibc malloc     | " << sysResult.throughputOpsPerSec << "        | "
              << sysResult.meanLatencyNs << "\n";
    std::cout << "  NexusAlloc      | " << customResult.throughputOpsPerSec << "        | "
              << customResult.meanLatencyNs << "\n";

    double speedup = (sysResult.throughputOpsPerSec > 0.0)
        ? (customResult.throughputOpsPerSec / sysResult.throughputOpsPerSec)
        : 0.0;
    std::cout << "\n  Relative throughput (NexusAlloc / glibc): " << speedup << "x\n";
    std::cout << "  NexusAlloc fragmentation estimate: " << custom.fragmentation() << "\n";

    return 0;
}

