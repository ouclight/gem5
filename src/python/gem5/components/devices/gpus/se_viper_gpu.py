# Copyright (c) 2026
# All rights reserved.

from __future__ import annotations

"""SE-mode Vega/gfx900 GPU building blocks for VIPER stdlib systems."""

from dataclasses import dataclass
from typing import List

from m5.objects import (
    ComputeUnit,
    DynPoolManager,
    GPUCommandProcessor,
    GPUDispatcher,
    HSAPacketProcessor,
    LdsState,
    RegisterFileCache,
    RegisterManager,
    ScalarRegisterFile,
    Shader,
    SimplePoolManager,
    TLBCoalescer,
    VectorRegisterFile,
    Wavefront,
    X86GPUTLB,
)
from m5.params import AddrRange


@dataclass(frozen=True)
class SEViperGPUConfig:
    gpu_index: int
    gpu_id: int
    vram_range: AddrRange
    vram_pool_id: int
    render_minor: int = 128
    num_compute_units: int = 4
    cu_per_sqc: int = 4
    num_hw_queues: int = 10
    hsapp_pio_base: int = 0x200000000
    hsapp_pio_size: int = 0x1000
    gpu_clock: str = "1GHz"
    gpu_voltage: str = "1.0V"
    wb_l2: bool = True

    @property
    def hsapp_pio_addr(self) -> int:
        return self.hsapp_pio_base + self.gpu_index * self.hsapp_pio_size


class SEViperComputeUnit(ComputeUnit):
    def __init__(
        self,
        cu_id: int,
        *,
        wf_size: int = 64,
        simds_per_cu: int = 4,
        wfs_per_simd: int = 8,
        lds_size: int = 65536,
    ):
        super().__init__()

        self.cu_id = cu_id
        self.num_SIMDs = simds_per_cu
        self.wf_size = wf_size
        self.n_wf = wfs_per_simd
        self.perLaneTLB = False
        self.localDataStore = LdsState(size=lds_size)

        self.wavefronts = [
            Wavefront(simdId=simd, wf_slot_id=slot, wf_size=wf_size)
            for simd in range(simds_per_cu)
            for slot in range(wfs_per_simd)
        ]

        self.vector_register_file = [
            VectorRegisterFile(simd_id=simd, wf_size=wf_size, num_regs=2048)
            for simd in range(simds_per_cu)
        ]
        self.scalar_register_file = [
            ScalarRegisterFile(simd_id=simd, wf_size=wf_size, num_regs=2048)
            for simd in range(simds_per_cu)
        ]
        self.register_file_cache = [
            RegisterFileCache(simd_id=simd, cache_size=0)
            for simd in range(simds_per_cu)
        ]
        self.register_manager = RegisterManager(
            policy="static",
            vrf_pool_managers=[
                DynPoolManager(pool_size=2048, min_alloc=4)
                for _ in range(simds_per_cu)
            ],
            srf_pool_managers=[
                SimplePoolManager(pool_size=2048, min_alloc=4)
                for _ in range(simds_per_cu)
            ],
        )

        self.ldsPort = self.ldsBus.cpu_side_port
        self.ldsBus.mem_side_port = self.localDataStore.cuPort

        self._create_l1_tlbs()

    def _new_l1_tlb(self):
        return X86GPUTLB(
            size=64,
            assoc=64,
            hitLatency=1,
            missLatency2=750,
            maxOutstandingReqs=64,
            accessDistance=1,
        )

    def _new_l1_coalescer(self):
        return TLBCoalescer(
            probesPerCycle=2,
            coalescingWindow=1,
            disableCoalescing=False,
        )

    def _create_l1_tlbs(self):
        self.l1_tlb = self._new_l1_tlb()
        self.l1_coalescer = self._new_l1_coalescer()
        self.translation_port = self.l1_coalescer.cpu_side_ports
        self.l1_coalescer.mem_side_ports = self.l1_tlb.cpu_side_ports

        self.scalar_tlb = self._new_l1_tlb()
        self.scalar_coalescer = self._new_l1_coalescer()
        self.scalar_tlb_port = self.scalar_coalescer.cpu_side_ports
        self.scalar_coalescer.mem_side_ports = self.scalar_tlb.cpu_side_ports

        self.sqc_tlb = self._new_l1_tlb()
        self.sqc_coalescer = self._new_l1_coalescer()
        self.sqc_tlb_port = self.sqc_coalescer.cpu_side_ports
        self.sqc_coalescer.mem_side_ports = self.sqc_tlb.cpu_side_ports

    def get_tlb_ports(self):
        return [
            self.l1_tlb.mem_side_ports,
            self.sqc_tlb.mem_side_ports,
            self.scalar_tlb.mem_side_ports,
        ]


