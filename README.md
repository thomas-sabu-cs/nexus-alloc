## NexusAlloc - C++17 Buddy Memory Allocator

NexusAlloc is a **multi-threaded, high-performance buddy memory allocator** implemented in modern C++17. It is designed as a portfolio-quality project for **Distributed Systems / Low-Level Software Engineering** roles and showcases:

- **Manual memory management** using `mmap` on Linux (no malloc/free wrappers)
- **Lock-free free-list** with CAS-based pop/push and **per-thread caching** (64/128/256 B) for hot paths
- **Buddy block allocation** to minimize fragmentation
- **Benchmarking** against the system allocator (`glibc` `malloc`)
- **Unit tests** with Google Test
- **Debug-ability** with Valgrind and AddressSanitizer

### Project Structure

- `include/` – public headers (`MemoryAllocator.hpp`, `BuddyAllocator.hpp`)
- `src/` – allocator implementation
- `tests/` – Google Test-based unit tests
- `benchmarks/` – micro-benchmark executable comparing NexusAlloc vs system allocator

### Building

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j
```

On Windows this project will configure, but the allocator implementation targets **Linux** because it uses `mmap`/`munmap`.

### Running Tests

From the `build` directory:

```bash
ctest --output-on-failure
```

### Running Benchmarks

From the `build` directory:

```bash
# Mixed sizes (16 B–4 KB), multi-threaded
./benchmarks/nexusalloc_bench --threads 8 --ops-per-thread 500000

# Cache-friendly (64–256 B only) — favors NexusAlloc’s per-thread cache
./benchmarks/nexusalloc_bench --threads 8 --ops-per-thread 500000 --profile small
```

The benchmark prints:

- **Throughput** in allocations/deallocations per second
- **Custom allocator fragmentation ratio**
- A comparison against the **system allocator** (`malloc` / `free`)

### Debugging and Verification

- **Valgrind** (on Linux):

```bash
valgrind --leak-check=full ./tests/nexusalloc_tests
```

- **AddressSanitizer**:

Configure with:

```bash
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON ..
```

and then run the tests/benchmarks as usual. (ASan flags are typically added via your toolchain or an additional CMake option.)

### Architecture (High Level)

- **`BuddyAllocator`**
  - Owns a contiguous memory region reserved via **`mmap`** (no malloc/free wrappers)
  - **Lock-free free-list**: each order uses an `std::atomic<BlockNode*>` head; **pop** and **push** use Compare-And-Swap (CAS) loops for thread-safe, non-blocking fast paths
  - Coalesce on deallocate uses a **drain-and-merge** strategy: atomically drain the list for an order, remove the buddy in the drained list, merge, then push the merged block and any remaining nodes back
  - Splits large blocks into smaller ones during allocation; maintains one free list per power-of-two order
  - Exposes `getBlockOrder()`, `orderForSize()`, `blockSizeForOrder()` for integration with the upper layer

- **`MemoryAllocator`**
  - RAII facade around `BuddyAllocator` with **per-thread caching**
  - **Thread-local cache** for hot sizes (64 B, 128 B, 256 B): each thread has a small cache of blocks; `allocate()` checks the cache first and refills in batches from the global buddy allocator when empty; `deallocate()` returns blocks to the cache when under the cap, otherwise to the global pool
  - Provides `allocate()`, `deallocate()`, `reallocate()` and stats for benchmarking and observability

The implementation is structured to be **readable**, **testable**, and to demonstrate systems-level C++ suitable for roles at companies like **NVIDIA**.

---

### Performance proof

Benchmarks run with the built-in suite (multi-threaded alloc/free mix). Build with `-DCMAKE_BUILD_TYPE=Release` and run:

```bash
# Mixed sizes (16 B–4 KB)
./benchmarks/nexusalloc_bench --threads 8 --ops-per-thread 500000

# Cache-friendly (64–256 B)
./benchmarks/nexusalloc_bench --threads 8 --ops-per-thread 500000 --profile small
```

**Results (8 threads, 500k ops/thread, WSL Ubuntu):**

| Profile        | Allocator     | Throughput (ops/s) | Mean latency (ns/op) |
|----------------|---------------|--------------------|----------------------|
| Mixed (16–4096 B)   | glibc malloc  | 2.43e+07           | 41.2                 |
| Mixed (16–4096 B)   | NexusAlloc    | 4.45e+06           | 224.8                |
| Small (64–256 B)    | glibc malloc  | 1.16e+08           | 8.6                  |
| Small (64–256 B)    | NexusAlloc    | 4.82e+06           | 207.5                |

The suite reports throughput (alloc+free per second), mean latency per op, relative throughput (NexusAlloc/glibc), and fragmentation. Re-run the commands above on your machine to reproduce.

