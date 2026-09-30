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

from m5.params import *
from m5.SimObject import SimObject


class SpeculationPolicy(Enum):
    """Which loads are treated as speculative for shadowing purposes.

    SafeSpec shadows every not-yet-committed load.  That is the faithful
    choice and the default here.  The other two exist so the paper's
    ablations can be run without recompiling:

      all_uncommitted   full SafeSpec - every uncommitted load is shadowed
      lvp_predicted_only  only loads the LVP predicted a value for, i.e.
                        isolate the incremental cost of adding SpaceSpec
                        on top of LVP speculation alone
      disabled          shadow nothing (the SimObject is inert)
    """

    all_uncommitted = "Shadow every not-yet-committed load (full SafeSpec)"
    lvp_predicted_only = "Shadow only LVP-predicted loads"
    disabled = "Never shadow (inert SimObject)"


class ShadowCache(SimObject):
    """SpaceSpec Shadow Cache (SafeSpec D-cache shadow structure).

    Holds speculative cache-line fills in isolation from the real L1-D
    cache.  Lines are either promoted to L1 at commit (WFC policy) or
    silently discarded on squash — preventing Spectre/Meltdown side-
    channel leakage through the cache hierarchy.

    Key design choices (from SafeSpec DAC'19, §3-5):
      - WFC  : wait-for-commit before promoting to L1 (blocks Meltdown too)
      - FIFO : ordering within each thread's partition (TSA-safe)
      - Drop: on a full buffer the fill is neither installed in L1 nor
              promoted, instead of evicting another speculative line
              (SafeSpec stalls here; dropping is equally safe and cannot
              deadlock)
      - 64   : entries per thread (covers 99.99th-percentile per Fig 3)

    The same SimObject instance should be shared between the O3 CPU and its
    private L1-D: the cache fills it and serves lookups from it, the CPU
    promotes from it at commit and squashes it from the ROB.
    """

    type = "ShadowCache"
    cxx_header = "mem/cache/shadow_cache.hh"
    cxx_class = "gem5::ShadowCache"

    num_entries = Param.Unsigned(
        64,
        "Maximum shadow entries per hardware thread.  "
        "SafeSpec Figure 3 shows 64 covers the 99.99th-percentile "
        "of concurrent speculative d-cache accesses under WFC policy.",
    )

    line_size = Param.Unsigned(
        64,
        "Cache line size in bytes.  Must match the cache's block size "
        "and be a non-zero power of two.",
    )

    speculation_policy = Param.SpeculationPolicy(
        "all_uncommitted",
        "Which loads get a shadowed fill.  See SpeculationPolicy.",
    )

    # NOTE: the number of hardware threads is deliberately *not* a param.
    # CPU::numThreads is not visible to a SimObject default, so the CPU
    # calls ShadowCache.setNumThreads() from its constructor instead; a
    # param here could silently disagree with the CPU.
