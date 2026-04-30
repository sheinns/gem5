/*
 * Load Classification Table (LCT) — Implementation
 */

#include "cpu/lvp/load_classification_table.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/LCT.hh"

namespace gem5
{
namespace lvp
{

LoadClassificationTable::LoadClassificationTable(const Params &p)
    : SimObject(p),
      numSets(p.localPredictorSize),
      ctrBits(p.localCtrBits),
      indexMask(numSets - 1),
      resetOnConstMiss(p.resetOnConstMiss),
      counters(numSets, SatCounter8(ctrBits, 0)),
      entryThread(numSets, 0)
{
    fatal_if(!isPowerOf2(numSets),
             "LCT: localPredictorSize must be a power of 2.");

    DPRINTF(LCT, "LCT created: %u sets, %u-bit counters\n",
            numSets, ctrBits);
}

unsigned
LoadClassificationTable::getIndex(Addr instAddr) const
{
    // Use low bits of PC (after removing alignment bits for RISC-V
    // which has 2-byte aligned instructions with C extension).
    return (instAddr >> 1) & indexMask;
}

LVPClassification
LoadClassificationTable::classify(uint8_t count) const
{
    if (count == 0)
        return LVPClassification::StrongUnpredictable;

    const uint8_t maxVal = (1u << ctrBits) - 1;
    const uint8_t halfMax = maxVal >> 1;

    if (count <= halfMax)
        return LVPClassification::WeakUnpredictable;
    if (count == maxVal)
        return LVPClassification::Constant;
    return LVPClassification::Predictable;
}

LVPClassification
LoadClassificationTable::lookup(ThreadID tid, Addr instAddr)
{
    unsigned idx = getIndex(instAddr);

    // SMT: different thread → unpredictable
    if (tid != entryThread[idx])
        return LVPClassification::StrongUnpredictable;

    uint8_t val = counters[idx];
    LVPClassification cls = classify(val);

    DPRINTF(LCT, "LCT lookup: PC %#x → idx %u, ctr=%u, class=%d\n",
            instAddr, idx, (unsigned)val, (int)cls);

    return cls;
}

LVPClassification
LoadClassificationTable::update(ThreadID tid, Addr instAddr,
                                LVPClassification prediction, bool correct)
{
    unsigned idx = getIndex(instAddr);

    if (tid == entryThread[idx]) {
        if (correct) {
            DPRINTF(LCT, "LCT update correct: PC %#x idx %u\n",
                    instAddr, idx);
            ++counters[idx];
        } else {
            DPRINTF(LCT, "LCT update incorrect: PC %#x idx %u\n",
                    instAddr, idx);
            if (prediction == LVPClassification::Constant && resetOnConstMiss)
                resetCounter(idx);
            else
                --counters[idx];
        }
    } else {
        // Destructive interference — new thread takes this entry
        entryThread[idx] = tid;
        resetCounter(idx);
    }

    uint8_t val = counters[idx];
    return classify(val);
}

void
LoadClassificationTable::reset()
{
    for (unsigned i = 0; i < numSets; ++i) {
        counters[i].reset();
    }
}

void
LoadClassificationTable::resetCounter(unsigned idx)
{
    counters[idx].reset();
}

} // namespace lvp
} // namespace gem5
