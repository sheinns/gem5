/*
 * Load Value Prediction Table (LVPT)
 * Stores the last-seen value for each load PC.
 * PC-indexed, direct-mapped, with tag + value + valid.
 */

#ifndef __CPU_LVP_LOAD_VALUE_PREDICTION_TABLE_HH__
#define __CPU_LVP_LOAD_VALUE_PREDICTION_TABLE_HH__

#include <vector>

#include "base/types.hh"
#include "params/LoadValuePredictionTable.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace lvp
{

class LoadValuePredictionTable : public SimObject
{
  public:
    PARAMS(LoadValuePredictionTable);
    LoadValuePredictionTable(const Params &p);

    /**
     * Look up the predicted value for a load instruction.
     * @param tid     Thread ID.
     * @param instPC  PC of the load instruction.
     * @param valid   [out] Set to true if entry is valid.
     * @return The predicted value (only meaningful if valid).
     */
    RegVal lookup(ThreadID tid, Addr instPC, bool *valid);

    /**
     * Update the table with the actual loaded value.
     */
    void update(Addr instPC, RegVal value, ThreadID tid);

    /**
     * Get the table index for a given PC (used by CVU).
     */
    unsigned getIndex(Addr instPC, ThreadID tid) const;

    /** Reset all entries. */
    void reset();

  private:
    struct LVPTEntry
    {
        Addr tag = 0;
        RegVal value = 0;
        ThreadID tid = 0;
        bool valid = false;
    };

    Addr getTag(Addr instPC) const;

    std::vector<LVPTEntry> table;
    const unsigned numEntries;
    const unsigned idxMask;
    const unsigned tagShiftAmt;
};

} // namespace lvp
} // namespace gem5

#endif // __CPU_LVP_LOAD_VALUE_PREDICTION_TABLE_HH__
