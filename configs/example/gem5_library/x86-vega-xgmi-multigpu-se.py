# Copyright (c) 2026
# All rights reserved.

"""
Run an SE-mode Vega/gfx900 VIPER system with XGMI-visible GPU peers.

Example:
    ./build/VEGA_X86/gem5.opt \
        configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py \
        --app tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip
"""

import argparse
import sys

import m5
from m5.objects import Root
from m5.params import AddrRange
from m5.util.convert import toMemorySize

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.devices.gpus.se_viper_gpu import (
    SEVegaGPU,
    SEViperGPUConfig,
)
from gem5.components.memory import SingleChannelDDR4_2400
from gem5.components.memory.single_channel import SingleChannelHBM
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.prebuilt.viper.se_board import (
    SEViperBoard,
    _default_rocm_env,
)
from gem5.prebuilt.viper.se_gpu_cache_hierarchy import (
    SEViperXGMICacheHierarchy,
)
from gem5.resources.resource import BinaryResource
from gem5.utils.requires import requires

parser = argparse.ArgumentParser()
parser.add_argument("--app", required=True, help="Path to a HIP SE binary")
parser.add_argument(
    "--opts",
    default="",
    help="Additional arguments passed to the HIP binary",
)
parser.add_argument(
    "--num-gpus",
    type=int,
    default=2,
    help="Number of XGMI-visible dGPUs to instantiate",
)
parser.add_argument("--cpu-memory-size", default="3GiB")
parser.add_argument("--gpu-memory-size", default="1GiB")
parser.add_argument(
    "--num-cpu-cores",
    type=int,
    default=4,
    help="Number of CPU ThreadContexts available to SE pthread/ROCm runtime",
)
parser.add_argument("--num-cus", type=int, default=4)
parser.add_argument(
    "--rocm-path",
    default="/opt/rocm",
    help="ROCm runtime path visible to the SE process",
)
parser.add_argument(
    "--env",
    action="append",
    default=[],
    metavar="KEY=VALUE",
    help="Override or add an environment variable for the SE process",
)
parser.add_argument(
    "--cpu-type",
    choices=("atomic", "kvm", "timing"),
    default="kvm",
    help="Host CPU model. KVM is recommended for the ROCm runtime path.",
)
parser.add_argument(
    "--kvm-perf",
    default=False,
    action="store_true",
    help="Use KVM perf counters when --cpu-type=kvm.",
)
parser.add_argument(
    "--max-ticks",
    type=int,
    default=None,
    help="Stop after this absolute simulated tick",
)
parser.add_argument(
    "--disable-wb-l2",
    action="store_true",
    help="Disable GPU_VIPER write-back L2. WB L2 is enabled by default.",
)
parser.add_argument(
    "--disable-gpu0-kernel-launch-acquire",
    action="store_true",
    help=(
        "Disable GPU0's implicit cache invalidation at kernel launch. "
        "Required only for cache-persistence diagnostics."
    ),
)
args = parser.parse_args()

if args.num_gpus < 1 or args.num_gpus & (args.num_gpus - 1):
    raise ValueError("--num-gpus must be a positive power of two")
if args.num_cpu_cores < 1:
    raise ValueError("--num-cpu-cores must be positive")

requires(
    isa_required=ISA.X86,
    coherence_protocol_required=CoherenceProtocol.GPU_VIPER,
    kvm_required=args.cpu_type == "kvm",
)

cpu_memory_size = toMemorySize(args.cpu_memory_size)
gpu_memory_size = toMemorySize(args.gpu_memory_size)
gpu_vram_base = cpu_memory_size + 0x40000000

cpu_type = {
    "atomic": CPUTypes.ATOMIC,
    "kvm": CPUTypes.KVM,
    "timing": CPUTypes.TIMING,
}[args.cpu_type]
processor = SimpleProcessor(
    cpu_type=cpu_type,
    isa=ISA.X86,
    num_cores=args.num_cpu_cores,
)
for core in processor.cores:
    if core.is_kvm_core():
        core.get_simobject().usePerf = args.kvm_perf

memory = SingleChannelDDR4_2400(size=args.cpu_memory_size)

gpus = []
gpu_memories = []
for gpu_index in range(args.num_gpus):
    vram_range = AddrRange(
        gpu_vram_base + gpu_index * gpu_memory_size,
        size=gpu_memory_size,
    )
    config = SEViperGPUConfig(
        gpu_index=gpu_index,
        gpu_id=22124 + gpu_index,
        vram_range=vram_range,
        vram_pool_id=gpu_index + 1,
        render_minor=128 + gpu_index,
        num_compute_units=args.num_cus,
        wb_l2=not args.disable_wb_l2,
    )
    gpus.append(SEVegaGPU(config))
    gpu_memories.append(SingleChannelHBM(size=args.gpu_memory_size))

if args.disable_gpu0_kernel_launch_acquire:
    gpus[0].impl_kern_launch_acq = False

cache_hierarchy = SEViperXGMICacheHierarchy(
    cu_per_sqc=4,
    wb_l2=not args.disable_wb_l2,
)

board = SEViperBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    gpus=gpus,
    gpu_memories=gpu_memories,
)

app_args = args.opts.split() if args.opts else []
rocm_env = _default_rocm_env(args.rocm_path)
env_index = {
    entry.partition("=")[0]: index
    for index, entry in enumerate(rocm_env)
}
for entry in args.env:
    key, separator, _ = entry.partition("=")
    if not separator or not key:
        raise ValueError(
            f"--env must use KEY=VALUE syntax: {entry}"
        )
    if key in env_index:
        rocm_env[env_index[key]] = entry
    else:
        env_index[key] = len(rocm_env)
        rocm_env.append(entry)

board.set_se_gpu_binary_workload(
    binary=BinaryResource(local_path=args.app),
    arguments=app_args,
    env_list=rocm_env,
    rocm_path=args.rocm_path,
    cpu_cores_count=args.num_cpu_cores,
)

root = board._pre_instantiate(full_system=False)
m5.instantiate()

def simulate_to_limit():
    if args.max_ticks is None:
        return m5.simulate()
    return m5.simulate(max(0, args.max_ticks - m5.curTick()))


exit_event = simulate_to_limit()
while True:
    cause = exit_event.getCause()
    if (
        cause == "m5_exit instruction encountered"
        or cause == "m5_fail instruction encountered"
        or cause == "user interrupt received"
        or cause == "simulate() limit reached"
        or "exiting with last active thread context" in cause
    ):
        break

    if "GPU Kernel Completed" in cause:
        print("GPU Kernel Completed dump and reset")
        m5.stats.dump()
        m5.stats.reset()
    elif "GPU Blit Kernel Completed" in cause:
        print("GPU Blit Kernel Completed dump and reset")
        m5.stats.dump()
        m5.stats.reset()
    elif "workbegin" in cause:
        print("m5 work begin dump and reset")
        m5.stats.dump()
        m5.stats.reset()
    elif "workend" in cause:
        print("m5 work end dump and reset")
        m5.stats.dump()
        m5.stats.reset()
    else:
        print(f"Unknown exit event: {cause}. Continuing...")

    exit_event = simulate_to_limit()

print(
    "Exiting @ tick {} because {}.".format(
        m5.curTick(),
        exit_event.getCause(),
    )
)
sys.exit(exit_event.getCode())
