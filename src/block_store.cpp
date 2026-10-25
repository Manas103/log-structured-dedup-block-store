#include "block_store.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "chunk_hash.h"
#include "crc32c.h"

namespace lsd {

DedupBlockStore::DedupBlockStore(IBackingFile& backing, uint32_t num_logical_blocks, GcPolicy gc_policy,
                                  double gc_reserve_fraction)
    : backing_(backing), num_blocks_(num_logical_blocks), gc_policy_(gc_policy), gc_reserve_fraction_(gc_reserve_fraction) {
    slots_per_segment_ = static_cast<uint32_t>(kSegmentSize / kRecordSlotBytes);
    uint64_t num_segments = backing_.size() / kSegmentSize;
    if (num_segments < kLifetimeClassCount + 2) {
        throw std::runtime_error("DedupBlockStore: backing file too small for a usable segment count");
    }
    segments_.assign(num_segments, SegmentMeta{});
    reserve_segments_ = static_cast<uint32_t>(static_cast<double>(num_segments) * gc_reserve_fraction_);
    if (reserve_segments_ < kLifetimeClassCount + 1) reserve_segments_ = kLifetimeClassCount + 1;
    lba_map_.assign(num_blocks_, std::array<LbaSlot, kChunksPerBlock>{});
    open_segment_.fill(kInvalidSeg);
}

// ---------------------------------------------------------------- records --

DedupBlockStore::ParsedRecord DedupBlockStore::read_record(PhysicalAddr a) const {
    ParsedRecord r;
    uint8_t hdr_buf[sizeof(RecordHeader)];
    backing_.read(addr_offset(a), hdr_buf, sizeof(hdr_buf));
    std::memcpy(&r.header, hdr_buf, sizeof(RecordHeader));
    if (r.header.magic != 0x4C534431u) return r;
    if (r.header.kind > 1) return r;

    uint32_t expected_crc;
    if (r.header.kind == 0) {
        backing_.read(addr_offset(a) + sizeof(RecordHeader), r.payload.data(), kChunkSize);
        uint8_t buf[sizeof(RecordHeader) + kChunkSize];
        RecordHeader zeroed = r.header;
        zeroed.crc = 0;
        std::memcpy(buf, &zeroed, sizeof(RecordHeader));
        std::memcpy(buf + sizeof(RecordHeader), r.payload.data(), kChunkSize);
        expected_crc = crc32c(buf, sizeof(buf));
    } else {
        RecordHeader zeroed = r.header;
        zeroed.crc = 0;
        expected_crc = crc32c(reinterpret_cast<uint8_t*>(&zeroed), sizeof(RecordHeader));
    }
    r.valid = (expected_crc == r.header.crc);
    return r;
}

PhysicalAddr DedupBlockStore::write_data_record(uint32_t lba, uint8_t slot_in_block, LifetimeClass cls,
                                                 uint32_t hash, const uint8_t* chunk) {
    PhysicalAddr a = allocate_slot(cls);
    RecordHeader hdr;
    hdr.seq = next_seq_++;
    hdr.lba = lba;
    hdr.slot_in_block = slot_in_block;
    hdr.lifetime = static_cast<uint8_t>(cls);
    hdr.kind = 0;
    hdr.hash = hash;
    hdr.epoch = segments_[a.segment_id].epoch;
    hdr.crc = 0;
    uint8_t buf[sizeof(RecordHeader) + kChunkSize];
    std::memcpy(buf, &hdr, sizeof(hdr));
    std::memcpy(buf + sizeof(hdr), chunk, kChunkSize);
    reinterpret_cast<RecordHeader*>(buf)->crc = crc32c(buf, sizeof(buf));
    backing_.write(addr_offset(a), buf, sizeof(buf));
    stats_.media_bytes_written += sizeof(buf);
    return a;
}

PhysicalAddr DedupBlockStore::write_ref_record(uint32_t lba, uint8_t slot_in_block, LifetimeClass cls,
                                                uint32_t hash) {
    PhysicalAddr a = allocate_slot(cls);
    RecordHeader hdr;
    hdr.seq = next_seq_++;
    hdr.lba = lba;
    hdr.slot_in_block = slot_in_block;
    hdr.lifetime = static_cast<uint8_t>(cls);
    hdr.kind = 1;
    hdr.hash = hash;
    hdr.epoch = segments_[a.segment_id].epoch;
    hdr.crc = 0;
    uint8_t buf[sizeof(RecordHeader)];
    std::memcpy(buf, &hdr, sizeof(hdr));
    reinterpret_cast<RecordHeader*>(buf)->crc = crc32c(buf, sizeof(buf));
    backing_.write(addr_offset(a), buf, sizeof(buf));
    stats_.media_bytes_written += sizeof(buf);
    return a;
}

// ------------------------------------------------------------- allocation --

void DedupBlockStore::open_new_segment(LifetimeClass cls) {
    size_t stream = (gc_policy_ == GcPolicy::Fifo) ? 0 : static_cast<size_t>(cls);
    if (open_segment_[stream] != kInvalidSeg) segments_[open_segment_[stream]].open = false;

    uint32_t chosen = kInvalidSeg;
    for (uint32_t i = 0; i < segments_.size(); ++i) {
        if (segments_[i].free) {
            chosen = i;
            break;
        }
    }
    if (chosen == kInvalidSeg) {
        throw std::runtime_error("DedupBlockStore: no free segment (GC not keeping up with write rate)");
    }
    SegmentMeta& seg = segments_[chosen];
    uint16_t next_epoch = static_cast<uint16_t>(seg.epoch + 1);
    seg = SegmentMeta{};
    seg.lifetime_class = cls;
    seg.epoch = next_epoch;
    seg.open = true;
    seg.free = false;
    open_segment_[stream] = chosen;
    next_slot_[stream] = 0;
}

PhysicalAddr DedupBlockStore::allocate_slot(LifetimeClass cls) {
    size_t stream = (gc_policy_ == GcPolicy::Fifo) ? 0 : static_cast<size_t>(cls);
    if (open_segment_[stream] == kInvalidSeg || !segments_[open_segment_[stream]].open ||
        next_slot_[stream] >= slots_per_segment_) {
        open_new_segment(cls);
    }
    PhysicalAddr a{open_segment_[stream], next_slot_[stream]++};
    segments_[a.segment_id].used_slots++;
    return a;
}

void DedupBlockStore::mark_dead(PhysicalAddr a) {
    if (!a.valid()) return;
    SegmentMeta& seg = segments_[a.segment_id];
    if (seg.live_slots > 0) seg.live_slots--;
    if (!seg.open && !seg.free && seg.live_slots == 0) {
        seg.free = true;
    }
}

// -------------------------------------------------------------- dedup path --

uint64_t DedupBlockStore::find_dedup_candidate(uint32_t hash, const uint8_t* chunk) const {
    auto it = hash_index_.find(hash);
    if (it == hash_index_.end()) return 0;
    for (uint64_t id : it->second) {
        auto eit = entries_.find(id);
        if (eit == entries_.end()) continue;
        ChunkBuffer existing{};
        backing_.read(addr_offset(eit->second.addr) + sizeof(RecordHeader), existing.data(), kChunkSize);
        if (std::memcmp(existing.data(), chunk, kChunkSize) == 0) return id;
    }
    return 0;
}

void DedupBlockStore::write_block(uint32_t lba, const uint8_t* data, LifetimeClass hint, bool fua) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lba >= num_blocks_) throw std::out_of_range("DedupBlockStore::write_block: lba out of range");

    auto& slots = lba_map_[lba];
    for (uint8_t s = 0; s < kChunksPerBlock; ++s) {
        const uint8_t* chunk = data + static_cast<size_t>(s) * kChunkSize;
        uint32_t h = chunk_hash(chunk, kChunkSize);
        uint64_t old_id = slots[s].entry_id;
        PhysicalAddr old_loc = slots[s].assertion_loc;

        uint64_t match_id = find_dedup_candidate(h, chunk);
        if (match_id != 0 && match_id == old_id) {
            // Idempotent rewrite of identical content already in place: no
            // new record needed, nothing changes on media.
            stats_.dedup_hits++;
            continue;
        }

        PhysicalAddr new_loc;
        if (match_id != 0) {
            stats_.dedup_hits++;
            entries_[match_id].refcount++;
            new_loc = write_ref_record(lba, s, hint, h);
            slots[s] = LbaSlot{match_id, new_loc};
        } else {
            stats_.dedup_misses++;
            uint64_t new_id = next_entry_id_++;
            new_loc = write_data_record(lba, s, hint, h, chunk);
            entries_[new_id] = DedupEntry{h, new_loc, 1};
            hash_index_[h].push_back(new_id);
            slots[s] = LbaSlot{new_id, new_loc};
        }
        segments_[new_loc.segment_id].live_slots++;

        if (old_loc.valid()) {
            // old_loc is the data's own home exactly when this LBA slot was
            // that chunk's original writer (not a dedup hit riding on
            // someone else's data). In that case old_loc's liveness is
            // governed by the entry's refcount, not by this one assertion
            // moving: other LBA slots can still hold ref-only records
            // pointing at the same entry_id, and freeing old_loc here
            // regardless of refcount would let the allocator hand that
            // segment's space to a new write while those other LBAs still
            // depend on the bytes in it, corrupting them. This was a real
            // bug (see README Findings): it surfaced as other, unrelated
            // LBAs reading back wrong data long after their own last write.
            bool old_loc_is_data_home = (old_id != 0) && entries_.count(old_id) && (entries_[old_id].addr == old_loc);
            if (!old_loc_is_data_home) mark_dead(old_loc);

            if (old_id != 0 && old_id != match_id) {
                auto eit = entries_.find(old_id);
                if (eit != entries_.end()) {
                    eit->second.refcount--;
                    if (eit->second.refcount == 0) {
                        mark_dead(eit->second.addr);
                        auto& bucket = hash_index_[eit->second.hash];
                        bucket.erase(std::remove(bucket.begin(), bucket.end(), old_id), bucket.end());
                        entries_.erase(eit);
                    }
                }
            }
        }
    }
    stats_.host_bytes_written += kLogicalBlockSize;
    maybe_run_gc_locked();
    if (fua) backing_.flush();
}