class SEVegaGPU(Shader):
    def __init__(self, config: SEViperGPUConfig):
        super().__init__()

        self._config = config
        self.n_wf = 8
        self.cu_per_sqc = config.cu_per_sqc
        self.timing = True
        self.impl_kern_launch_acq = True
        self.impl_kern_end_rel = False

        self.CUs = [
            SEViperComputeUnit(cu_id=cu_id)
            for cu_id in range(config.num_compute_units)
        ]

        self._cpu_dma_ports = []
        self._gpu_dma_ports = []
        self._create_shared_tlbs()

        self.dispatcher = GPUDispatcher(kernel_exit_events=True)
        self.gpu_cmd_proc = GPUCommandProcessor(
            hsapp=HSAPacketProcessor(
                pioAddr=config.hsapp_pio_addr,
                numHWQueues=config.num_hw_queues,
            ),
            dispatcher=self.dispatcher,
        )
        self.dispatcher = self.gpu_cmd_proc.dispatcher

        self._cpu_dma_ports.append(self.gpu_cmd_proc.hsapp.dma)
        self._cpu_dma_ports.append(self.gpu_cmd_proc.dma)

    def _create_shared_tlbs(self):
        self.l2_tlb = X86GPUTLB(
            size=4096,
            assoc=64,
            hitLatency=69,
            missLatency2=750,
            maxOutstandingReqs=64,
            accessDistance=2,
        )
        self.l2_coalescer = TLBCoalescer(probesPerCycle=2)
        self.l3_tlb = X86GPUTLB(
            size=8192,
            assoc=64,
            hitLatency=150,
            missLatency2=750,
            maxOutstandingReqs=64,
            accessDistance=3,
        )
        self.l3_coalescer = TLBCoalescer(probesPerCycle=2)

        for cu in self.CUs:
            for port in cu.get_tlb_ports():
                self.l2_coalescer.cpu_side_ports = port
        self.l2_coalescer.mem_side_ports = self.l2_tlb.cpu_side_ports
        self.l2_tlb.mem_side_ports = self.l3_coalescer.cpu_side_ports
        self.l3_coalescer.mem_side_ports = self.l3_tlb.cpu_side_ports

    def get_compute_units(self) -> List[SEViperComputeUnit]:
        return self.CUs

    def get_cpu_dma_ports(self):
        return self._cpu_dma_ports

    def get_gpu_dma_ports(self):
        return self._gpu_dma_ports

    def get_command_processor(self):
        return self.gpu_cmd_proc

    def get_vram_range(self):
        return self._config.vram_range

    def get_gpu_id(self):
        return self._config.gpu_id

    def get_vram_pool_id(self):
        return self._config.vram_pool_id

    def get_render_minor(self):
        return self._config.render_minor

    def set_cpu_pointer(self, cpu):
        self.cpu_pointer = cpu

    def connect_iobus(self, iobus):
        self.gpu_cmd_proc.pio = iobus.mem_side_ports
        self.gpu_cmd_proc.hsapp.pio = iobus.mem_side_ports
