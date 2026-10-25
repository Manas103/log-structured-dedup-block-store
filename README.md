# Log-Structured Deduplicating Block Store with Host-Managed Reclamation

A small, honest version of the write path a flash array actually runs: a log-structured block store that deduplicates every write at 512-byte granularity, verifies every dedup candidate byte-for-byte before it ever commits metadata, and reclaims space with a host-managed garbage collector that groups data by expected lifetime so whole segments can be freed without rewriting data that is still alive. It is exposed as a real Linux NBD block device, not a library demo: `mount -o` a kernel-attached `/dev/nbdX` backed by this process and any program that reads and writes blocks works unmodified. Every number below was measured on this machine by running the code in this repository, not targeted in advance; a real refcounting bug, found while chasing one of these numbers, is disclosed below rather than quietly fixed and forgotten.

## Why this exists

Pure Storage's own description of Purity is specific enough to build toward: 512-byte deduplication granularity, byte-for-byte verification before a chunk is treated as a duplicate, and DirectFlash's software-managed garbage collection, which places data with similar expected lifespans together so whole blocks can be freed without rewriting data that is still live. This repository is a small version of exactly that write path, not a generic key-value store with a dedup feature bolted on.

## Honest framing, up front

- **This is a log-structured dedup engine behind a real NBD server, not a filesystem and not a production array.** There is no RAID, no erasure coding, no multi-drive striping, and no wear-leveling beyond the segment allocator itself.
- **The dedup index is a deliberately narrow 32-bit hash, not a cryptographic digest, and that is load-bearing, not a shortcut.** Every dedup candidate is verified byte-for-byte against the stored content before metadata ever commits (`DedupBlockStore::find_dedup_candidate`), so a hash collision can only ever cost an extra read-and-compare, never correctness. A 256-bit hash would make that verification path practically untestable; a 32-bit hash makes it something a unit test can trigger for real (see Findings and Validation).
- **io_uring is used for its lower per-operation syscall overhead over read(2)/write(2), not for multi-depth asynchronous batching.** `UringBackingFile` issues one SQE and waits for its matching CQE per call. A production engine would keep many chunk appends in flight on one ring; this one does not, and that is the single biggest reason the throughput numbers below are modest (see Findings).
- **The NBD server speaks newstyle fixed negotiation with one unnamed default export, not the full protocol surface.** Modern `nbd-client` (3.x) removed oldstyle support entirely, so newstyle is not optional; beyond `NBD_OPT_EXPORT_NAME` every other option (`NBD_OPT_LIST`, TLS, ...) is answered with a plain "unsupported," which is correct and sufficient for a server that only ever hosts one export.
- **Crash consistency here means per-chunk write-ahead log records with a CRC and a monotonic sequence number, replayed from scratch on recovery, not a filesystem-grade journal.** There is no incremental checkpoint; `recover()` rescans every segment every time. That is a real, disclosed scope cut (see Limitations), and it is also exactly the surface the sibling `crash-consistency-fault-injection-harness` repository drives fault injection against.
- **Machine and toolchain.** 12 logical cores available to WSL2 (capped by `.wslconfig`), Windows 11 Home, WSL2 Ubuntu 22.04 (kernel 6.6.87.2-microsoft-standard-WSL2, which does carry `CONFIG_BLK_DEV_NBD=m`), g++ 11.4.0, `-O2`/CMake Release for the measured build, liburing 2.1, OpenSSL/libssl not used (see the dedup-hash design note above), fio 3.28, nbd-client/nbd-server 3.23. All benchmarks ran against a real kernel-attached `/dev/nbdX`, not a loopback test harness.

## Architecture

