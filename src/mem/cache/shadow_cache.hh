/*
 * SpaceSpec — Shadow Cache
 *
 * Implements the SafeSpec (Khasawneh et al., DAC'19) shadow state
 * structure for the L1-D cache. Speculative cache fills are held here
 * instead of in the real L1 until the instruction commits (WFC policy).
 * On squash the entry is discarded silently, leaving no trace in L1.
 *
 * Design parameters (recommended from SafeSpec §3–5):
 *   - 64 entries per thread (covers 99.99th percentile, Figure 3)
 *   - FIFO replacement within each thread's partition
 *   - WFC (wait-for-commit) promotion policy
 *   - Stall (not evict) when full — prevents TSA contention channel (§5)
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
#include "params/ShadowCache.hh"
#include "sim/sim_object.hh"

namespace gem5
{

/**
 * One shadow-cache entry.
 *
 * Holds a single committed-size cache line that was fetched
 * speculatively and must not yet reach L1.
 */
struct ShadowCacheEntry
{
    /** Block-aligned physical address of the cache line. */
    Addr tag = 0;

    /** Raw line bytes.  Size is determined at construction time via
     *  ShadowCache::lineSize (param: line_size, default 64). */
    std::vector<uint8_t> data;

    /** ROB sequence number of the instruction that caused this fill. */
    InstSeqNum seqNum = 0;

    /** Thread that owns this entry. */
    ThreadID tid = 0;

    /** Entry is occupied and not yet evicted/promoted. */
    bool valid = false;

    /** Default-construct an empty entry with a given line size. */
    explicit ShadowCacheEntry(size_t ls = 64)
        : data(ls, 0)
    {}
};

/**
 * ShadowCache — fully-associative, per-thread FIFO buffer.
 *
 * Public interface consumed by:
 *   - LSQUnit  : insert(), lookup(), isFull()
 *   - Commit   : promote()
 *   - ROB      : squash()
 */
class ShadowCache : public SimObject
{
  public:
    PARAMS(ShadowCache);
    explicit ShadowCache(const Params &p);
    ~ShadowCache() = default;

    // ------------------------------------------------------------------ //
    //  Core interface                                                      //
    // ------------------------------------------------------------------ //

    /**
     * Insert a speculatively-fetched cache line.
     *
     * Called by LSQUnit when a speculative load's memory response
     * arrives.  If the thread's shadow partition is full the call
     * returns false and the caller must stall the load.
     *
     * @param seqNum   ROB sequence number of the owning load.
     * @param tid      Thread ID.
     * @param lineAddr Block-aligned physical address.
     * @param src      Pointer to line_size bytes of data.
     * @param len      Number of valid bytes in src (≤ line_size).
     * @return true if the entry was accepted, false if the buffer is full.
     */
    bool insert(InstSeqNum seqNum, ThreadID tid,
                Addr lineAddr, const uint8_t *src, size_t len);

    /**
     * Lookup a cache line by address and thread.
     *
     * Called by LSQUnit before issuing a new speculative load to the
     * memory system — if the line is already in the shadow we can
     * service the load without touching L1.
     *
     * @param lineAddr Block-aligned physical address to search for.
     * @param tid      Thread ID.
     * @param dst      Output buffer of at least line_size bytes.
     * @param len      Number of bytes the caller wants.
     * @return true on hit (dst is filled), false on miss.
     */
    bool lookup(Addr lineAddr, ThreadID tid,
                uint8_t *dst, size_t len) const;

    /**
     * Promote an entry to L1 at commit time (WFC policy).
     *
     * Called by Commit for the instruction at the ROB head.
     * Returns the entry so the caller can construct a fill packet;
     * the entry is removed from the shadow buffer.
     *
     * @return The matching ShadowCacheEntry, or std::nullopt if none.
     */
    std::optional<ShadowCacheEntry>
    promote(InstSeqNum seqNum, ThreadID tid);

    /**
     * Discard all shadow entries whose seqNum > squashedSeqNum.
     *
     * Called from ROB::squash() — covers both branch mispredictions
     * and LVP-mismatch squashes (both flow through the same path).
     *
     * @param squashedSeqNum  Instructions newer than this are squashed.
     * @param tid             Thread ID.
     */
    void squash(InstSeqNum squashedSeqNum, ThreadID tid);

    /**
     * True when the thread's shadow partition has no free slots.
     * LSQUnit should stall (not issue) the load until room appears.
     */
    bool isFull(ThreadID tid) const;

    /** Total entries across all threads (for debug/stats). */
    size_t totalOccupancy() const;

    // ------------------------------------------------------------------ //
    //  Accessors                                                           //
    // ------------------------------------------------------------------ //

    /** Cache line size in bytes (matches system cache_line_size). */
    size_t lineSize() const { return lineSize_; }

    /** Maximum entries per thread. */
    size_t entriesPerThread() const { return entriesPerThread_; }

  private:
    // ------------------------------------------------------------------ //
    //  Internal helpers                                                    //
    // ------------------------------------------------------------------ //

    /** Find an entry by (lineAddr, tid) — returns nullptr if not found. */
    ShadowCacheEntry *findEntry(Addr lineAddr, ThreadID tid);
    const ShadowCacheEntry *findEntry(Addr lineAddr, ThreadID tid) const;

    /** Number of valid entries belonging to tid. */
    size_t occupancy(ThreadID tid) const;

    // ------------------------------------------------------------------ //
    //  Configuration (set from params, immutable after init)              //
    // ------------------------------------------------------------------ //

    /** Line size in bytes — must match the system cache line size. */
    const size_t lineSize_;

    /** Maximum shadow entries allocated to each thread.
     *  Sized at 64 (SafeSpec Fig 3, 99.99th-percentile WFC d-cache). */
    const size_t entriesPerThread_;

    /** Number of hardware threads (SMT). */
    const size_t numThreads_;

    // ------------------------------------------------------------------ //
    //  Storage                                                             //
    // ------------------------------------------------------------------ //

    /**
     * Per-thread FIFO queues of shadow entries.
     *
     * threadQueues_[tid] holds a deque of ShadowCacheEntry objects in
     * FIFO order (front = oldest).  FIFO replacement is the SafeSpec-
     * recommended policy because it prevents an attacker from using
     * shadow eviction timing as a covert channel (§5, TSA mitigation).
     *
     * Indexed by ThreadID (which is just an int).  Sized to numThreads_
     * in the constructor.
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

        /** Speculative loads that missed shadow (went to memory). */
        statistics::Scalar lookupMisses;

        /**
         * Cycles a load was stalled because the shadow buffer was full.
         * Non-zero here indicates the buffer may need to be enlarged,
         * or that the workload has unusually deep speculation.
         */
        statistics::Scalar stallsFull;

        /** High-water mark of simultaneous valid shadow entries. */
        statistics::Scalar peakOccupancy;
    } stats;
};

} // namespace gem5

#endif // __MEM_CACHE_SHADOW_CACHE_HH__
