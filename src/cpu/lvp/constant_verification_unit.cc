/*
 * Constant Verification Unit (CVU) — Implementation
 */

#include "cpu/lvp/constant_verification_unit.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/CVU.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace lvp
{

ConstantVerificationUnit::ConstantVerificationUnit(const Params &p)
    : SimObject(p),
      numEntries(p.entries),
      stats(this)
{
    DPRINTF(CVU, "CVU created: %u entries, FIFO replacement\n", numEntries);
}

ConstantVerificationUnit::CVUStats::CVUStats(ConstantVerificationUnit *cvu)
    : statistics::Group(cvu),
      ADD_STAT(constLoadHits, statistics::units::Count::get(),
               "Constant loads that hit in CVU CAM"),
      ADD_STAT(constLoadMisses, statistics::units::Count::get(),
               "Constant loads that missed in CVU CAM"),
      ADD_STAT(storeHits, statistics::units::Count::get(),
               "Stores that hit in CVU CAM (invalidations)"),
      ADD_STAT(storeMisses, statistics::units::Count::get(),
               "Stores that missed in CVU CAM"),
      ADD_STAT(replacements, statistics::units::Count::get(),
               "CVU CAM FIFO replacements")
{
}

void
ConstantVerificationUnit::processStoreAddress(ThreadID tid, Addr address)
{
    DPRINTF(CVU, "[tid:%d] Store addr %#x searching CVU CAM\n",
            tid, address);

    unsigned tidIdx = (unsigned)tid % 64;
    bool found = false;

    auto it = cam[tidIdx].begin();
    while (it != cam[tidIdx].end()) {
        if (it->loadAddress == address) {
            DPRINTF(CVU, "[tid:%d] Store hit: evicting addr %#x, PC %#x\n",
                    tid, address, it->pc);
            it = cam[tidIdx].erase(it);
            ++stats.storeHits;
            found = true;
            // Don't break — multiple entries might match the same address
        } else {
            ++it;
        }
    }

    if (!found) {
        ++stats.storeMisses;
    }
}

bool
ConstantVerificationUnit::processLoadAddress(Addr loadAddress,
                                             Addr lvptIndex, ThreadID tid)
{
    unsigned tidIdx = (unsigned)tid % 64;

    for (auto &entry : cam[tidIdx]) {
        if (entry.loadAddress == loadAddress &&
            entry.lvptIndex == lvptIndex) {
            DPRINTF(CVU, "[tid:%d] Const load hit: addr %#x\n",
                    tid, loadAddress);
            ++stats.constLoadHits;
            return true;
        }
    }

    DPRINTF(CVU, "[tid:%d] Const load miss: addr %#x\n",
            tid, loadAddress);
    ++stats.constLoadMisses;
    return false;
}

bool
ConstantVerificationUnit::updateConstLoad(Addr pc, Addr address,
                                          Addr lvptIndex, ThreadID tid)
{
    unsigned tidIdx = (unsigned)tid % 64;

    DPRINTF(CVU, "[tid:%d] Adding const load: PC %#x, addr %#x\n",
            tid, pc, address);

    CAMEntry entry;
    entry.pc = pc;
    entry.lvptIndex = lvptIndex;
    entry.loadAddress = address;
    entry.insertionTick = curTick();

    if (cam[tidIdx].size() >= numEntries) {
        replaceFIFO(entry, tid);
    } else {
        cam[tidIdx].push_back(entry);
    }

    return true;
}

void
ConstantVerificationUnit::replaceFIFO(const CAMEntry &newEntry, ThreadID tid)
{
    unsigned tidIdx = (unsigned)tid % 64;

    // FIFO: evict the oldest entry (front of the list)
    if (!cam[tidIdx].empty()) {
        DPRINTF(CVU, "[tid:%d] FIFO evict: PC %#x\n",
                tid, cam[tidIdx].front().pc);
        cam[tidIdx].pop_front();
    }
    cam[tidIdx].push_back(newEntry);
    ++stats.replacements;
}

} // namespace lvp
} // namespace gem5
