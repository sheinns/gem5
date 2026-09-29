# Wrapper around deprecated se.py that disables the Load Value Predictor.
# Usage: gem5.opt configs/deprecated/example/lvp_off.py <se.py args...>

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

from common import Simulation

_real_run = Simulation.run


def lvp_off_run(args, root, system, FutureClass):
    for cpu in system.cpu:
        cpu.loadValuePredictor.enabled = False
        print("LVP disabled on CPU %d" % cpu.cpu_id)
    _real_run(args, root, system, FutureClass)


Simulation.run = lvp_off_run

sys.argv = ["se.py"] + sys.argv[1:]
import se