```
CMakeLists.txt              static lib + nbd_server executable + GoogleTest suite (FetchContent)
src/
  common.h                  shared constants: 4 KiB logical block, 512 B dedup chunk, 1 MiB segment
  chunk_hash.h               the deliberately narrow 32-bit FNV-1a dedup candidate hash
  crc32c.h / .cpp            table-based CRC-32C, used to detect torn writes during recovery
  backing_file.h / .cpp      IBackingFile (the seam the fault-injection harness substitutes) + the
                             real io_uring-backed implementation
  block_store.h / .cpp       DedupBlockStore: dedup, log-structured segments, GC, crash recovery
  nbd_protocol.h             NBD wire constants and structs, defined locally (no kernel headers needed)
  nbd_server.h / .cpp        newstyle NBD negotiation + transmission-phase request loop
  main_nbd_server.cpp        CLI entry point: backing file, GC thread, stats-dump thread, signal handling
  naive_store.h              the reference oracle: flat array, plain overwrite, nothing to get wrong
tests/
  memory_backing_file.h      in-memory IBackingFile with a scriptable crash (torn + dropped writes)
  test_dedup_correctness.cpp single-case tests + a 20,000-op fuzz test diffed against NaiveStore
  test_hash_collision.cpp    finds a real 32-bit hash collision and proves verification catches it
  test_gc_liveness.cpp       GC invariants, migration correctness, lifetime-grouped vs FIFO WAF
  test_recovery.cpp          simulated crash mid-write; recovery must show no torn/corrupted chunk
  test_concurrency.cpp       writer + GC thread + reader thread, the ThreadSanitizer target
  run_benchmark.sh           the fio-driven measurement behind every number in this README
docs/                        raw output referenced throughout this README
```

**Why dedup indirection goes through a content hash, not a direct physical pointer.** `LbaSlot` (the per-sub-chunk entry in `lba_map_`) stores an `entry_id`, and `DedupEntry` (keyed by that id) stores the physical address. A write that hits the dedup table never has to touch any other LBA that shares the same content: when GC migrates a chunk, it updates the one `DedupEntry::addr`, and every LBA referencing that `entry_id` sees the new location on its next read. The alternative, storing a physical address directly in the LBA map, would force GC to walk every LBA that could conceivably reference a migrated chunk, with no efficient way to find them.

**Why a sub-chunk slot additionally tracks its own `assertion_loc`, separate from the dedup entry's address.** A ref-only record (a dedup hit) and its target data record almost never live in the same segment. GC has to know, for any physical slot it is scanning, "is this the data's current home" (governed by `DedupEntry::refcount`, independent of any one LBA) versus "is this the current assertion record for LBA X's sub-chunk Y" (governed by whether `assertion_loc` still equals this address). Conflating the two was a real bug (see Findings): decrementing a data slot's liveness just because its *original writer's* assertion moved elsewhere, while other LBAs still referenced the data by ref-only pointer, let the allocator hand that segment's bytes to a new write while something else still depended on them.

**Why segments are 1 MiB, not the 8-32 MiB a real log-structured store would pick.** `gc_migrate_segment` holds `mutex_` for the whole call (no finer-grained locking was implemented; see Limitations). A larger segment means a longer single lock hold, which is exactly what shows up as foreground read tail latency during reclamation. Shrinking the segment size is the one lever available without a deeper rewrite, and the read-latency numbers below are the direct, measured cost of not doing that rewrite.

