## NexusAlloc - C++17 Buddy Memory Allocator

NexusAlloc is a **multi-threaded buddy memory allocator** implemented in C++17. It is a portfolio project for **Distributed Systems / Low-Level Software Engineering** roles and demonstrates allocator design: manual memory management, lock-free structures, and predictable fragmentation—**benchmarked against** (not claiming to outperform) glibc `malloc`.

- **Manual memory management** using `mmap` on Linux (no malloc/free wrappers)
- **Lock-free free-list** in the buddy layer (CAS-based pop/push per order)
- **Per-thread cache** (64/128/256 B) with batch refill from the global pool (cache access guarded by a shared mutex; see Design tradeoffs)
- **Buddy block allocation** to reduce fragmentation and keep behavior predictable
- **Benchmarking** vs glibc `malloc`: throughput (ops/s), mean and sampled p50/p99 latency
- **Unit tests** (Google Test), **Valgrind** and **AddressSanitizer** for verification

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

**Environment:** The allocator uses `mmap`/`munmap` and targets **Linux**. Recommended: build and run on **WSL (Ubuntu)** or **native Linux**. On a Windows host, use WSL and build inside the Linux filesystem (e.g. clone/copy the repo to `~/06-NexusAlloc`) to avoid CMake "Operation not permitted" on `/mnt/c`. See [Building](#building) above.

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

The benchmark reports **throughput (ops/s)**, **mean latency (ns/op)**, and **approximate p50/p99 latency** (sampled). Timing includes per-op RNG and bookkeeping; both allocators see the same workload and overhead so the comparison is fair. It also prints relative throughput (NexusAlloc/glibc) and NexusAlloc fragmentation.

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
  - Owns a contiguous region via **`mmap`** (no malloc/free).
  - **Lock-free free-list**: one `std::atomic<FreeBlock*>` head per order; **pop** and **push** use Compare-And-Swap (CAS) loops for non-blocking alloc/free. No mutex in the buddy layer.
  - **Coalesce** on deallocate: drain the list for that order (atomic exchange), remove the buddy from the drained list, merge, push merged block and remaining nodes back.
  - Splits blocks by order; one free list per power-of-two order. Exposes `getBlockOrder()`, `orderForSize()`, `blockSizeForOrder()` for the cache layer.

- **`MemoryAllocator`**
  - RAII facade with **per-thread cache buckets** for 64 B, 128 B, 256 B (orders 0–2). Access to these buckets is guarded by a **single shared mutex** (contention point).
  - **allocate()**: if size maps to a cached order, take the mutex, check the thread’s bucket, pop if non-empty; else refill from the global buddy in a batch, then pop one. Otherwise call the buddy directly.
  - **deallocate()**: if the block’s order is cached and that bucket is under the cap, push into the bucket; else return to the global buddy.

### Design tradeoffs

- **Lock-free fast path (buddy):** Allocation and the push side of deallocate are mutex-free; CAS retries can increase under contention on the same order.
- **Coalescing:** We use drain-and-merge (exclusive access to the list for that order) instead of lock-free arbitrary remove.
- **Fragmentation vs throughput:** Buddy allocation gives predictable, bounded fragmentation. This project does not aim to beat glibc’s throughput; it emphasizes design and reproducibility.
- **Contention:** The per-thread cache uses one mutex for all threads; the global buddy free-lists are lock-free but can see CAS contention on popular orders.

---

### Performance proof

Benchmarks use the same workload for both allocators (thread count, ops/thread, size distribution). **`--profile mixed`**: sizes 16–4096 B; **`--profile small`**: 64–256 B (cache-friendly). Build with `-DCMAKE_BUILD_TYPE=Release` and run:

```bash
./benchmarks/nexusalloc_bench --threads 8 --ops-per-thread 500000
./benchmarks/nexusalloc_bench --threads 8 --ops-per-thread 500000 --profile small
```

Reported: **ops/s**, **mean ns/op**, **p50/p99 ns/op** (sampled). Timing includes per-op RNG and bookkeeping (same for both), so relative numbers are comparable.

**Example (8 threads, 500k ops/thread, WSL Ubuntu):**

| Profile        | Allocator     | Throughput (ops/s) | Mean latency (ns/op) |
|----------------|---------------|--------------------|----------------------|
| Mixed (16–4096 B)   | glibc malloc  | 2.43e+07           | 41.2                 |
| Mixed (16–4096 B)   | NexusAlloc    | 4.45e+06           | 224.8                |
| Small (64–256 B)    | glibc malloc  | 1.16e+08           | 8.6                  |
| Small (64–256 B)    | NexusAlloc    | 4.82e+06           | 207.5                |

NexusAlloc is slower than glibc on these workloads; the goal is to demonstrate allocator design and measurable, reproducible behavior, not to beat the system allocator.

