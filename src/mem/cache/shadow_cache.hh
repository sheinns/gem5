/*
 * SpaceSpec — Shadow Cache
 *
 * Implements the SafeSpec (Khasawneh et al., DAC'19) shadow state
 * structure for the L1-D cache.  Speculative cache fills are held here
 * instead of in the real L1 until the instruction commits (WFC policy).
 * On squash the entry is discarded silently, leaving no trace in L1.
 *
 * Where the fills come from
 * -------------------------
 * The interception happens in BaseCache::recvTimingResp(), *not* in the
 * LSQUnit.  By the time a load's response reaches the pipeline the L1-D
 * has already allocated and dirtied the block, so a pipeline-side hook is
 * far too late to protect the cache.  Catching the fill inside the cache
 * has two further benefits:
 *
 *   1. the fill packet carries the whole cache line
 *      (BaseCache::handleFill asserts pkt->getSize() == blkSize), whereas
 *      the response delivered to the CPU only carries the requested bytes;
 *   2. gem5 already has a "fill, but do not install" path
 *      (mshr->allocOnFill() == false -> tempBlock), so blocking the L1
 *      install costs a one-line change and no new cache machinery.
 *
 * Design parameters (recommended from SafeSpec 3-5):
 *   - 64 entries per thread (covers 99.99th percentile, Figure 3)
 *   - FIFO ordering within each thread's partition
 *   - WFC (wait-for-commit) promotion policy
 *   - drop, never evict, when full (see insert())
 */

#ifndef __MEM_CACHE_SHADOW_CACHE_HH__
#define __MEM_CACHE_SHADOW_CACHE_HH__

#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "enums/SpeculationPolicy.hh"
#include "params/ShadowCache.hh"
#include "sim/serialize.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class BaseCache;

/**
 * One shadow-cache entry.
 *
 * Holds a single full cache line that was fetched speculatively and must
 * not yet reach L1-D.
 */
struct ShadowCacheEntry
{
    /** Block-aligned physical address of the cache line. */
    Addr tag = 0;

    /** Raw line bytes; always line_size() long. */
    std::vector<uint8_t> data;

    /**
     * ROB sequence number of the *oldest* still-uncommitted load that
     * owns this line.  On promotion this is the instruction whose commit
     * installs the line into L1-D.
     */
    InstSeqNum seqNum = 0;

    /** Thread that owns this entry. */
    ThreadID tid = 0;

    /** Entry is occupied and not yet retired. */
    bool valid = false;

    /**
     * True once Commit has handed this line to the L1-D for promotion.
     * The entry is kept alive until retire() so that younger in-flight
     * loads, which still miss in L1, can be served from the shadow.
     */
    bool promoting = false;

    /** Default-construct an empty entry with a given line size. */
    explicit ShadowCacheEntry(size_t ls = 64)
        : data(ls, 0)
    {}
};

/**
 * ShadowCache - fully-associative, per-thread FIFO buffer.
 *
 * Public interface consumed by:
 *   - BaseCache : insert(), lookup(), dropLine()
 *   - Commit    : promote(), retire()
 *   - ROB       : squash()
 */
class ShadowCache : public SimObject
{
  public:
    PARAMS(ShadowCache);
    explicit ShadowCache(const Params &p);

    /** Drain support: the shadow holds architectural-adjacent state. */
    void drain() override;
    void takeOverFrom(const SimObject *other) override;

    // ------------------------------------------------------------------ //
    //  Core interface                                                      //
    // ------------------------------------------------------------------ //

