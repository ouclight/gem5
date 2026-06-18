# Copyright (c) 2026
# All rights reserved.

from __future__ import annotations

"""SE-mode stdlib board for VIPER multi-GPU experiments."""

from pathlib import Path
from typing import (
    List,
    Optional,
)

import m5
from m5.objects import (
    GPUComputeDriver,
    GPURenderDriver,
    IOXBar,
    Process,
    RedirectPath,
    SEWorkload,
)
from m5.params import (
    AddrRange,
    Port,
)
from m5.util import warn

from ...components.boards.abstract_system_board import AbstractSystemBoard
from ...components.boards.se_binary_workload import SEBinaryWorkload
from ...components.devices.gpus.se_viper_gpu import SEVegaGPU
from ...utils.override import overrides
from .se_kfd_topology import (
    default_xgmi_gpu_nodes,
    write_xgmi_topology,
)


def _default_rocm_env(rocm: str = "/opt/rocm") -> List[str]:
    return [
        "LD_LIBRARY_PATH="
        + ":".join(
            [
                f"{rocm}/lib",
                f"{rocm}/hcc/lib",
                f"{rocm}/hsa/lib",
                f"{rocm}/hip/lib",
                f"{rocm}/libhsakmt/lib",
                "/usr/lib/x86_64-linux-gnu",
            ]
        ),
        "HOME=/",
        "HSA_ENABLE_INTERRUPT=0",
        "HSA_ENABLE_SDMA=0",
        "HSAKMT_DEBUG_LEVEL=7",
        "LOADER_ENABLE_LOGGING=1",
        "LOADER_OPTIONS_APPEND=-dump-all -dump-dir /tmp",
    ]


