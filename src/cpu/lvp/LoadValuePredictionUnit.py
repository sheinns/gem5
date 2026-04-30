from m5.objects.SimObject import SimObject
from m5.params import *


class LoadClassificationTable(SimObject):
    """Load Classification Table (LCT) for LVP.
    PC-indexed saturating counters classify loads as
    Unpredictable/Predictable/Constant per the FLOP paper."""

    type = "LoadClassificationTable"
    cxx_class = "gem5::lvp::LoadClassificationTable"
    cxx_header = "cpu/lvp/load_classification_table.hh"

    localPredictorSize = Param.Unsigned(
        128, "Number of entries in the LCT (must be power of 2)"
    )
    localCtrBits = Param.Unsigned(
        8, "Bits per saturating counter (8 for ~240-load threshold)"
    )
    resetOnConstMiss = Param.Bool(
        True, "Reset counter to 0 on constant-class misprediction"
    )


class LoadValuePredictionTable(SimObject):
    """Load Value Prediction Table (LVPT) for LVP.
    Direct-mapped, PC-indexed table storing last-seen values."""

    type = "LoadValuePredictionTable"
    cxx_class = "gem5::lvp::LoadValuePredictionTable"
    cxx_header = "cpu/lvp/load_value_prediction_table.hh"

    entries = Param.Unsigned(
        1024, "Number of LVPT entries (must be power of 2)"
    )


class ConstantVerificationUnit(SimObject):
    """Constant Verification Unit (CVU) for LVP.
    CAM tracking constant loads; stores invalidate matching entries.
    FIFO replacement per FLOP paper §4.2."""

    type = "ConstantVerificationUnit"
    cxx_class = "gem5::lvp::ConstantVerificationUnit"
    cxx_header = "cpu/lvp/constant_verification_unit.hh"

    entries = Param.Unsigned(
        72, "Number of CAM entries (72 per FLOP paper)"
    )


class LoadValuePredictionUnit(SimObject):
    """Top-level Load Value Prediction Unit.
    Coordinates LCT, LVPT, and CVU to model the Apple M3 LVP
    as described in the FLOP paper."""

    type = "LoadValuePredictionUnit"
    cxx_class = "gem5::lvp::LoadValuePredictionUnit"
    cxx_header = "cpu/lvp/load_value_prediction_unit.hh"

    enabled = Param.Bool(True, "Enable the LVP")

    lct = Param.LoadClassificationTable(
        LoadClassificationTable(), "Load Classification Table"
    )
    lvpt = Param.LoadValuePredictionTable(
        LoadValuePredictionTable(), "Load Value Prediction Table"
    )
    cvu = Param.ConstantVerificationUnit(
        ConstantVerificationUnit(), "Constant Verification Unit"
    )