    /**
     * Insert a speculatively-fetched cache line.
     *
     * Called by BaseCache when a fill arrives for a request that was
     * tagged as a shadow fill.
     *
     * If the thread's shadow partition is full the call returns false and
     * the caller must NOT install the line in the real cache.  Dropping the
     * fill is what keeps the security property intact ("no speculative line
     * ever reaches L1"); the only cost is that this line gets no promotion
     * and is re-fetched if it is needed again.
     *
     * Note this deliberately differs from SafeSpec, which stalls the
     * pipeline until a slot frees up.  Dropping is equally safe and cannot
     * deadlock, since the only thing that frees a slot is commit and commit
     * never waits on the shadow.
     *
     * @param seqNum   ROB sequence number of the owning load.
     * @param tid      Thread ID.
     * @param lineAddr Block-aligned physical address.
     * @param src      Pointer to line_size bytes of data.
     * @param len      Number of valid bytes in src (<= line_size).
     * @return true if the entry was accepted, false if the buffer is full.
     */
    bool insert(InstSeqNum seqNum, ThreadID tid,
                Addr lineAddr, const uint8_t *src, size_t len);

    /**
     * Look up a cache line by address and thread.
     *
     * Called by BaseCache before consulting its own tag store, so that a
     * second in-flight load of a line that is currently held in the shadow
     * is served without touching the real cache.
     *
     * @param lineAddr Block-aligned physical address to search for.
     * @param tid      Thread ID.
     * @param offset   Byte offset of the wanted data within the line.
     * @param dst      Output buffer of at least len bytes.
     * @param len      Number of bytes the caller wants.
     * @return true on hit (dst is filled), false on miss.
     */
    bool lookup(Addr lineAddr, ThreadID tid, unsigned offset,
                uint8_t *dst, size_t len);

    /**
     * Hand an entry to the caller for promotion into L1 at commit time.
     *
     * Called by Commit for the instruction at the ROB head.  The entry is
     * *not* removed here: it is flagged promoting and stays visible to
     * lookup() until retire() is called, which Commit does on its next
     * pass.  That window matters because a younger load may still be in
     * flight and would otherwise miss in both L1 and the shadow.
     *
     * @return A copy of the entry, or std::nullopt if there is none.
     */
    std::optional<ShadowCacheEntry>
    promote(InstSeqNum seqNum, ThreadID tid);

    /**
     * Drop an entry that Commit has already promoted.
     *
     * Called by Commit on the pass after promote().
     */
    void retire(InstSeqNum seqNum, ThreadID tid);

    /**
     * Drop the shadow copy of a line, if any.
     *
     * Called by BaseCache when a write of any kind reaches the cache for a
     * line that is still shadow-held.  The shadow copy is by definition
     * older than the store, so keeping it would let a later promotion
     * install stale data over the store's line.  This is the one rule that
     * makes deferred promotion safe in the presence of stores.
     *
     * @return true if an entry was dropped.
     */
    bool dropLine(Addr lineAddr, ThreadID tid);

    /**
     * Discard all shadow entries whose seqNum > squashedSeqNum.
     *
     * Called from ROB::squash(), which covers both branch mispredictions
     * and LVP-mismatch squashes.  Note that the squash_num handed to
     * ROB::squash() is already the exclusive upper bound of what survives
     * (Commit decrements it when the squashing instruction itself is
     * included), so a strict '>' comparison here matches the pipeline
     * exactly.
     *
     * @param squashedSeqNum  Instructions newer than this are squashed.
     * @param tid             Thread ID.
     */
    void squash(InstSeqNum squashedSeqNum, ThreadID tid);

    /**
     * True when the thread's shadow partition has no free slots, i.e. the
     * next fill for this thread will be dropped rather than captured.
     */
    bool isFull(ThreadID tid) const;

    /** Total entries across all threads (for debug/stats). */
    size_t totalOccupancy() const;

    // ------------------------------------------------------------------ //
    //  Configuration / accessors                                          //
    // ------------------------------------------------------------------ //

    /** Cache line size in bytes (must match the cache's block size). */
    size_t lineSize() const { return lineSize_; }

    /** Maximum entries per thread. */
    size_t entriesPerThread() const { return entriesPerThread_; }

    /** Number of hardware threads currently served. */
    size_t numThreads() const { return threadQueues_.size(); }

    /**
     * Resize the per-thread partitions.
     *
     * Called by the CPU once numThreads is known.  Using this instead of a
     * Python param keeps the two from silently disagreeing.
     */
    void setNumThreads(size_t n);

    /** Which loads are treated as speculative. */
    enums::SpeculationPolicy policy() const { return policy_; }

