# Copyright (c) 2026
# All rights reserved.

from __future__ import annotations

"""Unified SE GPU_VIPER cache hierarchy for XGMI peer-VRAM experiments."""

import math

from m5.objects import (
    DMASequencer,
    RubyCache,
    RubyPortProxy,
    RubySequencer,
    SimpleMemory,
    RubySystem,
    SrcClockDomain,
    TreePLRURP,
    VIPERCoalescer,
    VoltageDomain,
)
from m5.params import AddrRange

from ...coherence_protocol import CoherenceProtocol
from ...components.cachehierarchies.abstract_cache_hierarchy import (
    AbstractCacheHierarchy,
)
from ...components.cachehierarchies.ruby.abstract_ruby_cache_hierarchy import (
    AbstractRubyCacheHierarchy,
)
from ...components.cachehierarchies.ruby.caches.viper.corepair_cache import (
    CorePairCache,
)
from ...components.cachehierarchies.ruby.caches.viper.directory import (
    ViperCPUDirectory,
    ViperGPUDirectory,
)
from ...components.cachehierarchies.ruby.caches.viper.dma_controller import (
    ViperGPUDMAController,
)
from ...components.cachehierarchies.ruby.caches.viper.sqc import SQCCache
from ...components.cachehierarchies.ruby.caches.viper.tcc import TCCCache
from ...components.cachehierarchies.ruby.caches.viper.tcp import TCPCache
from ...utils.override import overrides
from ...utils.requires import requires
from .se_board import SEViperBoard
from .se_xgmi_network import ClusteredXGMINetwork


