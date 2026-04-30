/*
 * Load Value Predictor — Enumerations
 * Based on the FLOP paper: "Breaking the Apple M3 CPU via False Load
 * Output Predictions" (Kim, Chuang, Genkin, Yarom)
 */

#ifndef __CPU_LVP_LVP_ENUMS_HH__
#define __CPU_LVP_LVP_ENUMS_HH__

namespace gem5
{
namespace lvp
{

/**
 * Classification of a load instruction's value predictability.
 * Mirrors the state-machine in a saturating counter:
 *   0                          => StrongUnpredictable
 *   1 .. (maxVal/2 - 1)        => WeakUnpredictable
 *   (maxVal/2) .. (maxVal - 1) => Predictable
 *   maxVal (saturated)         => Constant
 */
enum class LVPClassification
{
    StrongUnpredictable,
    WeakUnpredictable,
    Predictable,
    Constant
};

} // namespace lvp
} // namespace gem5

#endif // __CPU_LVP_LVP_ENUMS_HH__
