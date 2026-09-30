/*
 * SpaceSpec - Shadow Cache Implementation
 *
 * See shadow_cache.hh for full design rationale.
 */

#include "mem/cache/shadow_cache.hh"

#include <algorithm>
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
      policy_(p.speculation_policy),
      threadQueues_(1),
      stats(this)
{
    fatal_if(lineSize_ == 0 || (lineSize_ & (lineSize_ - 1)) != 0,
             "ShadowCache: line_size must be a non-zero power of two, got %zu",
             lineSize_);
    fatal_if(entriesPerThread_ == 0,
             "ShadowCache: num_entries must be > 0");

    // Sized for one thread until the CPU tells us otherwise; see
    // setNumThreads().  Note that CPU::numThreads is not knowable from a
    // Python default, and BaseCache may well be constructed first.
    DPRINTF(SpaceSpec,
            "ShadowCache created: %zu entries/thread, %zu-byte lines, "
            "policy %s\n",
            entriesPerThread_, lineSize_,
            enums::SpeculationPolicyStrings[(int)policy_]);
}

void
ShadowCache::setNumThreads(size_t n)
{
    fatal_if(n == 0, "ShadowCache: setNumThreads(0)");

    if (n == threadQueues_.size())
        return;

    fatal_if(totalOccupancy() != 0,
             "ShadowCache: cannot change the thread count from %zu to %zu "
             "while %zu entries are live", threadQueues_.size(), n,
             totalOccupancy());

    threadQueues_.resize(n);

    DPRINTF(SpaceSpec,
            "ShadowCache: thread count set to %zu (%zu entries/thread)\n",
            n, entriesPerThread_);
}

// ========================================================================= //
//  Serialization (takeover support)                                          //
// ========================================================================= //

void
ShadowCache::drain()
{
    // Nothing to hand over: a simulation that switches out mid-window
    // simply loses its speculative state, which is exactly what a squash
    // would have done anyway.  Draining is a no-op, but declaring it says
    // that gem5 considered the object.
}

void
ShadowCache::takeOverFrom(const SimObject *other)
{
    auto *src = dynamic_cast<const ShadowCache *>(other);
    if (!src)
        return;

    threadQueues_ = src->threadQueues_;
    stats.peakOccupancy = src->stats.peakOccupancy.value();
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
               "Loads served from shadow (L1 untouched)"),
      ADD_STAT(lookupMisses, statistics::units::Count::get(),
               "Loads that missed the shadow and went to the tag store"),
      ADD_STAT(droppedFull, statistics::units::Count::get(),
               "Fills dropped because the shadow was full (never "
               "installed in L1, so no security hole; only lost promotion)"),
      ADD_STAT(droppedByStore, statistics::units::Count::get(),
               "Shadow copies dropped because a store reached the line"),
      ADD_STAT(peakOccupancy, statistics::units::Count::get(),
               "Peak simultaneous valid shadow entries across all threads")
{
}

// ========================================================================= //
//  Private helpers                                                           //
// ========================================================================= //

void
ShadowCache::checkTid(ThreadID tid, const char *where) const
{
    // A fatal_if rather than an assert: asserts are compiled out of
    // NDEBUG (i.e. gem5.opt) builds, and an out-of-range tid would then
    // index the deque out of bounds.
    fatal_if(tid < 0 || static_cast<size_t>(tid) >= threadQueues_.size(),
             "ShadowCache::%s: tid %d out of range [0, %zu)", where, tid,
             threadQueues_.size());
}

ShadowCacheEntry *
ShadowCache::findEntry(Addr lineAddr, ThreadID tid)
{
    checkTid(tid, "findEntry");
    for (auto &e : threadQueues_[tid]) {
        if (e.valid && e.tag == lineAddr)
            return &e;
    }
    return nullptr;
}

size_t
ShadowCache::occupancy(ThreadID tid) const
{
    checkTid(tid, "occupancy");
    // Every entry in the deque is valid by construction: we only ever push
    // valid entries and only ever pop them wholesale.
    return threadQueues_[tid].size();
}

void
ShadowCache::updatePeakOccupancy()
{
    Counter cur = totalOccupancy();
    if (cur > stats.peakOccupancy.value())
        stats.peakOccupancy = cur;
}

// ========================================================================= //
//  Public interface                                                          //
// ========================================================================= //