class SEViperXGMICacheHierarchy(AbstractRubyCacheHierarchy):
    _seqs = 0

    @classmethod
    def seq_count(cls):
        cls._seqs += 1
        return cls._seqs - 1

    def __init__(
        self,
        l1d_size: str = "32KiB",
        l1d_assoc: int = 8,
        l1i_size: str = "32KiB",
        l1i_assoc: int = 8,
        l2_size: str = "1MiB",
        l2_assoc: int = 16,
        l3_size: str = "16MiB",
        l3_assoc: int = 16,
        tcp_size: str = "16KiB",
        tcp_assoc: int = 16,
        sqc_size: str = "32KiB",
        sqc_assoc: int = 8,
        scalar_size: str = "16KiB",
        scalar_assoc: int = 4,
        tcc_size: str = "256KiB",
        tcc_assoc: int = 16,
        tcc_count_per_gpu: int = 1,
        cu_per_sqc: int = 4,
        wb_l2: bool = True,
    ):
        super().__init__()

        self._l1d_size = l1d_size
        self._l1d_assoc = l1d_assoc
        self._l1i_size = l1i_size
        self._l1i_assoc = l1i_assoc
        self._l2_size = l2_size
        self._l2_assoc = l2_assoc
        self._l3_size = l3_size
        self._l3_assoc = l3_assoc
        self._tcp_size = tcp_size
        self._tcp_assoc = tcp_assoc
        self._sqc_size = sqc_size
        self._sqc_assoc = sqc_assoc
        self._scalar_size = scalar_size
        self._scalar_assoc = scalar_assoc
        self._tcc_size = tcc_size
        self._tcc_assoc = tcc_assoc
        self._tcc_count_per_gpu = tcc_count_per_gpu
        self._cu_per_sqc = cu_per_sqc
        self._wb_l2 = wb_l2
        self.ruby_system = RubySystem()

    @overrides(AbstractCacheHierarchy)
    def incorporate_cache(self, board: SEViperBoard) -> None:
        requires(coherence_protocol_required=CoherenceProtocol.GPU_VIPER)

        self.ruby_system.network = ClusteredXGMINetwork(
            self.ruby_system,
            cpu_controller_count=0,
            gpu_controller_counts=[],
        )
        self.ruby_system.block_size_bytes = board.get_cache_line_size()
        self.ruby_system.number_of_virtual_networks = 6
        self.ruby_system.network.number_of_virtual_networks = 6
        self._configure_backing_store(board)

        self._controllers = []
        self._directory_controllers = []
        self._dma_controllers = []
        self._cpu_cluster_controllers = []
        self._gpu_cluster_controllers = [
            [] for _ in board.get_devices()
        ]

        self._build_cpu_side(board)
        self._build_gpu_side(board)
        self._build_dma_controllers(board)

        self.ruby_system.num_of_sequencers = (
            self._cpu_sequencers
            + self._gpu_sequencers
            + len(self._dma_controllers)
        )
        self.ruby_system.controllers = self._controllers
        self.ruby_system.directory_controllers = self._directory_controllers
        if self._dma_controllers:
            self.ruby_system.dma_controllers = self._dma_controllers

        ordered_network_controllers = self._ordered_network_controllers()
        self.ruby_system.network.set_cluster_counts(
            cpu_controller_count=len(self._cpu_cluster_controllers),
            gpu_controller_counts=[
                len(controllers)
                for controllers in self._gpu_cluster_controllers
            ],
        )
        self.ruby_system.network.connect(ordered_network_controllers)
        self.ruby_system.network.setup_buffers()

        self.ruby_system.sys_port_proxy = RubyPortProxy(
            ruby_system=self.ruby_system
        )
        board.connect_system_port(self.ruby_system.sys_port_proxy.in_ports)

    def _build_cpu_side(self, board: SEViperBoard):
        self._cpu_sequencers = 0
        cores = board.get_processor().get_cores()

        for i in range(0, len(cores), 2):
            cache = CorePairCache(
                l1d_size=self._l1d_size,
                l1d_assoc=self._l1d_assoc,
                l1i_size=self._l1i_size,
                l1i_assoc=self._l1i_assoc,
                l2_size=self._l2_size,
                l2_assoc=self._l2_assoc,
                network=self.ruby_system.network,
                cache_line_size=board.get_cache_line_size(),
                core=cores[i],
            )
            cache.version = i // 2
            cache.ruby_system = self.ruby_system
            cache.clk_domain = board.get_clock_domain()
            cache.sequencer = RubySequencer(
                version=self.seq_count(),
                dcache=cache.L1D0cache,
                ruby_system=self.ruby_system,
                coreid=0,
                is_cpu_sequencer=True,
                clk_domain=board.get_clock_domain(),
            )
            cache.sequencer1 = RubySequencer(
                version=self.seq_count(),
                dcache=cache.L1D1cache,
                ruby_system=self.ruby_system,
                coreid=1,
                is_cpu_sequencer=True,
                clk_domain=board.get_clock_domain(),
            )
            self._connect_se_io_ports(cache.sequencer, board.get_io_bus())
            self._connect_se_io_ports(cache.sequencer1, board.get_io_bus())

            cores[i].connect_icache(cache.sequencer.in_ports)
            cores[i].connect_dcache(cache.sequencer.in_ports)
            cores[i].connect_walker_ports(
                cache.sequencer.in_ports,
                cache.sequencer.in_ports,
            )
            cores[i].connect_interrupt(
                cache.sequencer.interrupt_out_port,
                cache.sequencer.in_ports,
            )
            self._cpu_sequencers += 1

            if i + 1 < len(cores):
                cores[i + 1].connect_icache(cache.sequencer1.in_ports)
                cores[i + 1].connect_dcache(cache.sequencer1.in_ports)
                cores[i + 1].connect_walker_ports(
                    cache.sequencer.in_ports,
                    cache.sequencer1.in_ports,
                )
                cores[i + 1].connect_interrupt(
                    cache.sequencer.interrupt_out_port,
                    cache.sequencer.in_ports,
                )
                self._cpu_sequencers += 1

            self._controllers.append(cache)
            self._cpu_cluster_controllers.append(cache)

        for addr_range, port in board.get_memory().get_mem_ports():
            directory = ViperCPUDirectory(
                self.ruby_system.network,
                board.get_cache_line_size(),
                addr_range,
                port,
            )
            directory.ruby_system = self.ruby_system
            directory.version = len(self._directory_controllers)
            directory.L2isWB = self._wb_l2
            directory.L3CacheMemory = self._new_l3_cache()
            self._directory_controllers.append(directory)
            self._cpu_cluster_controllers.append(directory)

    def _build_gpu_side(self, board: SEViperBoard):
        self._gpu_sequencers = 0
        gpu_clk_domain = SrcClockDomain(
            clock="1801MHz",
            voltage_domain=VoltageDomain(),
        )
        if (
            self._tcc_count_per_gpu < 1
            or self._tcc_count_per_gpu & (self._tcc_count_per_gpu - 1)
        ):
            raise ValueError("Per-GPU TCC count must be a positive power of two")
        tcc_bits = int(math.log(self._tcc_count_per_gpu, 2))
        deadlock_threshold = 500000
        tcp_version = 0
        sqc_version = 0

        for gpu_index, gpu in enumerate(board.get_devices()):
            gpu_cluster = self._gpu_cluster_controllers[gpu_index]
            compute_units = gpu.get_compute_units()
            for cu in compute_units:
                tcp = TCPCache(
                    tcp_size=self._tcp_size,
                    tcp_assoc=self._tcp_assoc,
                    network=self.ruby_system.network,
                    cache_line_size=board.get_cache_line_size(),
                )
                tcp.version = tcp_version
                tcp_version += 1
                tcp.sequencer = RubySequencer(
                    version=self.seq_count(),
                    dcache=tcp.L1cache,
                    ruby_system=self.ruby_system,
                    is_cpu_sequencer=True,
                )
                tcp.coalescer = VIPERCoalescer(
                    version=self.seq_count(),
                    icache=tcp.L1cache,
                    dcache=tcp.L1cache,
                    ruby_system=self.ruby_system,
                    support_inst_reqs=False,
                    is_cpu_sequencer=False,
                    deadlock_threshold=deadlock_threshold,
                    max_coalesces_per_cycle=1,
                    gmTokenPort=cu.gmTokenPort,
                )
                for port_idx in range(cu.wf_size):
                    cu.memory_port[port_idx] = tcp.coalescer.in_ports
                tcp.ruby_system = self.ruby_system
                tcp.TCC_select_num_bits = tcc_bits
                tcp.TCC_select_cluster_id = gpu_index
                tcp.cluster_id = gpu_index
                tcp.use_seq_not_coal = False
                tcp.issue_latency = 1
                tcp.clk_domain = gpu_clk_domain
                tcp.recycle_latency = 10
                tcp.WB = False
                tcp.disableL1 = False
                self._controllers.append(tcp)
                gpu_cluster.append(tcp)
                self._gpu_sequencers += 1

            assert len(compute_units) % self._cu_per_sqc == 0
            num_sqcs = len(compute_units) // self._cu_per_sqc
            for idx in range(num_sqcs):
                sqc = SQCCache(
                    sqc_size=self._sqc_size,
                    sqc_assoc=self._sqc_assoc,
                    network=self.ruby_system.network,
                    cache_line_size=board.get_cache_line_size(),
                )
                sqc.version = sqc_version
                sqc_version += 1
                sqc.sequencer = RubySequencer(
                    version=self.seq_count(),
                    dcache=sqc.L1cache,
                    ruby_system=self.ruby_system,
                    support_data_reqs=False,
                    is_cpu_sequencer=False,
                    deadlock_threshold=deadlock_threshold,
                )
                cu_base = idx * self._cu_per_sqc
                for cu_num in range(self._cu_per_sqc):
                    compute_units[cu_base + cu_num].sqc_port = (
                        sqc.sequencer.in_ports
                    )
                sqc.ruby_system = self.ruby_system
                sqc.TCC_select_num_bits = tcc_bits
                sqc.TCC_select_cluster_id = gpu_index
                sqc.cluster_id = gpu_index
                sqc.clk_domain = gpu_clk_domain
                sqc.recycle_latency = 10
                self._controllers.append(sqc)
                gpu_cluster.append(sqc)
                self._gpu_sequencers += 1

                scalar = SQCCache(
                    sqc_size=self._scalar_size,
                    sqc_assoc=self._scalar_assoc,
                    network=self.ruby_system.network,
                    cache_line_size=board.get_cache_line_size(),
                )
                scalar.version = sqc_version
                sqc_version += 1
                scalar.sequencer = RubySequencer(
                    version=self.seq_count(),
                    dcache=scalar.L1cache,
                    ruby_system=self.ruby_system,
                    support_data_reqs=False,
                    is_cpu_sequencer=False,
                    deadlock_threshold=deadlock_threshold,
                )
                for cu_num in range(self._cu_per_sqc):
                    compute_units[cu_base + cu_num].scalar_port = (
                        scalar.sequencer.in_ports
                    )
                scalar.ruby_system = self.ruby_system
                scalar.TCC_select_num_bits = tcc_bits
                scalar.TCC_select_cluster_id = gpu_index
                scalar.cluster_id = gpu_index
                scalar.clk_domain = gpu_clk_domain
                scalar.recycle_latency = 10
                self._controllers.append(scalar)
                gpu_cluster.append(scalar)
                self._gpu_sequencers += 1

            for local_tcc_index in range(self._tcc_count_per_gpu):
                tcc = TCCCache(
                    tcc_size=self._tcc_size,
                    tcc_assoc=self._tcc_assoc,
                    network=self.ruby_system.network,
                    cache_line_size=board.get_cache_line_size(),
                )
                tcc.version = (
                    gpu_index * self._tcc_count_per_gpu + local_tcc_index
                )
                tcc.ruby_system = self.ruby_system
                tcc.cluster_id = gpu_index
                tcc.WB = self._wb_l2
                tcc.clk_domain = gpu_clk_domain
                tcc.recycle_latency = 10
                self._controllers.append(tcc)
                gpu_cluster.append(tcc)

        gpu_mem_ports_by_gpu = board.get_gpu_mem_ports_by_gpu()
        for gpu_index, gpu_mem_ports in enumerate(gpu_mem_ports_by_gpu):
            gpu_cluster = self._gpu_cluster_controllers[gpu_index]
            for addr_range, port in gpu_mem_ports:
                directory = ViperGPUDirectory(
                    self.ruby_system.network,
                    board.get_cache_line_size(),
                    addr_range,
                    port,
                )
                directory.ruby_system = self.ruby_system
                directory.version = len(self._directory_controllers)
                directory.TCC_select_num_bits = tcc_bits
                directory.TCC_select_cluster_id = gpu_index
                directory.cluster_id = gpu_index
                directory.L2isWB = self._wb_l2
                directory.L3CacheMemory = self._new_l3_cache(atomic_alus=64)
                self._directory_controllers.append(directory)
                gpu_cluster.append(directory)

    def _build_dma_controllers(self, board: SEViperBoard):
        for gpu_index, ports in enumerate(board.get_dma_ports_by_gpu()):
            gpu_cluster = self._gpu_cluster_controllers[gpu_index]
            for port in ports:
                controller = ViperGPUDMAController(
                    self.ruby_system.network,
                    board.get_cache_line_size(),
                )
                controller.dma_sequencer = DMASequencer(
                    version=len(self._dma_controllers),
                    in_ports=port,
                )
                controller.version = len(self._dma_controllers)
                controller.ruby_system = self.ruby_system
                controller.cluster_id = gpu_index
                controller.dma_sequencer.ruby_system = self.ruby_system
                self._dma_controllers.append(controller)
                gpu_cluster.append(controller)

    def _connect_se_io_ports(self, sequencer, piobus):
        # RubySequencer.connectIOPorts also connects pio_response_port to the
        # bus. That port only propagates address ranges in full-system mode,
        # so connecting it in SE leaves IOXBar waiting for a range forever.
        sequencer.pio_request_port = piobus.cpu_side_ports
        sequencer.mem_request_port = piobus.cpu_side_ports

    def _ordered_network_controllers(self):
        controllers = list(self._cpu_cluster_controllers)
        for gpu_controllers in self._gpu_cluster_controllers:
            controllers.extend(gpu_controllers)
        return controllers

    def _configure_backing_store(self, board: SEViperBoard):
        backing_ranges = list(board.mem_ranges)
        if not backing_ranges:
            raise ValueError("SE VIPER requires at least one memory range")

        backing_start = min(int(addr_range.start) for addr_range in backing_ranges)
        backing_end = max(int(addr_range.end) for addr_range in backing_ranges)
        self.ruby_system.access_backing_store = True
        self.ruby_system.phys_mem = SimpleMemory(
            range=AddrRange(start=backing_start, end=backing_end),
            in_addr_map=False,
        )

    def _new_l3_cache(self, atomic_alus: int = 0):
        kwargs = {}
        if atomic_alus:
            kwargs["atomicALUs"] = atomic_alus
        return RubyCache(
            size=self._l3_size,
            assoc=self._l3_assoc,
            replacement_policy=TreePLRURP(),
            resourceStalls=False,
            dataArrayBanks=16,
            tagArrayBanks=16,
            dataAccessLatency=20,
            tagAccessLatency=15,
            **kwargs,
        )
