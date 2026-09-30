/*
 * SpaceSpec — Shadow Cache Implementation
 *
 * See shadow_cache.hh for full design rationale.
 */

#include "mem/cache/shadow_cache.hh"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/SpaceSpec.hh"

namespace gem5
{

// ========================================================================= //
//  Constructor                                                               //
// ========================================================================= //

ShadowCache::ShadowCache(const Params &p)
    : SimObject(p),
      lineSize_(p.line_size),
      entriesPerThread_(p.num_entries),
      numThreads_(p.num_threads),
      threadQueues_(p.num_threads),
      stats(this)
{
    fatal_if(lineSize_ == 0 || (lineSize_ & (lineSize_ - 1)) != 0,
             "ShadowCache: line_size must be a non-zero power of two, got %zu",
             lineSize_);
    fatal_if(entriesPerThread_ == 0,
             "ShadowCache: num_entries must be > 0");
    fatal_if(numThreads_ == 0,
             "ShadowCache: num_threads must be > 0");

    DPRINTF(SpaceSpec,
            "ShadowCache created: %zu entries/thread, %zu threads, "
            "%zu-byte lines (WFC policy)\n",
            entriesPerThread_, numThreads_, lineSize_);
}

// ========================================================================= //
//  Statistics constructor                                                    //
// ========================================================================= //

ShadowCache::ShadowCacheStats::ShadowCacheStats(ShadowCache *sc)
    : statistics::Group(sc),
      ADD_STAT(inserts, statistics::units::Count::get(),
               "Speculative cache lines inserted into shadow"),
      ADD_STAT(promotions, statistics::units::Count::get(),
               "Shadow lines promoted to L1 at commit (WFC)"),
      ADD_STAT(squashedEntries, statistics::units::Count::get(),
               "Shadow lines discarded on squash (no L1 trace)"),
      ADD_STAT(lookupHits, statistics::units::Count::get(),
               "Speculative loads served from shadow (L1 untouched)"),
      ADD_STAT(lookupMisses, statistics::units::Count::get(),
               "Speculative loads that missed shadow (went to memory)"),
      ADD_STAT(stallsFull, statistics::units::Count::get(),
               "Loads stalled because shadow buffer was full "
               "(TSA-safe stall, not eviction)"),
      ADD_STAT(peakOccupancy, statistics::units::Count::get(),
               "Peak simultaneous valid shadow entries across all threads")
{
}

// ========================================================================= //
//  Private helpers                                                           //
// ========================================================================= //

ShadowCacheEntry *
ShadowCache::findEntry(Addr lineAddr, ThreadID tid)
{
    assert(tid >= 0 && static_cast<size_t>(tid) < numThreads_);
    for (auto &e : threadQueues_[tid]) {
        if (e.valid && e.tag == lineAddr)
            return &e;
    }
    return nullptr;
}

const ShadowCacheEntry *
ShadowCache::findEntry(Addr lineAddr, ThreadID tid) const
{
    assert(tid >= 0 && static_cast<size_t>(tid) < numThreads_);
    for (const auto &e : threadQueues_[tid]) {
        if (e.valid && e.tag == lineAddr)
            return &e;
    }
    return nullptr;
}

size_t
ShadowCache::occupancy(ThreadID tid) const
{
    assert(tid >= 0 && static_cast<size_t>(tid) < numThreads_);
    // All entries in the deque are valid by construction
    // (we only push valid entries and pop or invalidate on squash/promote).
    return threadQueues_[tid].size();
}

// ========================================================================= //
//  Public interface                                                          //
// ========================================================================= //

bool
ShadowCache::isFull(ThreadID tid) const
{
    assert(tid >= 0 && static_cast<size_t>(tid) < numThreads_);
    return occupancy(tid) >= entriesPerThread_;
}

size_t
ShadowCache::totalOccupancy() const
{
    size_t total = 0;
    for (const auto &q : threadQueues_)
        total += q.size();
    return total;
}

// ------------------------------------------------------------------------- //

bool
ShadowCache::insert(InstSeqNum seqNum, ThreadID tid,
                    Addr lineAddr, const uint8_t *src, size_t len)
{
    assert(tid >= 0 && static_cast<size_t>(tid) < numThreads_);
    assert(src);
    assert(len <= lineSize_);

    if (isFull(tid)) {
        // SafeSpec §5 (TSA mitigation): stall, never evict speculatively.
        // Evicting one speculative entry to make room for another creates
        // a contention side-channel; stalling is the safe alternative.
        DPRINTF(SpaceSpec,
                "[tid:%d] Shadow FULL (%zu/%zu) — stalling [sn:%llu] "
                "addr %#x\n",
                tid, occupancy(tid), entriesPerThread_, seqNum, lineAddr);
        ++stats.stallsFull;
        return false;
    }

    // If the same line address is already present (e.g. two speculative
    // loads to the same cache line) just refresh the data; do not add a
    // second entry.
    ShadowCacheEntry *existing = findEntry(lineAddr, tid);
    if (existing) {
        DPRINTF(SpaceSpec,
                "[tid:%d] Shadow UPDATE [sn:%llu] addr %#x "
                "(line already present, refreshing data)\n",
                tid, seqNum, lineAddr);
        std::memcpy(existing->data.data(), src, len);
        // Update to the newer seqNum so promotion fires at the right time.
        existing->seqNum = seqNum;
        return true;
    }

    // Append a new entry at the back of the FIFO queue.
    ShadowCacheEntry entry(lineSize_);
    entry.tag    = lineAddr;
    entry.seqNum = seqNum;
    entry.tid    = tid;
    entry.valid  = true;
    std::memcpy(entry.data.data(), src, len);

    threadQueues_[tid].push_back(std::move(entry));
    ++stats.inserts;

    // Track peak occupancy for sizing analysis.
    size_t cur = totalOccupancy();
    if (cur > stats.peakOccupancy.value())
        stats.peakOccupancy = cur;

    DPRINTF(SpaceSpec,
            "[tid:%d] Shadow INSERT [sn:%llu] addr %#x "
            "(occupancy %zu/%zu)\n",
            tid, seqNum, lineAddr, occupancy(tid), entriesPerThread_);

    return true;
}

// ------------------------------------------------------------------------- //

bool
ShadowCache::lookup(Addr lineAddr, ThreadID tid,
                    uint8_t *dst, size_t len) const
{
    assert(tid >= 0 && static_cast<size_t>(tid) < numThreads_);
    assert(dst);
    assert(len <= lineSize_);

    const ShadowCacheEntry *e = findEntry(lineAddr, tid);
    if (!e) {
        ++stats.lookupMisses;
        DPRINTF(SpaceSpec,
                "[tid:%d] Shadow MISS addr %#x\n", tid, lineAddr);
        return false;
    }

    std::memcpy(dst, e->data.data(), len);
    ++stats.lookupHits;

    DPRINTF(SpaceSpec,
            "[tid:%d] Shadow HIT addr %#x [sn:%llu]\n",
            tid, lineAddr, e->seqNum);
    return true;
}

// ------------------------------------------------------------------------- //

std::optional<ShadowCacheEntry>
ShadowCache::promote(InstSeqNum seqNum, ThreadID tid)
{
    assert(tid >= 0 && static_cast<size_t>(tid) < numThreads_);

    auto &q = threadQueues_[tid];

    // Walk front-to-back (oldest first); the committing instruction may
    // not be at the very front if earlier instructions had no shadow entry.
    for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->seqNum == seqNum) {
            ShadowCacheEntry result = std::move(*it);
            q.erase(it);
            ++stats.promotions;

            DPRINTF(SpaceSpec,
                    "[tid:%d] Shadow PROMOTE [sn:%llu] addr %#x → L1 "
                    "(occupancy now %zu/%zu)\n",
                    tid, seqNum, result.tag,
                    occupancy(tid), entriesPerThread_);

            return result;
        }
    }

    // No entry for this seqNum — the load may not have been speculative,
    // or may already have been promoted.  This is not an error.
    return std::nullopt;
}

// ------------------------------------------------------------------------- //

void
ShadowCache::squash(InstSeqNum squashedSeqNum, ThreadID tid)
{
    assert(tid >= 0 && static_cast<size_t>(tid) < numThreads_);

    auto &q = threadQueues_[tid];
    size_t before = q.size();

    // Remove all entries with seqNum > squashedSeqNum.
    // These are instructions younger than the squash point and must be
    // discarded with no trace reaching L1.
    q.erase(
        std::remove_if(q.begin(), q.end(),
            [squashedSeqNum](const ShadowCacheEntry &e) {
                return e.seqNum > squashedSeqNum;
            }),
        q.end());

    size_t removed = before - q.size();
    stats.squashedEntries += removed;

    if (removed > 0) {
        DPRINTF(SpaceSpec,
                "[tid:%d] Shadow SQUASH: discarded %zu entries "
                "(seqNum > %llu), occupancy now %zu/%zu\n",
                tid, removed, squashedSeqNum,
                occupancy(tid), entriesPerThread_);
    }
}

} // namespace gem5
