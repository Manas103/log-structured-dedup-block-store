#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "backing_file.h"
#include "common.h"

namespace lsd {

struct Stats {
    uint64_t host_bytes_written = 0;   // bytes the caller asked to write (post-dedup accounting: every
                                        // write() call counts kLogicalBlockSize, dedup or not)
    uint64_t media_bytes_written = 0;  // bytes actually appended to the backing file: original writes
                                        // (misses only) plus every byte a GC pass rewrites
    uint64_t gc_runs = 0;
    uint64_t gc_bytes_migrated = 0;
    uint64_t segments_total = 0;
    uint64_t segments_free = 0;
    uint64_t dedup_hits = 0;
    uint64_t dedup_misses = 0;

    double write_amplification() const {
        return host_bytes_written == 0 ? 1.0 : static_cast<double>(media_bytes_written) / static_cast<double>(host_bytes_written);
    }
    double utilization() const {
        return segments_total == 0 ? 0.0 : 1.0 - static_cast<double>(segments_free) / static_cast<double>(segments_total);
    }
};

// A record header precedes every chunk written to the log. `kind == 0` is a
// data record (kChunkSize payload bytes of real content follow); `kind == 1`
// is a reference-only record (a dedup hit: no payload, metadata only).
// `crc` covers the header (with crc zeroed) and the payload, and is how
// recovery detects a torn write: a record truncated or corrupted by a
// simulated power loss fails this check and recovery stops applying that
// stream right there.
#pragma pack(push, 1)
struct RecordHeader {
    uint32_t magic = 0x4C534431u;  // 'LSD1'
    uint64_t seq = 0;
    uint32_t lba = 0;
    uint8_t slot_in_block = 0;
    uint8_t lifetime = 0;
    uint8_t kind = 0;  // 0 = data, 1 = ref-only
    uint8_t reserved = 0;
    uint32_t hash = 0;
    // A segment reused by the allocator after GC frees it still has its
    // previous life's bytes sitting past the new high-water mark; nothing
    // persists that high-water mark, so recovery cannot tell "end of
    // current log" from "start of stale leftover data" by validity alone,
    // because a stale record is still a perfectly valid record. `epoch`
    // (bumped every time a segment transitions free -> open) breaks the
    // tie: a scan stops at the first record whose epoch differs from the
    // first valid record's epoch in that segment, not just the first
    // invalid one. See DedupBlockStore::recover.
    uint16_t epoch = 0;
    uint32_t crc = 0;
};
#pragma pack(pop)
static_assert(sizeof(RecordHeader) == 30, "RecordHeader must stay fixed-size for on-disk compatibility");

// One record (header, plus a kChunkSize payload for data records) occupies
// exactly one addressable slot in a segment.
constexpr size_t kRecordSlotBytes = sizeof(RecordHeader) + kChunkSize;

struct SegmentMeta {
    LifetimeClass lifetime_class = LifetimeClass::Hot;
    uint32_t used_slots = 0;  // slots ever written in this segment (the high-water mark)
    uint32_t live_slots = 0;  // slots still referenced by the dedup table
    uint16_t epoch = 0;       // bumped each time this segment goes free -> open
    bool open = false;        // currently the active append target for its lifetime class
    bool free = true;         // available for reuse by the allocator
};

struct DedupEntry {
    uint32_t hash = 0;
    PhysicalAddr addr;
    uint32_t refcount = 0;
};

// Where the most recent record asserting a given (lba, sub-chunk slot)
// mapping physically lives, plus which dedup entry it currently resolves
// to. GC re-asserts this forward (a new record at a new address) without
// needing to touch every other LBA that might share the same dedup entry.
struct LbaSlot {
    uint64_t entry_id = 0;  // 0 = never written (reads as zero)
    PhysicalAddr assertion_loc;
};

// The log-structured, deduplicating, host-managed-reclamation block store.
//
// Logical blocks are kLogicalBlockSize (4 KiB); each is deduplicated at
// kChunkSize (512 B) granularity, matching Purity's published dedup
// granularity. A write splits its 4 KiB payload into 8 chunks, hashes each,
// and either references an existing chunk (after a byte-for-byte compare)
// or appends a new one to the open segment for its lifetime class.
class DedupBlockStore {
public:
    DedupBlockStore(IBackingFile& backing, uint32_t num_logical_blocks, GcPolicy gc_policy,
                     double gc_reserve_fraction = 0.15);