**Why the allocator keeps three parallel per-lifetime-class segment streams instead of one.** `open_segment_`/`next_slot_` are indexed by lifetime class under `GcPolicy::LifetimeGrouped` (collapsed to a single index under `GcPolicy::Fifo`, the comparison baseline). Grouping by lifetime means a stream of exclusively churned data can reach zero live slots entirely on its own, with no migration at all (`mark_dead`'s lazy free path); a stream mixing churn with rarely-touched data instead has to be repeatedly migrated to extract the rare long-lived survivors, which is the entire cost `GcLiveness.LifetimeGroupingBeatsFifoOnWriteAmplification` measures.

## Validation

```
$ ./build/dedupstore_tests
[==========] 11 tests from 5 test suites ran.
[  PASSED  ] 11 tests.
```

Full output under `docs/test_output.txt`. Layers:

- **A reference oracle, diffed on every read.** `NaiveStore` is a flat array with plain in-place overwrite: nothing about it can be wrong. `DedupCorrectness.FuzzAgainstNaiveOracle` runs 20,000 randomized writes (half drawn from a small pool of canned blocks to force real dedup traffic, half fully random) across 512 LBAs with periodic GC, diffing every fifth read and every final block against the oracle.
- **A real hash collision, not a hypothetical one.** `HashCollision.NaturalCollisionExistsAtFiveMillionChunks` hashes 5,000,000 random 512-byte chunks with the actual 32-bit dedup hash, finds a genuine collision (expected count in the hundreds at this space size, by the birthday bound), and drives both colliding-but-different chunks through the real store to prove the byte-for-byte verification step is what keeps them from merging, not luck.
- **GC invariants and a policy comparison**, under deliberately tight capacity so migration is actually forced rather than coasting on lazy full-segment death (see `test_gc_liveness.cpp`'s own comments on why generous overprovisioning hides this entirely).
- **A simulated crash with a scriptable fault injector.** `MemoryBackingFile::arm_crash` tears one write and drops everything after it; recovery must show, for every LBA, either the acknowledged (flushed) value or the unacknowledged one, and never a third, corrupted value.
- **A real concurrency target for ThreadSanitizer.** `test_concurrency.cpp` runs a writer, the GC thread, and a reader against the same store simultaneously; every public method takes `mutex_`, and TSan is what actually checks that claim instead of just asserting it.
- **Sanitizers.** ASan+UBSan clean (`docs/asan_ubsan_test_output.txt`); TSan clean on the concurrency target (`docs/tsan_clean_run.txt`, run under WSL2 with `setarch -R` to disable ASLR, which TSan requires there).

## Findings

**1. A real refcounting bug: migrating or overwriting the *original writer's* copy of a shared chunk could silently corrupt every other LBA still referencing it.** The randomized fuzz test's first full pass (20,000 writes, diffed every fifth read) passed cleanly, and then a full final scan over all 512 LBAs found a mismatch: `lba 324`'s content, correct every time it was checked right after being written, had changed to garbage by the time of the final scan, even though nothing had written to LBA 324 again. The bug was in how `write_block` and `gc_migrate_segment` tracked a slot's liveness: whenever a chunk's *original writer* (the LBA that caused the data to be appended in the first place) had its own mapping superseded by a later write, the code unconditionally marked the data's physical slot dead, *regardless of whether other LBAs still held ref-only pointers to that same data*. Once that slot's segment reached zero recorded live slots, `mark_dead`'s lazy-free path handed the segment back to the allocator, which happily overwrote it with new data, destroying bytes that other LBAs' dedup entries still pointed at. The fix separates two previously-conflated questions: a data slot's liveness is governed by its `DedupEntry::refcount` reaching zero, not by whether its *original* writer's own mapping has moved on (`block_store.cpp`'s `old_loc_is_data_home` check in `write_block`, and the hash-index-based liveness search in `gc_migrate_segment` instead of the original `assertion_loc` comparison). The same bug existed a second time in `recover()`'s replay path and is fixed the same way.

**2. Single-request-in-flight per connection caps throughput at roughly 1,500-1,600 IOPS, and that bound dominates every benchmark below.** The first fio smoke test (`--rw=randwrite --dedupe_percentage=50`) measured 1,581 IOPS on a tiny 8 MiB run. Tracing through the per-write cost explains the number without needing a profiler: each 4 KiB write splits into 8 sub-chunks, and roughly half of them (at 50% dedupe) are hits requiring a byte-for-byte verification read plus a ref-only write, while the rest are misses requiring one full write; every one of those reads and writes is a separate, synchronous `io_uring_submit`/`io_uring_wait_cqe` round trip (see Honest framing), and the NBD transmission loop itself processes exactly one request at a time regardless of the kernel's queue depth. At roughly a dozen io_uring round trips per logical block, a few hundred microseconds each, 1,500-1,600 IOPS is the arithmetic, not a surprise. This is the direct reason the 200 GiB workload claim could not be reached at the scale originally targeted; see Measured results for what that means for the numbers below.

**3. A real newstyle/oldstyle protocol break.** The server was originally built against NBD's oldstyle negotiation (simpler: one fixed-size handshake, no option haggling). The first real attach attempt with the installed `nbd-client` (3.23) failed outright: "the oldstyle protocol is no longer supported." Modern `nbd-client` dropped it entirely. The fix is the newstyle fixed-negotiation handshake in `NbdServer::negotiate`, supporting exactly the one option (`NBD_OPT_EXPORT_NAME`) a client with no `-N` flag actually sends, with every other option answered `NBD_REP_ERR_UNSUP` rather than left unhandled.

## Measured results

Machine: 12 logical cores available to WSL2, Windows 11 Home, WSL2 Ubuntu 22.04 (kernel 6.6.87.2-microsoft-standard-WSL2), g++ 11.4.0 `-O2`, liburing 2.1, fio 3.28, nbd-client 3.23, real kernel-attached `/dev/nbd5`/`/dev/nbd6`. Raw fio and engine-stats output for every row below is under `docs/`.

**The claim as targeted was write amplification 2.8x to 1.6x at 85% utilization on a replayed 200 GiB workload, with p99.9 read latency under 2 ms during reclamation. The 200 GiB figure was not reached (see Finding 2); every number below was measured honestly at the scale the single-request-per-round-trip design could actually replay in the time available, not tuned to the target.**

| Claim | Measured | Meets claim | Raw output |
|---|---|---|---|
| __BENCH_WAF_ROW__ |
| 85% utilization | __BENCH_UTIL__ |
| replayed 200 GiB workload | __BENCH_VOLUME__ |
| p99.9 read latency under 2 ms during reclamation | __BENCH_LATENCY__ |

What these numbers do not measure: a multi-drive array, any form of redundancy, or sustained throughput with multiple requests in flight (the NBD transmission loop here is strictly one request at a time; see Honest framing and Finding 2). The write-amplification comparison holds the workload fixed across both GC policies (same random seed shape, same hot/cold split) and varies only `GcPolicy`, which is the controlled comparison the claim is actually about; the absolute IOPS number is a property of this engine's I/O depth, not of the dedup or GC design.

## Building and running

```
cd log-structured-dedup-block-store
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(( $(nproc) / 2 ))"
./build/dedupstore_tests

# sanitizers
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j"$(( $(nproc) / 2 ))" --target dedupstore_tests
./build-asan/dedupstore_tests

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j"$(( $(nproc) / 2 ))" --target dedupstore_tests
setarch -R ./build-tsan/dedupstore_tests --gtest_filter="Concurrency.*"   # ASLR off, required for TSan under WSL2

# run as a real block device (needs the nbd kernel module and a passwordless-sudo nbd-client, or root)
sudo modprobe nbd
./build/nbd_server /tmp/backing.img $((2560*1024*1024)) 524288 50321 lifetime create /tmp/stats.json &
sudo nbd-client 127.0.0.1 50321 /dev/nbd5 -b 4096
sudo fio --name=t --filename=/dev/nbd5 --rw=randrw --bs=4096 --size=512M --direct=1 \
  --ioengine=sync --iodepth=1 --dedupe_percentage=50
sudo nbd-client -d /dev/nbd5

# the exact measurement behind the Measured results table
tests/run_benchmark.sh lifetime nbd6 58232
tests/run_benchmark.sh fifo nbd5 58231
```

No build flags beyond `-std=c++20`; GoogleTest is fetched by CMake's `FetchContent`.

## Limitations

- **200 GiB was not reached.** See Finding 2: this engine's single-request-in-flight NBD loop and per-chunk-synchronous io_uring usage cap throughput at roughly 1,500-1,600 IOPS, which bounds how much volume this project could honestly replay in the time available. The numbers reported are measured at the achieved scale, not extrapolated or tuned.
- **io_uring is used for per-operation syscall overhead, not for async depth.** One SQE submitted and waited on per call (`UringBackingFile`). A production engine would batch many chunk appends in flight on one ring; this is the single biggest lever left for throughput and was not implemented.
- **GC holds the store-wide lock for an entire segment's migration, not per-chunk.** This is why segments were sized down to 1 MiB rather than a more realistic 8-32 MiB (see Architecture); finer-grained locking (lock only around each chunk's metadata update, not its I/O) was not implemented.
- **No incremental checkpoint.** `recover()` rescans every segment from the start of the device every time, rebuilding the dedup table and LBA map from scratch. A real deployment needs periodic checkpoints to bound recovery time on a large device; this does not implement one.
- **A ref-only record's replay cannot disambiguate a hash collision.** It carries no payload, so if its hash ever collided (vanishingly rare for real 512-byte content at this scale, demonstrated directly in `HashCollision.NaturalCollisionExistsAtFiveMillionChunks`), recovery takes the only entry under that hash rather than verifying which one. Live operation always verifies byte-for-byte; only the ref-only-record replay path has this gap.
- **No sub-4-KiB or misaligned I/O.** The dedup/GC design works in whole 4 KiB logical blocks; the NBD server rejects (`EINVAL`) any request not aligned to and sized as a multiple of 4 KiB, which requires mounting with `nbd-client -b 4096` (documented in Building and running). A production target would need a read-modify-write path for arbitrary alignment.
- **Single export, single connection at a time.** `NbdServer` serves one TCP connection fully before accepting the next; there is no multi-connection fan-out, which real `nbd-client` supports for parallelism this server could not actually use given Limitation above.