bool
ShadowCache::isFull(ThreadID tid) const
{
    checkTid(tid, "isFull");
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
    checkTid(tid, "insert");
    fatal_if(src == nullptr, "ShadowCache::insert: null data pointer");
    fatal_if(len > lineSize_,
             "ShadowCache::insert: %zu bytes exceeds line size %zu", len,
             lineSize_);

    // The same line may already be held because a younger in-flight load
    // fetched it too, or because this is the shadowed fill for a line that
    // some load already holds.  Refresh in place rather than adding a
    // second copy.
    ShadowCacheEntry *existing = findEntry(lineAddr, tid);
    if (existing) {
        // Deliberately keep the *oldest* seqNum: promotion is driven by the
        // first owner to reach the commit head, which installs the line
        // into L1 as early as it is legal to do so.  The refreshed bytes
        // are architecturally identical (any intervening store would have
        // called dropLine()), so the older owner promoting newer data is
        // harmless.
        std::memcpy(existing->data.data(), src, len);
        existing->promoting = false;
        DPRINTF(SpaceSpec,
                "[tid:%d] Shadow UPDATE [sn:%llu] addr %#x "
                "(held for [sn:%llu])\n",
                tid, seqNum, lineAddr, existing->seqNum);
        return true;
    }

    if (isFull(tid)) {
        // SafeSpec section 5 (TSA mitigation) forbids evicting one
        // speculative line to make room for another: that turns the shadow
        // into a contention side channel.  SafeSpec stalls instead; we
        // drop, which has the same security outcome (the line never enters
        // L1) and additionally cannot deadlock, since the only thing that
        // frees a slot is commit and commit never waits on the shadow.
        ++stats.droppedFull;
        DPRINTF(SpaceSpec,
                "[tid:%d] Shadow FULL (%zu/%zu) - dropping fill "
                "[sn:%llu] addr %#x (will not be installed in L1)\n",
                tid, occupancy(tid), entriesPerThread_, seqNum, lineAddr);
        return false;
    }

    ShadowCacheEntry entry(lineSize_);
    entry.tag    = lineAddr;
    entry.seqNum = seqNum;
    entry.tid    = tid;
    entry.valid  = true;
    std::memcpy(entry.data.data(), src, len);

    threadQueues_[tid].push_back(std::move(entry));
    ++stats.inserts;
    updatePeakOccupancy();

    DPRINTF(SpaceSpec,
            "[tid:%d] Shadow INSERT [sn:%llu] addr %#x "
            "(occupancy %zu/%zu)\n",
            tid, seqNum, lineAddr, occupancy(tid), entriesPerThread_);

    return true;
}

// ------------------------------------------------------------------------- //

bool
ShadowCache::lookup(Addr lineAddr, ThreadID tid, unsigned offset,
                    uint8_t *dst, size_t len)
{
    checkTid(tid, "lookup");
    fatal_if(dst == nullptr, "ShadowCache::lookup: null output pointer");
    fatal_if(len > lineSize_ || offset + len > lineSize_,
             "ShadowCache::lookup: offset %u + %zu bytes exceeds line size %zu",
             offset, len, lineSize_);

    ShadowCacheEntry *e = findEntry(lineAddr, tid);
    if (!e) {
        ++stats.lookupMisses;
        return false;
    }

    std::memcpy(dst, e->data.data() + offset, len);
    ++stats.lookupHits;

    DPRINTF(SpaceSpec,
            "[tid:%d] Shadow HIT addr %#x +%u/%zu [sn:%llu]\n",
            tid, lineAddr, offset, len, e->seqNum);
    return true;
}

// ------------------------------------------------------------------------- //

std::optional<ShadowCacheEntry>
ShadowCache::promote(InstSeqNum seqNum, ThreadID tid)
{
    checkTid(tid, "promote");

    ShadowCacheEntry *e = findEntryBySeqNum(seqNum, tid);
    if (!e) {
        // No entry for this seqNum: the load was not speculative, or its
        // line was already promoted, or a store invalidated it.  None of
        // these are errors.
        return std::nullopt;
    }

    e->promoting = true;
    ++stats.promotions;

    DPRINTF(SpaceSpec,
            "[tid:%d] Shadow PROMOTE [sn:%llu] addr %#x -> L1 "
            "(occupancy %zu/%zu)\n",
            tid, seqNum, e->tag, occupancy(tid), entriesPerThread_);

    return *e;
}

void
ShadowCache::retire(InstSeqNum seqNum, ThreadID tid)
{
    checkTid(tid, "retire");

    auto &q = threadQueues_[tid];
    for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->seqNum == seqNum) {
            DPRINTF(SpaceSpec,
                    "[tid:%d] Shadow RETIRE [sn:%llu] addr %#x\n",
                    tid, seqNum, it->tag);
            q.erase(it);
            return;
        }
    }
}

// ------------------------------------------------------------------------- //

bool
ShadowCache::dropLine(Addr lineAddr, ThreadID tid)
{
    checkTid(tid, "dropLine");

    auto &q = threadQueues_[tid];
    // Called on every write that reaches the cache, so keep the common case
    // (nothing shadow-held for this thread) cheap.
    if (q.empty())
        return false;

    for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->valid && it->tag == lineAddr) {
            DPRINTF(SpaceSpec,
                    "[tid:%d] Shadow DROP addr %#x (a write reached the "
                    "line while it was shadow-held)\n", tid, lineAddr);
            q.erase(it);
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------- //

void
ShadowCache::squash(InstSeqNum squashedSeqNum, ThreadID tid)
{
    checkTid(tid, "squash");

    auto &q = threadQueues_[tid];
    size_t before = q.size();

    // Entries strictly newer than the squash point.  gem5 hands
    // ROB::squash() an already-exclusive bound (Commit decrements it when
    // the squashing instruction itself is included), so '>' is exactly the
    // set of instructions the pipeline is discarding - including the
    // violating load itself, when squashDueToMemOrder includes it.
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

// ========================================================================= //
//  Sequence-number lookup (out-of-line, after the public API)                 //
// ========================================================================= //

ShadowCacheEntry *
ShadowCache::findEntryBySeqNum(InstSeqNum seqNum, ThreadID tid)
{
    for (auto &e : threadQueues_[tid]) {
        if (e.valid && e.seqNum == seqNum)
            return &e;
    }
    return nullptr;
}

} // namespace gem5