    // Writes one 4 KiB logical block (kLogicalBlockSize bytes at `data`).
    // `hint` tags every chunk in this write with a lifetime class so the
    // allocator can group like-lifetime chunks into the same segments
    // (the DirectFlash premise: data placed by expected lifetime frees
    // whole segments without rewriting live data). `fua` forces an
    // immediate flush of just this write (NBD_CMD_WRITE with the FUA
    // flag), matching the real protocol's per-write durability option.
    void write_block(uint32_t lba, const uint8_t* data, LifetimeClass hint, bool fua = false);
    void read_block(uint32_t lba, uint8_t* out) const;

    // Durability barrier. Every write issued before a flush() that returns
    // is acknowledged-durable; see crc32c.h and RecordHeader for how
    // recovery enforces that only a valid prefix of such writes survives
    // a simulated crash.
    void flush();

    // Rebuilds the dedup table and LBA map by replaying every valid record
    // in the backing file from scratch, in sequence order, stopping a given
    // lifetime stream at its first invalid (torn) record. Returns the
    // number of records successfully applied.
    uint64_t recover();

    // Runs garbage collection until free segments are restored above the
    // reserve fraction, or there is nothing left to reclaim. Safe to call
    // concurrently with write_block/read_block from another thread (both
    // paths take `mutex_`).
    void run_gc();

    Stats stats() const;
    std::mutex& mutex() { return mutex_; }

private:
    // entry_id of the dedup entry whose content byte-for-byte matches
    // `chunk`, or 0 if `hash` is unseen or every same-hash entry's content
    // differs (a real hash collision; see chunk_hash.h).
    uint64_t find_dedup_candidate(uint32_t hash, const uint8_t* chunk) const;
    PhysicalAddr allocate_slot(LifetimeClass cls);
    PhysicalAddr write_data_record(uint32_t lba, uint8_t slot_in_block, LifetimeClass cls, uint32_t hash,
                                    const uint8_t* chunk);
    PhysicalAddr write_ref_record(uint32_t lba, uint8_t slot_in_block, LifetimeClass cls, uint32_t hash);
    void open_new_segment(LifetimeClass cls);
    void mark_dead(PhysicalAddr a);
    void gc_migrate_segment(uint32_t segment_id);
    uint32_t pick_gc_victim() const;
    void maybe_run_gc_locked();

    // Parses and validates one on-disk record. The payload region is
    // always fetched because the CRC covers it for data records; ref-only
    // records leave `payload` unset.
    struct ParsedRecord {
        bool valid = false;
        RecordHeader header;
        ChunkBuffer payload;
    };
    ParsedRecord read_record(PhysicalAddr a) const;
    uint64_t addr_offset(PhysicalAddr a) const {
        return static_cast<uint64_t>(a.segment_id) * kSegmentSize + static_cast<uint64_t>(a.slot) * kRecordSlotBytes;
    }

    IBackingFile& backing_;
    uint32_t num_blocks_;
    GcPolicy gc_policy_;
    double gc_reserve_fraction_;
    uint32_t slots_per_segment_;
    uint32_t reserve_segments_;

    mutable std::mutex mutex_;
    uint64_t next_seq_ = 1;
    uint64_t next_entry_id_ = 1;
    std::vector<std::array<LbaSlot, kChunksPerBlock>> lba_map_;
    std::unordered_map<uint64_t, DedupEntry> entries_;                // entry_id -> canonical entry
    std::unordered_map<uint32_t, std::vector<uint64_t>> hash_index_;  // hash -> candidate entry_ids
    std::vector<SegmentMeta> segments_;
    // Per-lifetime-class open segment + next free slot within it. Under
    // GcPolicy::Fifo there is a single stream (index 0 used for all classes)
    // so GC cannot group by lifetime and must pick its victim by age alone.
    static constexpr uint32_t kInvalidSeg = UINT32_MAX;
    std::array<uint32_t, kLifetimeClassCount> open_segment_;
    std::array<uint32_t, kLifetimeClassCount> next_slot_{};

    mutable Stats stats_;
};

}  // namespace lsd
