# Copyright (c) 2026 The Regents of the University of California
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

"""
Script to run a full-system x86 simulation with two MI300X GPU devices.

Usage:
------
```
scons build/VEGA_X86/gem5.opt
./build/VEGA_X86/gem5.opt
    configs/example/gem5_library/x86-mi300x-multigpu.py
    --image <disk image>
    --kernel <kernel>
    --app <multi-GPU HIP application>
```
"""

import argparse

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.devices.gpus.amdgpu import MI300X
from gem5.components.memory import HBM2Stack
from gem5.components.memory.single_channel import SingleChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.prebuilt.viper.board import ViperBoard
from gem5.prebuilt.viper.cpu_cache_hierarchy import ViperCPUCacheHierarchy
from gem5.resources.resource import (
    DiskImageResource,
    FileResource,
)
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

requires(
    coherence_protocol_required=CoherenceProtocol.GPU_VIPER,
)

parser = argparse.ArgumentParser()

parser.add_argument(
    "--image",
    type=str,
    required=True,
    help="Full path to the gem5-resources x86-ubuntu-gpu-ml disk-image.",
)

parser.add_argument(
    "--kernel",
    type=str,
    required=True,
    help="Full path to the gem5-resources vmlinux-gpu-ml kernel.",
)

parser.add_argument(
    "--app",
    type=str,
    required=True,
    help="Path to a multi-GPU HIP application, Python script, or bash script.",
)

parser.add_argument(
    "--opts",
    type=str,
    default="",
    help="Additional arguments for the GPU application.",
)

parser.add_argument(
    "--kvm-perf",
    default=False,
    action="store_true",
    help="Use KVM perf counters to give accurate GPU insts/cycles with KVM.",
)

parser.add_argument(
    "--num-gpus",
    type=int,
    default=2,
    choices=[2],
    help="Number of MI300X GPUs to instantiate. This example supports 2.",
)

args = parser.parse_args()

memory = SingleChannelDDR4_2400(size="8GiB")

# Note: Only KVM and ATOMIC work due to buggy MOESI_AMD_Base protocol.
processor = SimpleProcessor(cpu_type=CPUTypes.KVM, isa=ISA.X86, num_cores=1)

for core in processor.cores:
    if core.is_kvm_core():
        core.get_simobject().usePerf = args.kvm_perf

gpus = [
    MI300X(gpu_memory=HBM2Stack(size="16GiB"))
    for _ in range(args.num_gpus)
]

board = ViperBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=ViperCPUCacheHierarchy(),
    gpus=gpus,
)

disk = DiskImageResource(local_path=args.image, root_partition="1")
kernel = FileResource(local_path=args.kernel)

board.set_kernel_disk_workload(
    kernel=kernel,
    disk_image=disk,
    readfile_contents=board.make_gpu_app(gpus[0], args.app, args.opts),
)

simulator = Simulator(board=board)
simulator.run()