    // ------------------------------------------------------------------ //
    //  The cache this shadow guards                                         //
    // ------------------------------------------------------------------ //

    /**
     * Register the cache whose L1 fills are being shadowed.
     *
     * Called by BaseCache::init().  Commit needs a way to install a promoted
     * line and the O3 CPU has no reference to its data cache, so the cache
     * registers itself here instead of the config wiring it up.  That also
     * makes it impossible for the two to disagree about which cache is
     * protected.
     */
    void setCache(BaseCache *c) { cache = c; }

    /** The cache guarded by this shadow, or nullptr if none registered. */
    BaseCache *cache() const { return cache; }

  private:
    // ------------------------------------------------------------------ //
    //  Internal helpers                                                    //
    // ------------------------------------------------------------------ //

    /** Find an entry by (lineAddr, tid) - returns nullptr if not found. */
    ShadowCacheEntry *findEntry(Addr lineAddr, ThreadID tid);

    /**
     * Find an entry by the ROB sequence number of its oldest owner.
     * Promotion is keyed on the sequence number rather than the address
     * because that is what Commit has at hand.
     */
    ShadowCacheEntry *findEntryBySeqNum(InstSeqNum seqNum, ThreadID tid);

    /** Number of valid entries belonging to tid. */
    size_t occupancy(ThreadID tid) const;

    /** Bounds-check a ThreadID, fatally if it is out of range. */
    void checkTid(ThreadID tid, const char *where) const;

    /** Re-derive the high-water-mark stat. */
    void updatePeakOccupancy();

    // ------------------------------------------------------------------ //
    //  Configuration (set from params, immutable after init)              //
    // ------------------------------------------------------------------ //

    /** Line size in bytes - must match the cache's block size. */
    size_t lineSize_;

    /**
     * Maximum shadow entries allocated to each thread.
     * Sized at 64 (SafeSpec Fig 3, 99.99th-percentile WFC d-cache).
     */
    size_t entriesPerThread_;

    /** Which loads are eligible for shadowing. */
    enums::SpeculationPolicy policy_;

    /** The cache whose L1 fills are shadowed; set by BaseCache::init(). */
    BaseCache *cache = nullptr;

    // ------------------------------------------------------------------ //
    //  Storage                                                             //
    // ------------------------------------------------------------------ //

    /**
     * Per-thread FIFO queues of shadow entries.
     *
     * threadQueues_[tid] holds entries in insertion order (front =
     * oldest).  FIFO is the SafeSpec-recommended ordering because it
     * prevents an attacker from using shadow eviction timing as a covert
     * channel (SafeSpec section 5, TSA mitigation).  In practice entries
     * are only ever removed by commit (retire) or squash, never displaced.
     */
    std::vector<std::deque<ShadowCacheEntry>> threadQueues_;

    // ------------------------------------------------------------------ //
    //  Statistics                                                          //
    // ------------------------------------------------------------------ //
    struct ShadowCacheStats : public statistics::Group
    {
        explicit ShadowCacheStats(ShadowCache *sc);

        /** Lines successfully inserted (speculative fills captured). */
        statistics::Scalar inserts;

        /** Lines promoted from shadow to L1 at commit. */
        statistics::Scalar promotions;

        /** Lines discarded on squash (no L1 trace). */
        statistics::Scalar squashedEntries;

        /** Speculative loads served from shadow (L1 untouched). */
        statistics::Scalar lookupHits;

        /** Loads that missed the shadow (went to the tag store). */
        statistics::Scalar lookupMisses;

        /**
         * Fills that arrived with no room in the shadow.  These are
         * dropped rather than installed in L1, so they are never a
         * security hole - they just lose their promotion.
         */
        statistics::Scalar droppedFull;

        /** Shadow copies discarded because a store reached the line. */
        statistics::Scalar droppedByStore;

        /** High-water mark of simultaneous valid shadow entries. */
        statistics::Scalar peakOccupancy;
    } stats;
};

} // namespace gem5

#endif // __MEM_CACHE_SHADOW_CACHE_HH__
