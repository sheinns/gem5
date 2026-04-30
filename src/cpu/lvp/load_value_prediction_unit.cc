/*
 * Load Value Prediction Unit (LVPU) — Implementation
 */

#include "cpu/lvp/load_value_prediction_unit.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/LVP.hh"

namespace gem5
{
namespace lvp
{

LoadValuePredictionUnit::LoadValuePredictionUnit(const Params &p)
    : SimObject(p),
      lct(p.lct),
      lvpt(p.lvpt),
      cvu(p.cvu),
      enabled(p.enabled),
      stats(this)
{
    DPRINTF(LVP, "LVP Unit created: enabled=%d\n", enabled);
}

LoadValuePredictionUnit::LVPStats::LVPStats(LoadValuePredictionUnit *lvpu)
    : statistics::Group(lvpu),
      ADD_STAT(totalLoads, statistics::units::Count::get(),
               "Total loads seen by LVP"),
      ADD_STAT(numPredictions, statistics::units::Count::get(),
               "Loads where a prediction was made"),
      ADD_STAT(numConstLoads, statistics::units::Count::get(),
               "Loads classified as constant"),
      ADD_STAT(numCorrect, statistics::units::Count::get(),
               "Correct LVP predictions"),
      ADD_STAT(numMispredictions, statistics::units::Count::get(),
               "LVP mispredictions (squash required)"),
      ADD_STAT(num8ByteZeroPredictions, statistics::units::Count::get(),
               "8-byte loads where zero was predicted"),
      ADD_STAT(numSuppressed, statistics::units::Count::get(),
               "Predictions suppressed (8-byte non-zero)")
{
}

bool
LoadValuePredictionUnit::isEligible(unsigned loadSize, RegVal value) const
{
    // Per FLOP paper §4.2: 8-byte loads are only predicted if
    // the value is zero.  Loads ≤4 bytes are always eligible.
    // loadSize==0 means size is unknown (dispatch time); allow it
    // and defer the check to verification.
    if (loadSize == 0 || loadSize <= 4)
        return true;
    if (loadSize == 8 && value == 0)
        return true;
    return false;
}

LVPResult
LoadValuePredictionUnit::predictLoad(ThreadID tid, Addr instPC,
                                     unsigned loadSize)
{
    LVPResult result;
    result.classification = LVPClassification::StrongUnpredictable;
    result.predictedValue = 0;
    result.valid = false;

    if (!enabled)
        return result;

    ++stats.totalLoads;

    // Step 1: Classification lookup
    LVPClassification cls = lct->lookup(tid, instPC);
    result.classification = cls;

    // Only make predictions for Constant-classified loads
    if (cls != LVPClassification::Constant) {
        DPRINTF(LVP, "predictLoad: PC %#x class=%d, not constant\n",
                instPC, (int)cls);
        return result;
    }

    ++stats.numConstLoads;

    // Step 2: Value lookup
    bool lvptValid = false;
    RegVal predicted = lvpt->lookup(tid, instPC, &lvptValid);

    if (!lvptValid) {
        DPRINTF(LVP, "predictLoad: PC %#x no LVPT entry\n", instPC);
        return result;
    }

    // Step 3: Apply FLOP 8-byte guard
    if (!isEligible(loadSize, predicted)) {
        DPRINTF(LVP, "predictLoad: PC %#x suppressed (8B non-zero: %#x)\n",
                instPC, predicted);
        ++stats.numSuppressed;
        return result;
    }

    if (loadSize == 8 && predicted == 0) {
        ++stats.num8ByteZeroPredictions;
    }

    result.predictedValue = predicted;
    result.valid = true;
    ++stats.numPredictions;

    DPRINTF(LVP, "predictLoad: PC %#x → val %#x (constant, %dB)\n",
            instPC, predicted, loadSize);

    return result;
}

bool
LoadValuePredictionUnit::verifyPrediction(ThreadID tid, Addr instPC,
                                          Addr loadAddr, RegVal correctVal,
                                          RegVal predictedVal,
                                          LVPClassification prediction)
{
    if (!enabled)
        return true;

    bool correct = (correctVal == predictedVal);

    // Update LVPT with the actual value (always)
    lvpt->update(instPC, correctVal, tid);

    // Update LCT
    lct->update(tid, instPC, prediction, correct);

    if (correct) {
        DPRINTF(LVP, "verifyPrediction: PC %#x CORRECT (val %#x)\n",
                instPC, correctVal);
        ++stats.numCorrect;
    } else {
        DPRINTF(LVP, "verifyPrediction: PC %#x MISMATCH "
                "(predicted %#x, actual %#x)\n",
                instPC, predictedVal, correctVal);
        ++stats.numMispredictions;
    }

    return correct;
}

void
LoadValuePredictionUnit::trainLoad(ThreadID tid, Addr instPC, RegVal actualVal)
{
    if (!enabled)
        return;

    bool lvptValid = false;
    RegVal prevVal = lvpt->lookup(tid, instPC, &lvptValid);

    bool correct = false;
    if (lvptValid && prevVal == actualVal) {
        correct = true;
    }

    // Always update LVPT with actual value
    lvpt->update(instPC, actualVal, tid);

    // Update LCT based on whether value matches previous
    lct->update(tid, instPC, LVPClassification::StrongUnpredictable, correct);

    if (correct) {
        DPRINTF(LVP, "trainLoad: PC %#x CORRECT (val %#x)\n",
                instPC, actualVal);
        ++stats.numCorrect;
    } else {
        DPRINTF(LVP, "trainLoad: PC %#x MISMATCH "
                "(prev %#x, actual %#x)\n",
                instPC, prevVal, actualVal);
        ++stats.numMispredictions;
    }
}

void
LoadValuePredictionUnit::processStoreAddress(ThreadID tid, Addr storeAddr)
{
    if (!enabled)
        return;

    cvu->processStoreAddress(tid, storeAddr);
}

void
LoadValuePredictionUnit::processConstLoadAddress(ThreadID tid, Addr instPC,
                                                 Addr loadAddr,
                                                 unsigned lvptIndex)
{
    if (!enabled)
        return;

    cvu->updateConstLoad(instPC, loadAddr, (Addr)lvptIndex, tid);
}

unsigned
LoadValuePredictionUnit::getLVPTIndex(Addr instPC, ThreadID tid)
{
    return lvpt->getIndex(instPC, tid);
}

} // namespace lvp
} // namespace gem5
