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


class ShadowCache(SimObject):
    """SpaceSpec Shadow Cache (SafeSpec D-cache shadow structure).

    Holds speculative cache-line fills in isolation from the real L1-D
    cache.  Lines are either promoted to L1 at commit (WFC policy) or
    silently discarded on squash — preventing Spectre/Meltdown side-
    channel leakage through the cache hierarchy.

    Key design choices (from SafeSpec DAC'19, §3–5):
      - WFC  : wait-for-commit before promoting to L1 (blocks Meltdown too)
      - FIFO : replacement within each thread's partition (TSA-safe)
      - Stall: on full buffer instead of evicting (prevents contention
               side-channel; SafeSpec §5 TSA mitigation)
      - 64   : entries per thread (covers 99.99th-percentile per Fig 3)
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
        "Cache line size in bytes.  Must match the system cache_line_size "
        "and be a non-zero power of two.",
    )

    num_threads = Param.Unsigned(
        1,
        "Number of hardware threads (SMT degree).  Each thread receives "
        "an independent FIFO partition of num_entries slots to prevent "
        "cross-thread TSA contention.",
    )
