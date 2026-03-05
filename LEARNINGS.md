# LEARNINGS — NexusAlloc (Buddy Memory Allocator)

**tl;dr:** Implemented a C++17 buddy allocator on top of `mmap` with lock-free free-lists and a per-thread small-block cache. Learned where things actually get hard: correct order/alignment math, safe coalescing with a lock-free stack (drain-and-merge), contention from CAS under load, and how to benchmark fairly against glibc across realistic size profiles.

## 1. What I Attempted

- **Multi-threaded buddy allocator in C++17** using `mmap` for the pool (no malloc/free). One contiguous region, power-of-two block sizes, split and merge (coalesce) on alloc/free.
- **Lock-free free-lists per order** (one `std::atomic<FreeBlock*>` head per order, CAS push/pop) and a **per-thread cache** for 64/128/256 B with batch refill from the global buddy; cache access is guarded by a single shared mutex (per AUDIT.md).
- **Benchmarked against glibc malloc** to study throughput (ops/s), mean and p50/p99 latency, and fragmentation—same workload for both allocators, with `--profile mixed` (16–4096 B) and `--profile small` (64–256 B).

## 2. Architecture Snapshot

- **Buddy allocator:** One `mmap`’d region; effective pool size is a power-of-two multiple of `minBlockSize` (e.g. 64 B). Orders 0..N map to block sizes `minBlockSize << order`. One **free-list per order**; each head is `std::atomic<FreeBlock*>`. **Allocate:** compute order for requested size, pop from that order (or higher), split down if needed, update `m_allocatedBytes`. **Deallocate:** push block, then **coalesce** by draining the list for that order (atomic exchange), finding the buddy in the drained list, merging, and pushing the merged block (and any other nodes) back; repeat for higher orders. No mutex in the buddy layer.
- **Lock-free free-list:** Pop: load head, if non-null CAS head to head->next, mark block used, return. Push: load head, set block->next = head, CAS head to block. Coalesce needs to “remove” the buddy from a lock-free list; the implementation uses **drain-and-merge** (exchange the list head to nullptr, scan the list to remove the buddy, merge, then push the merged block and push the other nodes back) so there’s no arbitrary remove on a lock-free stack.
- **Per-thread cache:** `MemoryAllocator` keeps `m_caches[tid][order]` for orders 0–2 (64/128/256 B). Thread ID from a static atomic counter + thread_local. **Allocate:** if order ≤ 2, take `m_cacheMutex`, ensure `m_caches` has a slot for this thread, pop from bucket if non-empty; else call `refillCache` (batch of 8 from buddy) then pop one; else fall back to `m_buddy.allocate(size)`. **Deallocate:** if order ≤ 2 and bucket size < 32, push into bucket under the same mutex; else call `m_buddy.deallocate(ptr)`. Destructor flushes the current thread’s cache back to the buddy.
- **Tests:** Google Test in `tests/` — basic alloc/dealloc, realloc larger (data preserved), pool exhaustion (alloc until null, then free all), and multi-threaded alloc/free (8 threads, 5k iters, 128 B). No I/O in the benchmark worker loop; `benchmarks/nexusalloc_bench` runs the same config for glibc and NexusAlloc, reports ops/s, mean ns/op, and sampled p50/p99; `live` is reserved to avoid realloc in the hot path (AUDIT).

## 3. What Broke / Got Tricky

- **Alignment and order:** Block size must fit header + user bytes; `orderForSize` rounds up to the next power-of-two block; effective pool size is rounded down to a power-of-two multiple of `minBlockSize`. Getting the effective pool and max order right so the initial free list has exactly one block at max order is easy to get wrong.
- **Lock-free under contention:** CAS in push/pop can spin when many threads hit the same order; no mutex means no deadlock but possible livelock or heavy retries. Coalesce avoids lock-free “remove arbitrary node” by draining the whole list for that order (exchange), which is correct but can be costly when the list is long.
- **Coalescing:** Finding the buddy by address (offset ^ blockSize), then removing it from the free list, only works if we have exclusive access to the list—hence drain, then linear scan to unlink buddy, merge, push merged block and remaining nodes back.
- **Per-thread cache vs contention:** Per-thread buckets reduce traffic to the global buddy for hot sizes, but a **single mutex** for all cache access (AUDIT) serializes every cache hit/miss across threads, which becomes a bottleneck at high concurrency.
- **Benchmarks:** Ensuring both allocators see identical workload (same thread count, ops per thread, size distribution) and that timing isn’t skewed (e.g. no printf in loop, same RNG/bookkeeping; AUDIT noted reserving `live` to avoid vector growth in the timed region). Adding p50/p99 required sampling (e.g. every 50th op) and aggregating across threads.

## 4. What I Learned (Technical Delta)

- **Buddy allocation** gives predictable, bounded fragmentation: all blocks are power-of-two; splitting and merging are deterministic. The trade-off is internal fragmentation (round-up to block size) and potentially more operations (split/merge) than a single global free-list.
- **Lock-free (CAS) vs mutex:** Lock-free push/pop avoid lock contention and deadlock but introduce retry loops and careful memory ordering (acquire/release on the atomic head). Coalesce doesn’t fit a simple lock-free stack (arbitrary remove), so drain-and-merge was used instead of a more complex lock-free list.
- **Fast path for small sizes:** Real allocators often separate small-object pools (or caches) from large-object paths. Here, the per-thread cache for 64/128/256 B is that idea; the cost is the shared mutex and the need to refill in batches from the global buddy.
- **Benchmark results:** NexusAlloc is slower than glibc on the measured workloads (README and AUDIT). The project prioritizes clear design, reproducible benchmarks, and predictable fragmentation over beating the system allocator; the benchmark suite makes that comparison explicit (ops/s, mean and p50/p99, same workload).

## 5. Why This Version Is Better

- **Free-list and coalescing:** The current design uses atomic heads and CAS for push/pop only; coalesce uses drain-and-merge instead of trying to remove a node from a lock-free list in place, which would require extra machinery (e.g. hazard pointers or a different structure). The split path uses the same lock-free push for the “right” half-blocks, so the buddy layer stays mutex-free.
- **Per-thread cache:** Introducing per-thread buckets for 64/128/256 B reduces calls to the global buddy for those sizes and batches refills (8 at a time), which can amortize contention on the buddy’s free-lists. The trade-off (single cache mutex) is documented in the README and AUDIT as a known contention point.
- **Tests and tooling:** Google Test covers basic behavior, exhaustion, and concurrency; README and AUDIT recommend Valgrind and AddressSanitizer to catch leaks and memory errors. The benchmark was tightened (reserve for `live`, latency sampling for p50/p99, methodology note) so results are comparable and interpretable.

## 6. Next Iteration Plan

- **Large allocations / huge pages:** Support for allocations that exceed the pool (e.g. fallback to `mmap` for one-off large blocks) or optional use of huge pages for the main pool could improve behavior for mixed workloads.
- **Per-thread cache design:** Reduce contention by using a lock-free or per-thread lock for cache buckets instead of one global mutex, or by making the cache truly thread-local with a bounded size and a lock-free structure for refill/overflow.
- **Benchmarking:** More profiles (e.g. size classes, allocation patterns), different platforms (native Linux vs WSL), and perhaps a “allocator-only” timing mode (minus RNG/vector in the loop) to isolate allocator cost; keep reporting ops/s, mean, and p50/p99.
