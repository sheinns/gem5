/*
 * Load Value Prediction Table (LVPT) — Implementation
 */

#include "cpu/lvp/load_value_prediction_table.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/LVPT.hh"

namespace gem5
{
namespace lvp
{

LoadValuePredictionTable::LoadValuePredictionTable(const Params &p)
    : SimObject(p),
      numEntries(p.entries),
      idxMask(numEntries - 1),
      tagShiftAmt(floorLog2(numEntries) + 1) // +1 for RISC-V alignment
{
    fatal_if(!isPowerOf2(numEntries),
             "LVPT: entries must be a power of 2.");

    table.resize(numEntries);

    DPRINTF(LVPT, "LVPT created: %u entries\n", numEntries);
}

unsigned
LoadValuePredictionTable::getIndex(Addr instPC, ThreadID tid) const
{
    return ((instPC >> 1) ^ tid) & idxMask;
}

Addr
LoadValuePredictionTable::getTag(Addr instPC) const
{
    return instPC >> tagShiftAmt;
}

RegVal
LoadValuePredictionTable::lookup(ThreadID tid, Addr instPC, bool *valid)
{
    unsigned idx = getIndex(instPC, tid);
    *valid = false;

    if (table[idx].valid && table[idx].tid == tid &&
        table[idx].tag == getTag(instPC)) {
        *valid = true;
        DPRINTF(LVPT, "LVPT hit: PC %#x → idx %u, val %#x\n",
                instPC, idx, table[idx].value);
        return table[idx].value;
    }

    DPRINTF(LVPT, "LVPT miss: PC %#x → idx %u\n", instPC, idx);
    return 0;
}

void
LoadValuePredictionTable::update(Addr instPC, RegVal value, ThreadID tid)
{
    unsigned idx = getIndex(instPC, tid);

    table[idx].tag = getTag(instPC);
    table[idx].value = value;
    table[idx].tid = tid;
    table[idx].valid = true;

    DPRINTF(LVPT, "LVPT update: PC %#x → idx %u, val %#x\n",
            instPC, idx, value);
}

void
LoadValuePredictionTable::reset()
{
    for (auto &e : table)
        e.valid = false;
}

} // namespace lvp
} // namespace gem5
