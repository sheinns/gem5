# Copyright (c) 2026 SpaceSpec Authors
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Run a RISC-V SE-mode binary with the SpaceSpec shadow cache enabled.

SpaceSpec (SafeSpec, Khasawneh et al., DAC'19) keeps speculative cache
fills out of L1-D: a speculative miss fill is diverted into a per-thread
shadow cache and only becomes resident in L1-D once the load commits, or is
discarded silently if the load is squashed first.

Usage (all se.py options are accepted and forwarded)::

    build/RISCV/gem5.opt configs/example/spacespec.py \\
        --cpu-type=O3CPU --caches --cmd=<elf> [se.py args...]

Note that this script *requires* ``--caches``: without a private L1-D there is
nothing for the shadow to protect.

Why the cache is wired up here and not with ``--param``
------------------------------------------------------
``--param`` cannot create SimObjects.  ``SimObject.apply_config`` execs the
command-line string with globals bound to the node's *existing* child
SimObjects, and ``shadow_cache`` defaults to NULL, so there is no child to bind
and no ``ShadowCache`` name in scope.  The object therefore has to be
constructed in Python, which is what the hook below does.

Threat model note
-----------------
``se.py --caches`` builds a private L1I and L1D and *no* L2 (CacheConfig only
creates ``system.l2`` under ``--l2cache``), so the L1-D is the only cache that
can leak a speculative line.  For full-system runs, use a private-L1-only
hierarchy: a private L2 below is an unshadowed leak path.

Environment variables
---------------------
``SPACESPEC_ENTRIES``   shadow entries per thread (default 64)
``SPACESPEC_POLICY``    all_uncommitted | lvp_predicted_only | disabled
``SPACESPEC_LVP``       0 to disable the Load Value Predictor (default 1)
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", ".."))
sys.path.insert(0, os.path.join(_HERE, "..", "deprecated"))

from m5.objects import ShadowCache  # noqa: E402
from m5.params import NULL  # noqa: E402

from common import CacheConfig  # noqa: E402

NUM_ENTRIES = int(os.environ.get("SPACESPEC_ENTRIES", "64"))
POLICY = os.environ.get("SPACESPEC_POLICY", "all_uncommitted")
LVP_ENABLED = os.environ.get("SPACESPEC_LVP", "1") not in ("0", "false", "no")

_real_config_cache = CacheConfig.config_cache


def _spacespec_config_cache(options, system):
    """Build the caches, then bolt a shadow cache onto every L1-D."""
    _real_config_cache(options, system)

    if not options.caches:
        sys.exit("spacespec.py requires --caches: SpaceSpec protects a "
                 "private L1-D, and without one there is nothing to protect.")

    for cpu in system.cpu:
        shadow = ShadowCache(
            num_entries=NUM_ENTRIES,
            # Must match the cache block size, i.e. the system's line size.
            line_size=options.cacheline_size,
            speculation_policy=POLICY,
        )

        # The CPU promotes from the shadow at commit and squashes it from the
        # ROB.
        cpu.shadow_cache = shadow

        # The L1-D fills the shadow, serves tag misses out of it, and installs
        # lines into it at promotion.  This one line is all the wiring the
        # cache needs; the cache registers itself back with the shadow in
        # BaseCache::init() so that Commit can find it again.
        cpu.dcache.shadow_cache = shadow

        # The LVP is an independent speculation source: its value
        # mispredictions funnel through the same ROB::squash() that discards
        # shadow lines, so both defences compose without extra plumbing.
        if not LVP_ENABLED and cpu.loadValuePredictor is not NULL:
            cpu.loadValuePredictor.enabled = False

        print(
            "[spacespec] cpu %d: ShadowCache(num_entries=%d, line_size=%d, "
            "policy=%s) lvp=%s"
            % (
                cpu.cpu_id,
                NUM_ENTRIES,
                options.cacheline_size,
                POLICY,
                "on" if LVP_ENABLED else "off",
            )
        )


CacheConfig.config_cache = _spacespec_config_cache

# Hand control to the standard SE script, which will import this file's
# patched CacheConfig.
sys.argv = ["se.py"] + sys.argv[1:]
import se  # noqa: E402,F401
