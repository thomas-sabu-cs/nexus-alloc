## NexusAlloc - C++17 Buddy Memory Allocator

NexusAlloc is a **multi-threaded, high-performance buddy memory allocator** implemented in modern C++17. It is designed as a portfolio-quality project for **Distributed Systems / Low-Level Software Engineering** roles and showcases:

- **Manual memory management** using `mmap` on Linux (no malloc/free wrappers)
- **Thread safety** with `std::mutex` and `std::atomic`
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
./benchmarks/nexusalloc_bench --threads 8 --ops-per-thread 500000
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
  - Owns a contiguous memory region reserved via `mmap`
  - Manages a set of power-of-two **free lists** (one per order)
  - Splits large blocks into smaller ones during allocation
  - Coalesces buddies on deallocation to reduce fragmentation
  - Maintains statistics (allocated bytes, fragmentation estimate)

- **`MemoryAllocator`**
  - RAII wrapper/facade around `BuddyAllocator`
  - Provides `allocate()`, `deallocate()`, `reallocate()` API
  - Exposes stats for benchmarking and observability