void DedupBlockStore::read_block(uint32_t lba, uint8_t* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lba >= num_blocks_) throw std::out_of_range("DedupBlockStore::read_block: lba out of range");
    const auto& slots = lba_map_[lba];
    for (uint8_t s = 0; s < kChunksPerBlock; ++s) {
        uint8_t* dst = out + static_cast<size_t>(s) * kChunkSize;
        if (slots[s].entry_id == 0) {
            std::memset(dst, 0, kChunkSize);
            continue;
        }
        const DedupEntry& e = entries_.at(slots[s].entry_id);
        backing_.read(addr_offset(e.addr) + sizeof(RecordHeader), dst, kChunkSize);
    }
}

void DedupBlockStore::flush() { backing_.flush(); }

// ------------------------------------------------------------------- gc ---

uint32_t DedupBlockStore::pick_gc_victim() const {
    uint32_t best = kInvalidSeg;
    double best_score = 2.0;  // liveness fraction is in [0,1]; Fifo ignores this
    for (uint32_t i = 0; i < segments_.size(); ++i) {
        const SegmentMeta& seg = segments_[i];
        if (seg.free || seg.open) continue;
        if (gc_policy_ == GcPolicy::Fifo) {
            // Oldest sealed segment by id, ignoring liveness and lifetime
            // grouping entirely: the naive baseline this project compares
            // against.
            if (best == kInvalidSeg || i < best) best = i;
            continue;
        }
        double liveness = seg.used_slots == 0 ? 0.0 : static_cast<double>(seg.live_slots) / seg.used_slots;
        if (liveness < best_score) {
            best_score = liveness;
            best = i;
        }
    }
    return best;
}