class SEViperBoard(AbstractSystemBoard, SEBinaryWorkload):
    def __init__(
        self,
        clk_freq: str,
        processor,
        memory,
        cache_hierarchy,
        gpus: List[SEVegaGPU],
        gpu_memories,
    ):
        if len(gpus) != len(gpu_memories):
            raise ValueError("Each GPU requires one local memory system")

        self._gpus = list(gpus)
        self._gpu_memories = list(gpu_memories)
        self._cache_line_size = 64
        super().__init__(
            clk_freq=clk_freq,
            processor=processor,
            memory=memory,
            cache_hierarchy=cache_hierarchy,
        )
        self.gpus = self._gpus
        self.gpu_memories = self._gpu_memories
        self.piobus = IOXBar(
            width=32,
            response_latency=0,
            frontend_latency=0,
            forward_latency=0,
        )

    @overrides(AbstractSystemBoard)
    def _setup_board(self) -> None:
        for gpu in self._gpus:
            gpu.connect_iobus(self.piobus)

    @overrides(AbstractSystemBoard)
    def has_io_bus(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_io_bus(self):
        return self.piobus

    @overrides(AbstractSystemBoard)
    def has_pci_bus(self) -> bool:
        return False

    @overrides(AbstractSystemBoard)
    def get_pci_bus(self):
        raise NotImplementedError("SEViperBoard does not model PCI devices")

    @overrides(AbstractSystemBoard)
    def has_dma_ports(self) -> bool:
        return True

    @overrides(AbstractSystemBoard)
    def get_dma_ports(self) -> List[Port]:
        ports = []
        for gpu in self._gpus:
            ports.extend(gpu.get_cpu_dma_ports())
        return ports

    def get_dma_ports_by_gpu(self) -> List[List[Port]]:
        return [
            list(gpu.get_cpu_dma_ports())
            for gpu in self._gpus
        ]

    @overrides(AbstractSystemBoard)
    def has_coherent_io(self) -> bool:
        return False

    @overrides(AbstractSystemBoard)
    def get_mem_side_coherent_io_port(self) -> Port:
        raise NotImplementedError("SEViperBoard has no coherent IO port")

    @overrides(AbstractSystemBoard)
    def get_devices(self):
        return self._gpus

    @overrides(AbstractSystemBoard)
    def _setup_memory_ranges(self) -> None:
        cpu_range = AddrRange(self.memory.get_size())
        self.mem_ranges = [cpu_range]
        self.memory.set_memory_range([cpu_range])

        for gpu, gpu_memory in zip(self._gpus, self._gpu_memories):
            gpu_memory.set_memory_range([gpu.get_vram_range()])
            self.mem_ranges.append(gpu.get_vram_range())

    @overrides(AbstractSystemBoard)
    def _connect_things(self) -> None:
        super()._connect_things()

        host_cpu = self.processor.get_cores()[0].get_simobject()
        for gpu in self._gpus:
            gpu.set_cpu_pointer(host_cpu)

    def get_all_mem_ports(self):
        mem_ports = list(self.memory.get_mem_ports())
        for gpu_memory in self._gpu_memories:
            mem_ports.extend(gpu_memory.get_mem_ports())
        return mem_ports

    def get_gpu_mem_ports(self):
        ports = []
        for gpu_memory in self._gpu_memories:
            ports.extend(gpu_memory.get_mem_ports())
        return ports

    def get_gpu_mem_ports_by_gpu(self):
        return [
            list(gpu_memory.get_mem_ports())
            for gpu_memory in self._gpu_memories
        ]

    def set_se_gpu_binary_workload(
        self,
        binary,
        arguments: Optional[List[str]] = None,
        env_list: Optional[List[str]] = None,
        rocm_path: str = "/opt/rocm",
        cpu_cores_count: int = 1,
        exit_on_work_items: bool = True,
    ) -> None:
        if arguments is None:
            arguments = []
        if env_list is None:
            env_list = _default_rocm_env(rocm_path)

        if self.is_workload_set():
            warn("Workload has been set more than once!")
        self.set_is_workload_set(True)
        self._set_fullsystem(False)

        topology_nodes = default_xgmi_gpu_nodes(
            num_gpus=len(self._gpus),
            gpu_memory_size=self._gpus[0].get_vram_range().size(),
        )
        write_xgmi_topology(
            root=Path(m5.options.outdir),
            gpu_nodes=topology_nodes,
            cpu_memory_size=self.memory.get_size(),
            cpu_cores_count=cpu_cores_count,
        )
        self._configure_redirect_paths()

        executable = binary.get_local_path()
        gpu_driver = GPUComputeDriver(
            filename="kfd",
            isdGPU=True,
            gfxVersion="gfx900",
            dGPUPoolID=0,
            m_type=6,
            devices=[gpu.get_command_processor() for gpu in self._gpus],
            gpuIds=[gpu.get_gpu_id() for gpu in self._gpus],
            vramPoolIds=[gpu.get_vram_pool_id() for gpu in self._gpus],
        )
        render_drivers = [
            GPURenderDriver(filename=f"dri/renderD{gpu.get_render_minor()}")
            for gpu in self._gpus
        ]
        process = Process(
            executable=executable,
            cmd=[executable] + arguments,
            drivers=[gpu_driver] + render_drivers,
            env=env_list,
        )
        if any(core.is_kvm_core() for core in self.processor.get_cores()):
            process.kvmInSE = True
            process.useArchPT = True

        self.workload = SEWorkload.init_compatible(executable)
        self.m5ops_base = max(0xFFFF0000, self.memory.get_size())
        for core in self.processor.get_cores():
            core.set_workload(process)

        self.exit_on_work_items = exit_on_work_items

    def _configure_redirect_paths(self) -> None:
        fs_root = Path(m5.options.outdir) / "fs"
        for dirname in ("proc", "sys", "tmp"):
            (fs_root / dirname).mkdir(parents=True, exist_ok=True)

        self.redirect_paths = [
            RedirectPath(
                app_path="/proc",
                host_paths=[str(fs_root / "proc")],
            ),
            RedirectPath(
                app_path="/sys",
                host_paths=[str(fs_root / "sys")],
            ),
            RedirectPath(
                app_path="/tmp",
                host_paths=[str(fs_root / "tmp")],
            ),
        ]
