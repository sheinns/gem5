/*
 * Load Value Prediction Unit (LVPU)
 * Top-level controller coordinating LCT + LVPT + CVU.
 * Models the Apple M3 LVP as reverse-engineered in the FLOP paper.
 *
 * Key FLOP-paper behaviours modelled:
 *   - 8-byte loads: only predict zero (§4.2)
 *   - ~240-load training threshold via 8-bit saturating counters
 *   - 72-entry CVU for simultaneous tracking
 *   - PC-tagged, constant-only training (no stride)
 *   - FIFO replacement
 */

#ifndef __CPU_LVP_LOAD_VALUE_PREDICTION_UNIT_HH__
#define __CPU_LVP_LOAD_VALUE_PREDICTION_UNIT_HH__

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/lvp/constant_verification_unit.hh"
#include "cpu/lvp/load_classification_table.hh"
#include "cpu/lvp/load_value_prediction_table.hh"
#include "cpu/lvp/lvp_enums.hh"
#include "params/LoadValuePredictionUnit.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace lvp
{

/**
 * Result of a load prediction attempt.
 */
struct LVPResult
{
    LVPClassification classification;
    RegVal predictedValue;
    bool valid;
};

class LoadValuePredictionUnit : public SimObject
{
  public:
    PARAMS(LoadValuePredictionUnit);
    LoadValuePredictionUnit(const Params &p);
    ~LoadValuePredictionUnit() = default;

    /**
     * Predict the value for a load instruction.
     * @param tid        Thread ID.
     * @param instPC     PC of the load instruction.
     * @param loadSize   Size of the load in bytes.
     * @return LVPResult containing classification, value, and validity.
     */
    LVPResult predictLoad(ThreadID tid, Addr instPC, unsigned loadSize);

    /**
     * Verify a prediction after the load completes.
     * Updates LCT, LVPT, and triggers CVU invalidation on mismatch.
     *
     * @param tid           Thread ID.
     * @param instPC        PC of the load.
     * @param loadAddr      Effective address of the load.
     * @param correctVal    The actual value loaded from memory.
     * @param predictedVal  The value that was predicted.
     * @param prediction    The classification used for prediction.
     * @return true if the prediction was correct.
     */
    bool verifyPrediction(ThreadID tid, Addr instPC, Addr loadAddr,
                          RegVal correctVal, RegVal predictedVal,
                          LVPClassification prediction);

    /**
     * Train the LVP for a load that was not predicted.
     * Looks up the last value in LVPT to determine if the value has changed.
     */
    void trainLoad(ThreadID tid, Addr instPC, RegVal actualVal);

    /**
     * Notify the CVU of a store's effective address.
     */
    void processStoreAddress(ThreadID tid, Addr storeAddr);

    /**
     * Notify the CVU of a constant load address for tracking.
     */
    void processConstLoadAddress(ThreadID tid, Addr instPC,
                                 Addr loadAddr, unsigned lvptIndex);

    /**
     * Get the LVPT index for an instruction PC.
     */
    unsigned getLVPTIndex(Addr instPC, ThreadID tid);

    /** Whether LVP is enabled. */
    bool isEnabled() const { return enabled; }

  private:
    /** Check if the load is eligible for prediction per FLOP rules. */
    bool isEligible(unsigned loadSize, RegVal value) const;

    LoadClassificationTable *lct;
    LoadValuePredictionTable *lvpt;
    ConstantVerificationUnit *cvu;

    const bool enabled;

    struct LVPStats : public statistics::Group
    {
        LVPStats(LoadValuePredictionUnit *lvpu);
        statistics::Scalar totalLoads;
        statistics::Scalar numPredictions;
        statistics::Scalar numConstLoads;
        statistics::Scalar numPredictionCorrect;
        statistics::Scalar numPredictionIncorrect;
        statistics::Scalar numTrainCorrect;
        statistics::Scalar numTrainIncorrect;
        statistics::Scalar num8ByteZeroPredictions;
        statistics::Scalar numSuppressed;
    } stats;
};

} // namespace lvp
} // namespace gem5

#endif // __CPU_LVP_LOAD_VALUE_PREDICTION_UNIT_HH__
