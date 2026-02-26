# NexusAlloc Audit Report

## 1) Lock-free free-list

| Claim | Reality | Action |
|-------|---------|--------|
| "std::atomic<BlockNode*>" | Code uses `std::atomic<FreeBlock*>` (type is `FreeBlock`, not `BlockNode`). | README: use "FreeBlock*" or "atomic head pointer". |
| CAS loops for push/pop | **Verified.** `popFreeBlock` and `pushFreeBlock` use compare_exchange_weak loops; no mutex. | None. |
| Per-order free lists | **Verified.** `m_freeListHeads[order]` is one atomic head per order. | None. |
| Coalesce path uses mutex? | **No.** Coalesce uses `drainFreeList` (exchange) and lock-free push; no mutex in BuddyAllocator. | None. |

**Conclusion:** Lock-free free-list is real. Only fix: README type name (BlockNode → FreeBlock).

---

## 2) Per-thread caching

| Claim | Reality | Action |
|-------|---------|--------|
| "thread_local cache" for 64/128/256 B | Caches are **per-thread buckets** in a shared `m_caches[tid][order]`, but **all access is guarded by a single `m_cacheMutex`**. So it's per-thread storage with serialized access. | README: clarify "per-thread buckets protected by a shared mutex". |
| allocate() checks cache first | **Verified.** Under lock: if bucket non-empty, pop and return; else refill then pop. | None. |
| Refill in batches from global | **Verified.** `refillCache` calls `m_buddy.allocate` kRefillBatch (8) times. | None. |
| deallocate() to cache under cap else global | **Verified.** If `bucket.size() < kMaxCacheSize` push to cache, else `m_buddy.deallocate(ptr)`. | None. |

**Conclusion:** Per-thread caching is real; README should note the cache mutex as a contention point.

---

## 3) Benchmark correctness and reporting

| Issue | Finding | Action |
|-------|---------|--------|
| Timing includes RNG/vector? | Yes. Timed region includes RNG, `live` vector ops, and allocator calls. Same for both allocators → relative comparison valid; absolute numbers include overhead. | Add one-line methodology note in output and README. |
| printf in loop? | No. No I/O in worker loop. | None. |
| Identical workload for both? | Yes. Same `cfg`, same thread count, same ops per thread, same size distribution. | None. |
| --profile small / mixed? | **Verified.** `--profile small` → 64–256 B; `--profile mixed` → 16–4096 B. | None. |
| Output: ops/sec, ns/op | Present. | None. |
| p50/p99 latency? | Not reported. | Add optional approximate p50/p99 via sampling in worker. |
| Vector growth in worker? | `live.reserve(1024)` but workload can exceed 1024 live pointers → possible realloc in hot path. | Reserve larger (e.g. min(opsPerThread, 200000)) to avoid growth. |

**Conclusion:** Methodology is sound; add methodology note, optional p50/p99, and larger reserve.

---

## 4) README honesty and clarity

| Issue | Action |
|-------|--------|
| "high-performance" implies beating glibc | Rephrase to emphasize design, predictability, and benchmarking vs glibc without claiming superiority. |
| Why buddy can be slower | Add Design Tradeoffs: lock-free fast path, coalesce complexity, fragmentation vs throughput, cache mutex contention. |
| Environment (WSL / Linux) | Add Environment section: build/run on WSL or native Linux; note Windows host. |
| Design Tradeoffs section | Add: lock-free fast path in buddy; coalesce drain-and-merge; fragmentation behavior vs raw throughput; single cache mutex. |

---

## Deliverables (applied)

- **Mismatches:** Listed above.
- **Code changes applied:**
  - **README.md:** Intro rephrased (no “beat glibc”); Architecture: `BlockNode` → `FreeBlock`, cache mutex and per-thread buckets described; new **Environment** note (WSL/Linux, build on Linux fs); new **Design tradeoffs** section; Performance proof updated (methodology, p50/p99, honest “slower than glibc”).
  - **benchmarks/benchmark_allocators.cpp:** `live.reserve(std::min(opsPerThread, 200000))` to avoid vector growth in hot path; latency sampling every 50 ops (up to 2000 samples/thread) for p50/p99; one-line methodology note in output (“Timing includes per-op RNG and bookkeeping…”); summary prints p50/p99 when available; `BenchmarkResult` and `ThreadStats` extended with latency samples and p50/p99.
- **README:** Honest language, Environment, Design tradeoffs, and Performance proof all updated as above.
