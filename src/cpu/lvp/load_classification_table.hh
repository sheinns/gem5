/*
 * Load Classification Table (LCT)
 * Implements PC-indexed saturating counters to classify loads as
 * Unpredictable / Predictable / Constant per the FLOP paper §4.2.
 */

#ifndef __CPU_LVP_LOAD_CLASSIFICATION_TABLE_HH__
#define __CPU_LVP_LOAD_CLASSIFICATION_TABLE_HH__

#include <vector>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/lvp/lvp_enums.hh"
#include "params/LoadClassificationTable.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace lvp
{

class LoadClassificationTable : public SimObject
{
  public:
    PARAMS(LoadClassificationTable);
    LoadClassificationTable(const Params &p);

    /**
     * Look up the classification for a load at the given PC.
     */
    LVPClassification lookup(ThreadID tid, Addr instAddr);

    /**
     * Update the counter for a load after verification.
     * @param prediction   The classification that was used.
     * @param correct      Whether the predicted value matched.
     * @return The new classification after the update.
     */
    LVPClassification update(ThreadID tid, Addr instAddr,
                             LVPClassification prediction, bool correct);

    /** Reset all counters to zero. */
    void reset();

  private:
    /** Map a PC to a table index. */
    unsigned getIndex(Addr instAddr) const;

    /** Derive the classification from a counter value. */
    LVPClassification classify(uint8_t count) const;

    /** Reset a single counter to zero. */
    void resetCounter(unsigned idx);

    const unsigned numSets;
    const unsigned ctrBits;
    const unsigned indexMask;

    /** Whether a constant-miss resets the counter to 0 (vs decrement). */
    const bool resetOnConstMiss;

    std::vector<SatCounter8> counters;
    /** Track which thread "owns" each entry (for SMT tag). */
    std::vector<ThreadID> entryThread;
};

} // namespace lvp
} // namespace gem5

#endif // __CPU_LVP_LOAD_CLASSIFICATION_TABLE_HH__
