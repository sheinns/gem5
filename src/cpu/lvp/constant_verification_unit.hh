/*
 * Constant Verification Unit (CVU)
 * Tracks load addresses classified as "constant" by the LCT.
 * A store to a tracked address invalidates the entry.
 * Uses a CAM with FIFO replacement (per FLOP paper §4.2: 72 entries).
 */

#ifndef __CPU_LVP_CONSTANT_VERIFICATION_UNIT_HH__
#define __CPU_LVP_CONSTANT_VERIFICATION_UNIT_HH__

#include <list>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "params/ConstantVerificationUnit.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace lvp
{

struct CAMEntry
{
    Addr pc;
    Addr lvptIndex;
    Addr loadAddress;
    Tick insertionTick;  // For FIFO ordering
};

class ConstantVerificationUnit : public SimObject
{
  public:
    PARAMS(ConstantVerificationUnit);
    ConstantVerificationUnit(const Params &p);
    ~ConstantVerificationUnit() = default;

    /**
     * Process a store address: invalidate any CAM entries whose
     * load address matches.
     */
    void processStoreAddress(ThreadID tid, Addr address);

    /**
     * Check if a constant-classified load address + LVPT index
     * is present in the CAM.
     * @return true if found (load is still constant).
     */
    bool processLoadAddress(Addr loadAddress, Addr lvptIndex, ThreadID tid);

    /**
     * Insert a new constant load into the CAM.
     */
    bool updateConstLoad(Addr pc, Addr address, Addr lvptIndex, ThreadID tid);

  private:
    /** FIFO replacement when CAM is full. */
    void replaceFIFO(const CAMEntry &newEntry, ThreadID tid);

    /** Per-thread CAM (simplified: one list per thread, max 64 threads). */
    std::list<CAMEntry> cam[64];

    /** Max entries per thread. */
    const uint32_t numEntries;

    struct CVUStats : public statistics::Group
    {
        CVUStats(ConstantVerificationUnit *cvu);
        statistics::Scalar constLoadHits;
        statistics::Scalar constLoadMisses;
        statistics::Scalar storeHits;
        statistics::Scalar storeMisses;
        statistics::Scalar replacements;
    } stats;
};

} // namespace lvp
} // namespace gem5

#endif // __CPU_LVP_CONSTANT_VERIFICATION_UNIT_HH__