void DedupBlockStore::gc_migrate_segment(uint32_t segment_id) {
    SegmentMeta& victim = segments_[segment_id];
    LifetimeClass cls = victim.lifetime_class;
    uint32_t used = victim.used_slots;
    for (uint32_t s = 0; s < used; ++s) {
        PhysicalAddr a{segment_id, s};
        ParsedRecord rec = read_record(a);
        if (!rec.valid) continue;  // should not happen for sealed data written by this process

        // RecordHeader is #pragma pack(1) to match the on-disk layout, so
        // its fields are not necessarily naturally aligned once embedded
        // in another struct. Copy them out to plain locals before binding
        // any reference to them (an unordered_map lookup takes its key by
        // const reference): binding a reference to the packed field
        // directly is undefined behavior UBSan's alignment check catches.
        uint32_t hash = rec.header.hash;
        uint32_t lba = rec.header.lba;
        uint8_t slot_in_block = rec.header.slot_in_block;
        uint8_t kind = rec.header.kind;

        if (kind == 0) {
            // A data record's liveness is whether any dedup entry still
            // calls this address home, not whether the (lba, slot)
            // mapping that originally wrote it is still current: other
            // LBAs can hold ref-only records pointing at this same entry
            // (see write_block's old_loc_is_data_home comment for the bug
            // that ignoring this caused).
            uint64_t id = 0;
            auto it = hash_index_.find(hash);
            if (it != hash_index_.end()) {
                for (uint64_t cand : it->second) {
                    auto eit = entries_.find(cand);
                    if (eit != entries_.end() && eit->second.addr == a) {
                        id = cand;
                        break;
                    }
                }
            }
            if (id == 0) continue;  // no entry still points here: genuinely dead

            PhysicalAddr new_loc = write_data_record(lba, slot_in_block, cls, hash, rec.payload.data());
            entries_[id].addr = new_loc;
            segments_[new_loc.segment_id].live_slots++;
            stats_.gc_bytes_migrated += sizeof(RecordHeader) + kChunkSize;

            // If the original writer's own mapping still points directly
            // at this slot (it was never superseded by a later write to
            // that same lba/slot), fold its re-assertion into this same
            // move instead of leaving it dangling.
            LbaSlot& slot = lba_map_[lba][slot_in_block];
            if (slot.assertion_loc == a) slot.assertion_loc = new_loc;
        } else {
            LbaSlot& slot = lba_map_[lba][slot_in_block];
            if (!(slot.assertion_loc == a)) continue;  // superseded ref-only assertion: already dead
            PhysicalAddr new_loc = write_ref_record(lba, slot_in_block, cls, hash);
            slot.assertion_loc = new_loc;
            segments_[new_loc.segment_id].live_slots++;
            stats_.gc_bytes_migrated += sizeof(RecordHeader);
        }
    }
    victim.free = true;
    victim.used_slots = 0;
    victim.live_slots = 0;
}

void DedupBlockStore::maybe_run_gc_locked() {
    uint64_t free_count = 0;
    for (const auto& s : segments_) if (s.free) free_count++;
    uint32_t guard = 0;
    while (free_count < reserve_segments_ && guard < segments_.size()) {
        uint32_t victim = pick_gc_victim();
        if (victim == kInvalidSeg) break;
        gc_migrate_segment(victim);
        stats_.gc_runs++;
        free_count = 0;
        for (const auto& s : segments_) if (s.free) free_count++;
        guard++;
    }
}

void DedupBlockStore::run_gc() {
    std::lock_guard<std::mutex> lock(mutex_);
    maybe_run_gc_locked();
}

// -------------------------------------------------------------- recovery --

uint64_t DedupBlockStore::recover() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(segments_.begin(), segments_.end(), SegmentMeta{});
    lba_map_.assign(num_blocks_, std::array<LbaSlot, kChunksPerBlock>{});
    entries_.clear();
    hash_index_.clear();
    next_entry_id_ = 1;
    next_seq_ = 1;

    struct Found {
        PhysicalAddr addr;
        RecordHeader header;
        ChunkBuffer payload;
    };
    std::vector<Found> found;

    for (uint32_t seg = 0; seg < segments_.size(); ++seg) {
        uint16_t scan_epoch = 0;
        bool have_epoch = false;
        for (uint32_t slot = 0; slot < slots_per_segment_; ++slot) {
            PhysicalAddr a{seg, slot};
            ParsedRecord rec = read_record(a);
            if (!rec.valid) break;  // torn write or truly unwritten tail: stop this segment
            if (!have_epoch) {
                scan_epoch = rec.header.epoch;
                have_epoch = true;
            } else if (rec.header.epoch != scan_epoch) {
                break;  // stale leftover from this segment's previous life
            }
            segments_[seg].used_slots = slot + 1;
            segments_[seg].epoch = scan_epoch;
            segments_[seg].lifetime_class = static_cast<LifetimeClass>(rec.header.lifetime);
            found.push_back(Found{a, rec.header, rec.payload});
        }
    }

    std::sort(found.begin(), found.end(), [](const Found& x, const Found& y) { return x.header.seq < y.header.seq; });

    // Replaying every record in seq order without also undoing the
    // reference an overwritten (lba, slot) used to hold would leave every
    // superseded entry's refcount permanently inflated: harmless for what
    // a read returns (reads always follow the slot's current entry_id),
    // but it would make the post-recovery live_slots accounting below
    // overcount, so a segment holding only genuinely dead data could
    // never be reclaimed again after a restart. This mirrors write_block's
    // own decref path exactly.
    uint64_t applied = 0;
    for (const auto& f : found) {
        // See gc_migrate_segment for why packed header fields are copied
        // to locals before anything binds a reference to them.
        uint32_t hash = f.header.hash;
        uint32_t lba = f.header.lba;
        uint8_t slot_in_block = f.header.slot_in_block;
        uint8_t kind = f.header.kind;

        next_seq_ = std::max<uint64_t>(next_seq_, f.header.seq + 1);
        LbaSlot& slot = lba_map_[lba][slot_in_block];
        uint64_t old_id = slot.entry_id;

        uint64_t new_id = 0;
        if (kind == 0) {
            new_id = next_entry_id_++;
            entries_[new_id] = DedupEntry{hash, f.addr, 1};
            hash_index_[hash].push_back(new_id);
        } else {
            // A ref-only record carries no payload, so replay cannot
            // byte-for-byte disambiguate which same-hash entry it pointed
            // to if that hash ever collided; it takes the only (almost
            // always the only) entry under that hash. See README
            // Limitations: this is a real, disclosed gap, not an oversight.
            auto it = hash_index_.find(hash);
            if (it == hash_index_.end() || it->second.empty()) continue;
            new_id = it->second.front();
            if (new_id != old_id) entries_[new_id].refcount++;
        }
        slot = LbaSlot{new_id, f.addr};

        if (old_id != 0 && old_id != new_id) {
            auto eit = entries_.find(old_id);
            if (eit != entries_.end()) {
                eit->second.refcount--;
                if (eit->second.refcount == 0) {
                    auto& bucket = hash_index_[eit->second.hash];
                    bucket.erase(std::remove(bucket.begin(), bucket.end(), old_id), bucket.end());
                    entries_.erase(eit);
                }
            }
        }
        applied++;
    }

    // live_slots has two disjoint sources, matching how write_block and
    // gc_migrate_segment maintain it during normal operation: one credit
    // per still-referenced entry at its own data address, plus one credit
    // per ref-only record whose (lba, slot) mapping is still current.
    // Crediting every current lba_map_ assertion at its own address
    // instead (the simpler-looking version) would undercount a data
    // record shared by other LBAs' ref-only pointers once its original
    // writer's own mapping moves on.
    for (const auto& [id, entry] : entries_) {
        if (entry.refcount > 0) segments_[entry.addr.segment_id].live_slots++;
    }
    for (const auto& lba_slots : lba_map_) {
        for (const auto& s : lba_slots) {
            if (s.entry_id == 0) continue;
            auto it = entries_.find(s.entry_id);
            if (it != entries_.end() && !(s.assertion_loc == it->second.addr)) {
                segments_[s.assertion_loc.segment_id].live_slots++;
            }
        }
    }
    for (uint32_t seg = 0; seg < segments_.size(); ++seg) {
        if (segments_[seg].used_slots == 0) segments_[seg] = SegmentMeta{};
    }
    open_segment_.fill(kInvalidSeg);
    return applied;
}

Stats DedupBlockStore::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s = stats_;
    s.segments_total = segments_.size();
    s.segments_free = 0;
    for (const auto& seg : segments_) if (seg.free) s.segments_free++;
    return s;
}

}  // namespace lsd
