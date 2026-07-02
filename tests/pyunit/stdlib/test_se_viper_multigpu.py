import importlib.util
import re
import sys
import tempfile
import unittest
from pathlib import Path

from gem5.prebuilt.viper.se_kfd_topology import (
    default_xgmi_gpu_nodes,
    write_xgmi_topology,
)


def _load_worktree_se_xgmi_network():
    module_name = "_se_xgmi_network_worktree_test"
    module_path = Path(
        "src/python/gem5/prebuilt/viper/se_xgmi_network.py"
    )
    spec = importlib.util.spec_from_file_location(module_name, module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load {module_path}")

    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def _load_worktree_se_gpu_cache_hierarchy():
    module_name = (
        "gem5.prebuilt.viper._se_gpu_cache_hierarchy_worktree_test"
    )
    module_path = Path(
        "src/python/gem5/prebuilt/viper/se_gpu_cache_hierarchy.py"
    )
    spec = importlib.util.spec_from_file_location(module_name, module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load {module_path}")

    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def _load_worktree_se_viper_board():
    module_name = "gem5.prebuilt.viper._se_board_worktree_test"
    module_path = Path("src/python/gem5/prebuilt/viper/se_board.py")
    spec = importlib.util.spec_from_file_location(module_name, module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Cannot load {module_path}")

    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


class SEViperMultiGPUTest(unittest.TestCase):
    def test_hip_free_smoke_isolates_malloc_free_sequence(self):
        source_path = Path(
            "tests/test-progs/gpu/hip-api-smoke/hip_free_smoke.hip"
        )
        makefile = Path(
            "tests/test-progs/gpu/hip-api-smoke/Makefile"
        ).read_text()

        self.assertIn("HIP_FREE_SMOKE_TARGET", makefile)
        self.assertIn("hip_free_smoke", makefile)
        self.assertTrue(source_path.exists())

        source = source_path.read_text()
        required_markers = [
            "hipSetDevice(0)",
            "hipMalloc(&device_data, bytes)",
            "hipFree(device_data)",
            "HIP_FREE_PASSED",
            "m5_exit(0)",
        ]
        previous = -1
        for marker in required_markers:
            position = source.find(marker)
            self.assertNotEqual(position, -1, marker)
            self.assertGreater(position, previous, marker)
            previous = position

    def test_se_hip_compat_shims_hip_free_without_entering_rocm_free_path(self):
        source = Path(
            "tests/test-progs/gpu/hip-api-smoke/se_hip_compat/"
            "se_hip_compat.cpp"
        ).read_text()

        self.assertIn('extern "C" hipError_t\nhipFree(void *ptr)', source)
        self.assertIn("[se_hip_compat] hipFree shim", source)
        self.assertIn("return hipSuccess", source)
        hip_free_impl = source[
            source.index("hipFree(void *ptr)"):
        ]
        self.assertNotIn('dlsym(RTLD_NEXT, "hipFree")', hip_free_impl)
        self.assertNotIn("RealHipFree", hip_free_impl)

    def test_hip_lifecycle_preload_smoke_combines_core_api_sequence(self):
        source_path = Path(
            "tests/test-progs/gpu/hip-api-smoke/"
            "hip_lifecycle_preload_smoke.hip"
        )
        makefile = Path(
            "tests/test-progs/gpu/hip-api-smoke/Makefile"
        ).read_text()

        self.assertIn("HIP_LIFECYCLE_PRELOAD_SMOKE_TARGET", makefile)
        self.assertIn("hip_lifecycle_preload_smoke", makefile)
        self.assertTrue(source_path.exists())

        source = source_path.read_text()
        required_markers = [
            "hipSetDevice(0)",
            "hipMalloc(&device_data, bytes)",
            "hipMemset(device_data, memset_byte, bytes)",
            "hipMemcpy(device_data, host_input",
            "hipMemcpy(host_output, device_data",
            "hipFree(device_data)",
            "HIP_LIFECYCLE_PRELOAD_PASSED",
            "m5_exit(0)",
        ]
        previous = -1
        for marker in required_markers:
            position = source.find(marker)
            self.assertNotEqual(position, -1, marker)
            self.assertGreater(position, previous, marker)
            previous = position

    def test_directory_b_state_debug_instrumentation_removed_after_sdma_fix(self):
        protocol = Path(
            "src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm"
        ).read_text()
        ruby_sconscript = Path("src/mem/ruby/SConscript").read_text()

        temporary_markers = [
            "dir MemData-debug before trigger",
            "dir B-debug stall core-side",
            "dir B-debug stall dma",
            "dir B-debug exit CoreUnblock",
            "dir B-debug exit UnblockWriteThrough",
            "dir B-debug enter from MemData response",
            "dir B-debug enter from ProbeAcksComplete",
            "intToAddress(20213376)",
            "DPRINTF(RubyDirSDMADebug",
        ]
        for marker in temporary_markers:
            self.assertNotIn(marker, protocol)
        self.assertNotIn("RubyDirSDMADebug", ruby_sconscript)

    def test_se_sdma_atomic_uses_single_ruby_atomic_request(self):
        header = Path("src/dev/hsa/se_sdma_engine.hh").read_text()
        source = Path("src/dev/hsa/se_sdma_engine.cc").read_text()
        dma_header = Path("src/dev/dma_device.hh").read_text()
        dma_source = Path("src/dev/dma_device.cc").read_text()
        dma_sequencer_source = Path(
            "src/mem/ruby/system/DMASequencer.cc"
        ).read_text()
        dma_protocol = Path(
            "src/mem/ruby/protocol/MOESI_AMD_Base-dma.sm"
        ).read_text()
        dir_protocol = Path(
            "src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm"
        ).read_text()
        msg_protocol = Path(
            "src/mem/ruby/protocol/MOESI_AMD_Base-msg.sm"
        ).read_text()

        self.assertIn("dmaAtomicAddr", header)
        self.assertIn("dmaAtomic", dma_header)
        self.assertIn("AtomicOpFunctorPtr", dma_header)
        self.assertIn("Request::ATOMIC_RETURN_OP, std::move(atomic_op)",
                      dma_header)
        self.assertIn("AtomicOpFunctorPtr atomic_op", dma_source)
        self.assertIn("std::move(atomic_op)", dma_source)
        self.assertIn("pkt->cmd = MemCmd::WriteReq", dma_sequencer_source)
        self.assertIn("#include \"base/amo.hh\"", source)
        self.assertIn("std::make_unique<AtomicOpAdd<uint64_t>>", source)
        self.assertIn("dmaAtomicAddr(pkt.addr", source)
        self.assertIn("ATOMIC,        desc=\"Atomic read-modify-write\"", msg_protocol)
        self.assertIn("WriteMask writeMask", msg_protocol)
        self.assertIn("SequencerRequestType:ATOMIC", dma_protocol)
        self.assertIn("DMARequestType:ATOMIC", dma_protocol)
        self.assertIn("dma_sequencer.atomicCallback", dma_protocol)
        self.assertIn("in_msg.Type == DMARequestType:ATOMIC", dir_protocol)
        self.assertIn("tbe.atomicData := true", dir_protocol)
        self.assertIn("out_msg.Type := DMAResponseType:DATA", dir_protocol)

        execute_atomic = source[
            source.index("SESDMAEngine::executeAtomic("):
            source.index("SESDMAEngine::executeConstFill(")
        ]
        self.assertNotIn("executeAtomicData", execute_atomic)
        self.assertNotIn("dmaReadAddr(pkt.addr", execute_atomic)
        self.assertNotIn("dmaWriteAddr(pkt.addr", execute_atomic)

    def test_default_gpu_nodes_scale_past_two_devices(self):
        nodes = default_xgmi_gpu_nodes(
            num_gpus=4,
            gpu_memory_size=0x10000000,
        )

        self.assertEqual([node.node_id for node in nodes], [1, 2, 3, 4])
        self.assertEqual(
            [node.gpu_id for node in nodes],
            [22124, 22125, 22126, 22127],
        )
        self.assertEqual([node.vram_pool_id for node in nodes], [1, 2, 3, 4])
        self.assertEqual({node.hive_id for node in nodes}, {1})
        self.assertNotEqual(nodes[0].hive_id, 0)

    def test_topology_emits_xgmi_peer_links(self):
        nodes = default_xgmi_gpu_nodes(
            num_gpus=3,
            gpu_memory_size=0x10000000,
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            write_xgmi_topology(
                root=root,
                gpu_nodes=nodes,
                cpu_memory_size=0x40000000,
            )

            gpu0_props = (
                root
                / "fs/sys/devices/virtual/kfd/kfd/topology/nodes/1/properties"
            ).read_text()
            cpu_props = (
                root
                / "fs/sys/devices/virtual/kfd/kfd/topology/nodes/0/properties"
            ).read_text()
            cpu_cache = (
                root
                / "fs/sys/devices/virtual/kfd/kfd/topology/nodes/0/caches/0/properties"
            ).read_text()
            topology_generation = (
                root / "fs/sys/devices/virtual/kfd/kfd/topology/generation_id"
            ).read_text()
            topology_system = (
                root
                / "fs/sys/devices/virtual/kfd/kfd/topology/system_properties"
            ).read_text()
            gpu0_id = (
                root / "fs/sys/devices/virtual/kfd/kfd/topology/nodes/1/gpu_id"
            ).read_text()
            gpu0_name = (
                root / "fs/sys/devices/virtual/kfd/kfd/topology/nodes/1/name"
            ).read_text()
            gpu0_xgmi = (
                root
                / "fs/sys/devices/virtual/kfd/kfd/topology/nodes/1/io_links/1/properties"
            ).read_text()
            gpu0_xgmi_2 = (
                root
                / "fs/sys/devices/virtual/kfd/kfd/topology/nodes/1/io_links/2/properties"
            ).read_text()
            gpu2_xgmi = (
                root
                / "fs/sys/devices/virtual/kfd/kfd/topology/nodes/3/io_links/1/properties"
            ).read_text()

        self.assertEqual(topology_generation, "2\n")
        self.assertIn("platform_oem", topology_system)
        self.assertEqual(gpu0_id, "22124\n")
        self.assertEqual(gpu0_name, "Vega\n")
        self.assertIn("caches_count 1", cpu_props)
        self.assertIn("processor_id_low 0", cpu_cache)
        self.assertIn("type 7", cpu_cache)
        self.assertNotIn("gpu_id 22124", gpu0_props)
        self.assertIn("location_id 1024", gpu0_props)
        self.assertIn("domain 0", gpu0_props)
        self.assertIn("hive_id 1", gpu0_props)
        self.assertIn("unique_id 22124", gpu0_props)
        self.assertIn("local_mem_size 268435456", gpu0_props)
        self.assertIn("fw_version 421", gpu0_props)
        self.assertIn("capability 238208", gpu0_props)
        self.assertIn("num_sdma_xgmi_engines 1", gpu0_props)
        self.assertIn("num_sdma_queues_per_engine 8", gpu0_props)
        self.assertIn("num_cp_queues 8", gpu0_props)
        self.assertIn("num_gws 64", gpu0_props)
        self.assertNotIn("node_id", gpu0_props)
        self.assertNotIn("name gfx900", gpu0_props)
        self.assertNotIn("vram_pool_id", gpu0_props)
        self.assertIn("type 11", gpu0_xgmi)
        self.assertIn("node_to 2", gpu0_xgmi)
        self.assertIn("flags 1", gpu0_xgmi)
        self.assertIn("type 11", gpu0_xgmi_2)
        self.assertIn("node_to 3", gpu0_xgmi_2)
        self.assertIn("type 11", gpu2_xgmi)
        self.assertIn("node_to 1", gpu2_xgmi)

    def test_gpu_viper_tcc_destinations_pass_cluster_id(self):
        protocol_files = [
            Path("src/mem/ruby/protocol/GPU_VIPER-TCP.sm"),
            Path("src/mem/ruby/protocol/GPU_VIPER-SQC.sm"),
            Path("src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm"),
        ]

        for path in protocol_files:
            text = path.read_text()
            pos = 0
            while True:
                pos = text.find("mapAddressToRange(", pos)
                if pos == -1:
                    break
                end = text.find(")", pos)
                self.assertNotEqual(end, -1, f"unterminated call in {path}")
                call = text[pos:end]
                if "MachineType:TCC" in call:
                    self.assertIn(
                        "cluster_id",
                        call,
                        f"{path} has TCC/TCCdir mapping without cluster_id",
                    )
                pos = end + 1

    def test_clustered_xgmi_network_helper_uses_bidirectional_int_links(self):
        from gem5.prebuilt.viper.se_xgmi_network import (
            _cluster_int_link_specs,
        )

        specs = _cluster_int_link_specs(num_gpus=3)
        pairs = [(spec.src_router, spec.dst_router) for spec in specs]

        self.assertEqual(len(pairs), len(set(pairs)))
        for gpu_router in [1, 2, 3]:
            self.assertIn((0, gpu_router), pairs)
            self.assertIn((gpu_router, 0), pairs)

        for src in [1, 2, 3]:
            for dst in [1, 2, 3]:
                if src != dst:
                    self.assertIn((src, dst), pairs)

        for spec in specs:
            if spec.src_router == 0 or spec.dst_router == 0:
                self.assertEqual(spec.latency, 32)
                self.assertEqual(spec.weight, 4)
            else:
                self.assertEqual(spec.latency, 8)
                self.assertEqual(spec.weight, 1)

    def test_clustered_xgmi_network_constructs_cluster_topology(self):
        from m5.objects import RubyController, RubySystem

        module = _load_worktree_se_xgmi_network()
        controller_router_ids = [0, 0, 1, 2, 2, 3, 3, 3]
        controllers = [
            RubyController() for _ in controller_router_ids
        ]
        network = module.ClusteredXGMINetwork(
            RubySystem(),
            cpu_controller_count=2,
            gpu_controller_counts=[1, 2, 3],
            pcie_link_latency=31,
            pcie_link_weight=7,
            xgmi_link_latency=9,
            xgmi_link_weight=2,
        )

        network.connect(controllers)

        self.assertEqual(
            [int(router.router_id) for router in network.routers],
            [0, 1, 2, 3],
        )
        self.assertEqual(len(network.ext_links), len(controllers))

        ext_router_by_controller = {
            id(link.ext_node): int(link.int_node.router_id)
            for link in network.ext_links
        }
        self.assertEqual(len(ext_router_by_controller), len(controllers))
        self.assertEqual(
            [
                ext_router_by_controller[id(controller)]
                for controller in controllers
            ],
            controller_router_ids,
        )

        actual_links = {}
        for link in network.int_links:
            pair = (
                int(link.src_node.router_id),
                int(link.dst_node.router_id),
            )
            self.assertNotIn(pair, actual_links)
            actual_links[pair] = (
                int(link.latency),
                int(link.weight),
            )

        pcie_pairs = {
            pair
            for gpu_router in (1, 2, 3)
            for pair in ((0, gpu_router), (gpu_router, 0))
        }
        xgmi_pairs = {
            (src_router, dst_router)
            for src_router in (1, 2, 3)
            for dst_router in (1, 2, 3)
            if src_router != dst_router
        }

        self.assertEqual(set(actual_links), pcie_pairs | xgmi_pairs)
        for pair in pcie_pairs:
            self.assertEqual(actual_links[pair], (31, 7))
        for pair in xgmi_pairs:
            self.assertEqual(actual_links[pair], (9, 2))

        link_ids = [
            int(link.link_id)
            for link in [*network.ext_links, *network.int_links]
        ]
        self.assertEqual(len(link_ids), len(set(link_ids)))
        self.assertEqual(sorted(link_ids), list(range(len(link_ids))))

    def test_clustered_xgmi_network_is_owned_by_se_module(self):
        se_network = Path(
            "src/python/gem5/prebuilt/viper/se_xgmi_network.py"
        ).read_text()
        general_network = Path(
            "src/python/gem5/prebuilt/viper/viper_network.py"
        ).read_text()
        hierarchy = Path(
            "src/python/gem5/prebuilt/viper/se_gpu_cache_hierarchy.py"
        ).read_text()

        self.assertIn("class ClusteredXGMINetwork", se_network)
        self.assertNotIn("class ClusteredXGMINetwork", general_network)
        self.assertNotIn("from .se_xgmi_network import", general_network)
        self.assertIn(
            "from .se_xgmi_network import ClusteredXGMINetwork",
            hierarchy,
        )

    def test_simple_pt2pt_link_ids_are_unique_and_contiguous(self):
        network = Path(
            "src/python/gem5/prebuilt/viper/viper_network.py"
        ).read_text()

        simple_pt2pt_start = network.index("class SimplePt2Pt")
        next_class = network.index(
            "class SimpleDoubleCrossbar",
            simple_pt2pt_start,
        )
        simple_pt2pt = network[simple_pt2pt_start:next_class]

        self.assertIn("link_count = len(self.ext_links)", simple_pt2pt)
        append_position = simple_pt2pt.index(
            "SimpleIntLink(link_id=link_count"
        )
        increment_position = simple_pt2pt.index("link_count += 1")
        self.assertLess(append_position, increment_position)

    def test_se_viper_uses_backing_store_for_functional_access(self):
        hierarchy = Path(
            "src/python/gem5/prebuilt/viper/se_gpu_cache_hierarchy.py"
        ).read_text()

        self.assertIn("SimpleMemory", hierarchy)
        self.assertIn("access_backing_store = True", hierarchy)
        self.assertIn("board.mem_ranges", hierarchy)
        self.assertNotIn("PortTerminator", hierarchy)
        self.assertNotIn("_terminate_unused_controller_memory_ports", hierarchy)

    def test_se_viper_ruby_backing_store_is_only_kvm_mapped_memory(self):
        from m5.objects import SimpleMemory
        from m5.params import AddrRange

        module = _load_worktree_se_gpu_cache_hierarchy()

        cpu_memory = SimpleMemory(range=AddrRange(0, size="3GiB"))
        gpu0_memory = SimpleMemory(range=AddrRange("4GiB", size="1GiB"))
        gpu1_memory = SimpleMemory(range=AddrRange("5GiB", size="1GiB"))

        class FakeBoard:
            mem_ranges = [
                cpu_memory.range,
                gpu0_memory.range,
                gpu1_memory.range,
            ]

            @staticmethod
            def get_all_mem_interfaces():
                return [cpu_memory, gpu0_memory, gpu1_memory]

        hierarchy = module.SEViperXGMICacheHierarchy()
        hierarchy._configure_backing_store(FakeBoard())

        for memory in FakeBoard.get_all_mem_interfaces():
            self.assertFalse(memory.kvm_map.value)
        self.assertTrue(hierarchy.ruby_system.phys_mem.kvm_map.value)

    def test_se_viper_m5ops_range_follows_all_cpu_and_gpu_memory(self):
        from m5.params import AddrRange

        module = _load_worktree_se_viper_board()
        memory_ranges = [
            AddrRange(0, size="3GiB"),
            AddrRange("4GiB", size="1GiB"),
            AddrRange("5GiB", size="1GiB"),
        ]

        m5ops_base = module._m5ops_base_after_ranges(memory_ranges)
        m5ops_range = AddrRange(m5ops_base, size="64KiB")

        self.assertEqual(m5ops_base, 0x180000000)
        for memory_range in memory_ranges:
            self.assertGreaterEqual(
                int(m5ops_range.start),
                int(memory_range.end),
            )

    def test_se_viper_places_all_kvm_cores_on_device_event_queue(self):
        module = _load_worktree_se_viper_board()

        class FakeCore:
            def __init__(self, eventq_index):
                self.simobject = type(
                    "FakeKvmCPU",
                    (),
                    {"eventq_index": eventq_index},
                )()

            @staticmethod
            def is_kvm_core():
                return True

            def get_simobject(self):
                return self.simobject

        cores = [FakeCore(index) for index in range(1, 5)]

        module._place_kvm_cores_on_device_event_queue(cores)

        self.assertEqual(len(cores), 4)
        self.assertEqual(
            [core.get_simobject().eventq_index for core in cores],
            [0, 0, 0, 0],
        )

    def test_kvm_context_activation_preserves_clone_state(self):
        kvm_cpu = Path("src/cpu/kvm/base.cc").read_text()
        activate_start = kvm_cpu.index("BaseKvmCPU::activateContext")
        suspend_start = kvm_cpu.index(
            "BaseKvmCPU::suspendContext",
            activate_start,
        )
        activate_context = kvm_cpu[activate_start:suspend_start]

        clone_branch = activate_context.index(
            "if (tc->getUseForClone())"
        )
        discard_position = activate_context.index(
            "kvmStateDirty = false",
            clone_branch,
        )
        normal_branch = activate_context.index(
            "} else {",
            discard_position,
        )
        sync_position = activate_context.index(
            "syncThreadContext()",
            normal_branch,
        )
        clear_clone_position = activate_context.index(
            "tc->setUseForClone(false)",
            sync_position,
        )
        dirty_position = activate_context.index(
            "threadContextDirty = true",
            clear_clone_position,
        )
        schedule_position = activate_context.index(
            "schedule(tickEvent"
        )
        self.assertLess(clone_branch, discard_position)
        self.assertLess(discard_position, normal_branch)
        self.assertLess(normal_branch, sync_position)
        self.assertLess(sync_position, clear_clone_position)
        self.assertLess(dirty_position, schedule_position)

    def test_process_init_defers_clone_context_activation(self):
        process = Path("src/sim/process.cc").read_text()
        init_start = process.index("Process::initState()")
        drain_start = process.index("Process::drain()", init_start)
        init_state = process[init_start:drain_start]

        clone_guard = init_state.index("if (!tc->getUseForClone())")
        activate_position = init_state.index("tc->activate()", clone_guard)
        page_table_position = init_state.index("pTable->initState()")

        self.assertLess(clone_guard, activate_position)
        self.assertLess(activate_position, page_table_position)

    def test_x86_kvm_clone_single_step_diagnostic_is_removed(self):
        base_cpu = Path("src/cpu/kvm/base.cc").read_text()
        x86_cpu = Path("src/arch/x86/kvm/x86_cpu.cc").read_text()
        x86_header = Path("src/arch/x86/kvm/x86_cpu.hh").read_text()

        self.assertNotIn("case KVM_EXIT_DEBUG:", base_cpu)
        self.assertNotIn("handleKvmExitDebug()", base_cpu)
        self.assertNotIn("KVM_GUESTDBG", x86_cpu)
        self.assertNotIn("KVM clone single-step", x86_cpu)
        self.assertNotIn("autoSingleStep", x86_header)

    def test_kvm_se_syscall_return_uses_vcpu_local_cr2_scratch(self):
        process = Path("src/arch/x86/process.cc").read_text()
        handler_start = process.index("/* System call handler */")
        handler_end = process.index("/** Page fault handler */", handler_start)
        handler = process[handler_start:handler_end]

        self.assertNotIn("syscallDataBuf", handler)
        self.assertNotIn("// push", handler)
        self.assertNotIn("// pop", handler)
        self.assertIn("// mov    %rax, %cr2", handler)
        self.assertIn("// mov    %cr2, %rax", handler)
        self.assertIn("0x0f, 0x22, 0xd0", handler)
        self.assertIn("0x0f, 0x20, 0xd0", handler)

    def test_se_viper_board_redirects_rocm_filesystem_paths(self):
        board = Path("src/python/gem5/prebuilt/viper/se_board.py").read_text()

        self.assertIn("RedirectPath", board)
        self.assertIn('app_path="/proc"', board)
        self.assertIn('app_path="/sys"', board)
        self.assertIn('app_path="/tmp"', board)
        self.assertIn("self._configure_redirect_paths()", board)

    def test_kfd_driver_handles_rocm_device_probe_mmap_and_acquire_vm(self):
        driver = Path("src/gpu-compute/gpu_compute_driver.cc").read_text()

        self.assertIn("case 0:", driver)
        self.assertIn("Ignoring KFD mmap over existing range", driver)
        self.assertNotIn("Replacing existing range with KFD mmap", driver)
        self.assertIn('mem_state->mapRegion(start, length, "kfd")', driver)
        self.assertIn("process->allocateMem(start, length)", driver)
        self.assertIn("kfd_ioctl_acquire_vm_args", driver)
        self.assertIn("deviceForGpuId(args->gpu_id)", driver)
        self.assertIn("kfd_ioctl_set_memory_policy_args", driver)
        self.assertNotIn(
            'warn("unimplemented ioctl: AMDKFD_IOC_ACQUIRE_VM',
            driver,
        )
        self.assertNotIn(
            'warn("unimplemented ioctl: AMDKFD_IOC_SET_MEMORY_POLICY',
            driver,
        )

    def test_kfd_multigpu_apertures_are_indexed(self):
        driver = Path("src/gpu-compute/gpu_compute_driver.cc").read_text()

        self.assertIn("if (devices.size() > 1)", driver)
        self.assertIn("scratchApeBase(i + 1)", driver)
        self.assertIn("ldsApeBase(i + 1)", driver)

    def test_kfd_normal_operations_use_debug_tracing(self):
        kfd_driver = Path(
            "src/gpu-compute/gpu_compute_driver.cc"
        ).read_text()
        render_driver = Path(
            "src/gpu-compute/gpu_render_driver.cc"
        ).read_text()

        normal_trace_messages = (
            "Opened KFD driver",
            "AMDKFD_IOC_CREATE_QUEUE gpu_id",
            "AMDKFD_IOC_CREATE_QUEUE queue_id",
            "AMDKFD_IOC_GET_PROCESS_APERTURES returning",
            "aperture[%d] gpu_id",
            "AMDKFD_IOC_CREATE_EVENT type",
            "AMDKFD_IOC_GET_PROCESS_APERTURES_NEW capacity",
            "aperture_new[%d] gpu_id",
            "AMDKFD_IOC_ACQUIRE_VM gpu_id",
            "AMDKFD_IOC_ALLOC_MEMORY_OF_GPU gpu_id",
            "AMDKFD_IOC_ALLOC_MEMORY_OF_GPU handle",
            "AMDKFD_IOC_MAP_MEMORY_TO_GPU handle",
            "map target gpu_id",
        )
        for message in normal_trace_messages:
            self.assertNotIn(f'warn("{message}', kfd_driver)
            self.assertIn(message, kfd_driver)

        self.assertNotIn('warn("Opened GPU render driver', render_driver)
        self.assertIn("DPRINTF(GPUDriver", render_driver)
        self.assertIn("Opened GPU render driver", render_driver)

        actionable_warnings = (
            "Ignoring AMDKFD_IOC_SET_MEMORY_POLICY",
            "Ignoring KFD mmap over existing range",
            "AMDKFD_IOC_FREE_MEMORY_OF_GPU skipped",
        )
        for message in actionable_warnings:
            self.assertRegex(
                kfd_driver,
                rf"warn(?:_once)?\(\"{re.escape(message)}",
            )

        self.assertIn(
            'warn("Unsupported GPU render driver ioctl',
            render_driver,
        )

    def test_x86_clock_nanosleep_matches_se_nanosleep_policy(self):
        syscall_table = Path(
            "src/arch/x86/linux/syscall_tbl64.cc"
        ).read_text()

        self.assertIn(
            '{230, "clock_nanosleep", ignoreWarnOnceFunc}',
            syscall_table,
        )

    def test_x86_frndint_uses_fcw_rounding_mode(self):
        round_macro = Path(
            "src/arch/x86/isa/insts/x87/arithmetic/round.py"
        ).read_text()
        fp_microops = Path(
            "src/arch/x86/isa/microops/fpop.isa"
        ).read_text()
        decoder = Path(
            "src/arch/x86/isa/decoder/x87.isa"
        ).read_text()

        self.assertIn("def macroop FRNDINT", round_macro)
        self.assertIn("rdval t1, fcw", round_macro)
        self.assertIn("roundfp st(0), st(0), t1", round_macro)
        self.assertIn("0x4: Inst::FRNDINT();", decoder)

        self.assertIn("class Roundfp(FpOp)", fp_microops)
        self.assertIn(
            "operand_types = (FloatDestOp, FloatSrc1Op, IntSrc2Op)",
            fp_microops,
        )
        self.assertIn("bits(SrcReg2, 11, 10)", fp_microops)
        self.assertIn("std::nearbyint(FpSrcReg1)", fp_microops)
        self.assertIn("std::floor(FpSrcReg1)", fp_microops)
        self.assertIn("std::ceil(FpSrcReg1)", fp_microops)
        self.assertIn("std::trunc(FpSrcReg1)", fp_microops)

    def test_hip_api_smoke_uses_public_hip_cumulative_stages(self):
        root = Path("tests/test-progs/gpu/hip-api-smoke")
        source = (root / "hip_api_smoke.hip").read_text()
        makefile = (root / "Makefile").read_text()
        preload_smoke = (
            root / "hip_memset_preload_smoke.hip"
        ).read_text()

        for stage in ("malloc", "memset", "memcpy", "launch", "lifecycle"):
            self.assertIn(f'"{stage}"', source)

        required_calls = (
            "hipGetDeviceCount",
            "hipSetDevice(0)",
            "hipMalloc",
            "hipMemset",
            "hipMemcpyHostToDevice",
            "hipMemcpyDeviceToHost",
            "hipGetLastError",
            "hipDeviceSynchronize",
            "hipFree",
        )
        for call in required_calls:
            self.assertIn(call, source)

        self.assertIn("transform_kernel<<<", source)
        self.assertIn("validate_memset", source)
        self.assertIn("validate_round_trip", source)
        self.assertIn("validate_kernel", source)
        self.assertIn("API_STAGE_PASSED stage=%s", source)
        self.assertIn("LIFECYCLE_PASSED", source)

        malloc_pos = source.index("run_malloc_stage")
        memset_pos = source.index("run_memset_stage")
        memcpy_pos = source.index("run_memcpy_stage")
        launch_pos = source.index("run_launch_stage")
        free_pos = source.index("HIP_CHECK(hipFree")
        lifecycle_pos = source.index('std::printf("LIFECYCLE_PASSED')
        return_pos = source.index("return 0;", lifecycle_pos)
        self.assertLess(malloc_pos, memset_pos)
        self.assertLess(memset_pos, memcpy_pos)
        self.assertLess(memcpy_pos, launch_pos)
        self.assertLess(launch_pos, free_pos)
        self.assertLess(free_pos, lifecycle_pos)
        self.assertLess(lifecycle_pos, return_pos)

        self.assertNotIn("#include <hsa/", source)
        self.assertNotIn("hsa_", source)
        self.assertNotIn("m5_exit", source)
        self.assertNotIn("m5_fail", source)
        self.assertNotIn("volatile const uint32_t *", source)

        self.assertIn("--offload-arch=gfx900", makefile)
        self.assertIn("-mno-code-object-v3", makefile)
        self.assertNotIn("m5op", source)
        self.assertNotIn("-lhsa-runtime64", makefile)
        self.assertNotIn("-lhsakmt", makefile)
        self.assertIn("hipMemset", preload_smoke)
        self.assertIn("volatile const", preload_smoke)
        self.assertNotIn("hipDeviceSynchronize", preload_smoke)
        self.assertNotIn("hipMemcpy", preload_smoke)
        self.assertIn("m5_exit", preload_smoke)
        self.assertIn("HIP_MEMSET_PRELOAD_PASSED", preload_smoke)
        self.assertIn("hip_memset_preload_smoke", makefile)

    def test_hip_malloc_smoke_contract(self):
        source = Path(
            "tests/test-progs/gpu/hip-api-smoke/hip_malloc_smoke.hip"
        ).read_text()
        makefile = Path(
            "tests/test-progs/gpu/hip-api-smoke/Makefile"
        ).read_text()

        self.assertIn("parse_device", source)
        self.assertIn('"--device"', source)
        self.assertIn("hipSetDevice(device)", source)
        self.assertIn("hipMalloc(&device_data", source)
        self.assertIn("HIP_API_MALLOC_PASSED device=%d", source)
        self.assertIn("m5_exit(0)", source)
        self.assertNotIn("hipFree(", source)
        self.assertIn("hip_malloc_smoke", makefile)

    def test_hip_memcpy_preload_smoke_contract(self):
        source = Path(
            "tests/test-progs/gpu/hip-api-smoke/hip_memcpy_preload_smoke.hip"
        ).read_text()
        makefile = Path(
            "tests/test-progs/gpu/hip-api-smoke/Makefile"
        ).read_text()

        self.assertIn("hipMemcpy(device_data, input", source)
        self.assertIn("hipMemcpy(output", source)
        self.assertIn("hipMemcpyHostToDevice", source)
        self.assertIn("hipMemcpyDeviceToHost", source)
        self.assertIn("HIP_MEMCPY_PRELOAD_PASSED", source)
        self.assertIn("m5_exit(0)", source)
        self.assertIn("hip_memcpy_preload_smoke", makefile)

    def test_se_hip_memset_compatibility_layer_contract(self):
        root = Path(
            "tests/test-progs/gpu/hip-api-smoke/se_hip_compat"
        )
        preload_smoke = Path(
            "tests/test-progs/gpu/hip-api-smoke/hip_memset_preload_smoke.hip"
        ).read_text()
        kernel = (root / "se_hip_memset.hip").read_text()
        wrapper = (root / "se_hip_compat.cpp").read_text()
        makefile = (root / "Makefile").read_text()

        self.assertIn('extern "C" __global__ void', kernel)
        self.assertIn("seHipMemsetKernel", kernel)
        self.assertIn('extern "C" hipError_t', wrapper)
        self.assertIn("hipMemset(void *dst", wrapper)
        self.assertIn("RTLD_NEXT", wrapper)
        self.assertIn("hsa_executable_load_agent_code_object", wrapper)
        self.assertIn("hsa_executable_get_symbol_by_name", wrapper)
        self.assertIn('"seHipMemsetKernel@kd"', wrapper)
        self.assertIn("hsa_queue_create", wrapper)
        self.assertIn("hsa_kernel_dispatch_packet_t", wrapper)
        self.assertIn("hsa_signal_wait_scacquire", wrapper)
        self.assertIn("SE_HIP_MEMSET_HSACO", wrapper)
        self.assertIn("--offload-arch=gfx900", makefile)
        self.assertIn("-mno-code-object-v3", makefile)
        self.assertIn("-shared", makefile)
        self.assertIn("HIP_MEMSET_PRELOAD_PASSED", preload_smoke)
        self.assertIn(
            "volatile const unsigned char *observed",
            preload_smoke,
        )
        self.assertIn("m5_exit(0)", preload_smoke)
        self.assertNotIn("hipMemcpy", preload_smoke)

    def test_se_hip_memcpy_compatibility_layer_contract(self):
        wrapper = Path(
            "tests/test-progs/gpu/hip-api-smoke/se_hip_compat/"
            "se_hip_compat.cpp"
        ).read_text()

        self.assertIn("hipMemcpy(void *dst", wrapper)
        self.assertIn("hipMemcpyHostToDevice", wrapper)
        self.assertIn("hipMemcpyDeviceToHost", wrapper)
        self.assertIn("hipMemcpyDeviceToDevice", wrapper)
        self.assertIn("hsa_amd_memory_async_copy", wrapper)
        self.assertIn("hsa_signal_wait_scacquire", wrapper)
        self.assertIn("RTLD_NEXT", wrapper)

    def test_peer_invalidate_diagnostic_has_host_sequenced_phases(self):
        root = Path("tests/test-progs/gpu/xgmi-peer-invalidate")
        host = (root / "invalidate_hip.cpp").read_text()
        kernels = (root / "invalidate_kernels.hip").read_text()
        makefile = (root / "Makefile").read_text()
        config = Path(
            "configs/example/gem5_library/"
            "x86-vega-xgmi-multigpu-se.py"
        ).read_text()
        peer_baseline = Path(
            "tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip.cpp"
        ).read_text()

        phase_markers = [
            'MARK("phase 1: GPU1 write A")',
            'MARK("phase 2: GPU0 read A")',
            'MARK("phase 3: GPU1 write B")',
            'MARK("phase 4: GPU0 read and classify")',
        ]
        phase_positions = [host.index(marker) for marker in phase_markers]
        self.assertEqual(phase_positions, sorted(phase_positions))

        self.assertIn('"write_value"', host)
        self.assertIn('"read_value"', host)
        self.assertIn('"classify_value"', host)
        self.assertIn("write_value(", kernels)
        self.assertIn("read_value(", kernels)
        self.assertIn("classify_value(", kernels)

        self.assertIn("invalidation observed", host)
        self.assertIn("stale cache line observed", host)
        self.assertIn("unexpected value observed", host)
        self.assertIn("setup read failed", host)
        self.assertIn("m5_exit(0)", host)
        self.assertIn("m5_fail(0, 1)", host)
        self.assertIn("m5_fail(0, 2)", host)
        self.assertIn("std::fflush(stdout)", host)

        first_read = host.index('MARK("phase 2: GPU0 read A")')
        second_write = host.index('MARK("phase 3: GPU1 write B")')
        second_read = host.index(
            'MARK("phase 4: GPU0 read and classify")'
        )
        result_read = host.index("volatile const uint32_t *cpu_results")
        first_exit = min(
            host.index("m5_exit(0)"),
            host.index("m5_fail(0, 1)"),
            host.index("m5_fail(0, 2)"),
        )
        self.assertLess(first_read, second_write)
        self.assertLess(second_write, second_read)
        self.assertLess(second_read, result_read)
        self.assertLess(result_read, first_exit)

        self.assertNotIn("hsa_queue_destroy(", host)
        self.assertNotIn("hsa_executable_destroy(", host)
        self.assertNotIn("hsa_code_object_reader_destroy(", host)
        self.assertNotIn("hipFree(", host)

        self.assertIn("util/m5/src/abi/x86/m5op.S", makefile)
        self.assertIn("-I$(GEM5_ROOT)/include", makefile)
        self.assertIn("-lm5op_x86", makefile)
        self.assertIn("--offload-arch=gfx900", makefile)

        self.assertIn(
            "--disable-gpu-kernel-launch-acquire",
            config,
        )
        self.assertIn(
            "gpus[gpu_index].impl_kern_launch_acq = False",
            config,
        )
        self.assertIn(
            'cause == "m5_fail instruction encountered"',
            config,
        )

        self.assertIn("peer VRAM verification passed", peer_baseline)
        self.assertNotIn("phase 1: GPU1 write A", peer_baseline)

    def test_peer_vram_result_uses_cpu_mapping_instead_of_hip_memcpy(self):
        test_program = Path(
            "tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip.cpp"
        ).read_text()

        self.assertNotIn("hipMemcpy(host_errors.data()", test_program)
        self.assertIn("volatile const uint32_t *cpu_errors", test_program)
        self.assertIn("host_errors[i] = cpu_errors[i]", test_program)
        self.assertNotIn("HIP_CHECK(hipFree(errors))", test_program)
        self.assertNotIn("HIP_CHECK(hipFree(gpu1_mem))", test_program)
        self.assertIn("#include <gem5/m5ops.h>", test_program)
        self.assertIn("m5_exit(0)", test_program)

        result_check = test_program.index("uint32_t error_count = 0")
        m5_exit = test_program.index("m5_exit(0)")
        success_output = test_program.index(
            'std::printf("peer VRAM verification passed'
        )
        self.assertLess(result_check, m5_exit)
        self.assertLess(success_output, m5_exit)
        self.assertNotIn("hsa_queue_destroy(", test_program)
        self.assertNotIn("hsa_executable_destroy(", test_program)
        self.assertNotIn("hsa_code_object_reader_destroy(", test_program)

        makefile = Path(
            "tests/test-progs/gpu/xgmi-peer-vram/Makefile"
        ).read_text()
        self.assertIn("util/m5/src/abi/x86/m5op.S", makefile)
        self.assertIn("-I$(GEM5_ROOT)/include", makefile)

    def test_direct_hsa_sdma_fill_smoke_uses_hsa_amd_memory_fill(self):
        root = Path("tests/test-progs/gpu/xgmi-peer-vram")
        source = (root / "hsa_sdma_fill.cpp").read_text()
        makefile = (root / "Makefile").read_text()

        self.assertIn("HSA_SDMA_FILL_TARGET", makefile)
        self.assertIn("hsa_sdma_fill.cpp", makefile)
        self.assertIn("$(HSA_SDMA_FILL_TARGET):", makefile)
        self.assertIn("$(HSA_CXXFLAGS)", makefile)
        self.assertIn("-lhsa-runtime64", makefile)
        self.assertIn("-lhsakmt", makefile)

        self.assertIn("hsa_init", source)
        self.assertIn("hsa_iterate_agents", source)
        self.assertIn("hsa_amd_agent_iterate_memory_pools", source)
        self.assertIn("hsa_amd_memory_pool_allocate", source)
        self.assertIn("hsa_amd_memory_fill", source)
        self.assertIn("hsa_amd_memory_pool_free", source)
        self.assertIn("HSA_SDMA_FILL_PASSED", source)
        self.assertIn("[hsa_sdma_fill] begin hsa_amd_memory_fill", source)
        self.assertNotIn("hipMemset", source)

    def test_direct_hsa_sdma_async_copy_smoke_uses_hsa_amd_memory_async_copy(self):
        root = Path("tests/test-progs/gpu/xgmi-peer-vram")
        source = (root / "hsa_sdma_async_copy.cpp").read_text()
        makefile = (root / "Makefile").read_text()

        self.assertIn("HSA_SDMA_ASYNC_COPY_TARGET", makefile)
        self.assertIn("hsa_sdma_async_copy.cpp", makefile)
        self.assertIn("$(HSA_SDMA_ASYNC_COPY_TARGET):", makefile)
        self.assertIn("$(HSA_CXXFLAGS)", makefile)
        self.assertIn("-lhsa-runtime64", makefile)
        self.assertIn("-lhsakmt", makefile)
        self.assertIn("$(HSA_SDMA_ASYNC_COPY_TARGET)", makefile)

        self.assertIn("hsa_init", source)
        self.assertIn("hsa_iterate_agents", source)
        self.assertIn("HSA_DEVICE_TYPE_CPU", source)
        self.assertIn("HSA_DEVICE_TYPE_GPU", source)
        self.assertIn("hsa_amd_agent_iterate_memory_pools", source)
        self.assertIn("hsa_amd_memory_pool_allocate", source)
        self.assertIn("hsa_amd_memory_async_copy", source)
        self.assertIn("hsa_signal_create", source)
        self.assertIn("hsa_signal_wait_scacquire", source)
        self.assertIn("HSA_SIGNAL_CONDITION_LT", source)
        self.assertIn("[hsa_sdma_async_copy] begin %s async copy", source)
        self.assertIn('"H2D"', source)
        self.assertIn('"D2H"', source)
        self.assertIn("HSA_SDMA_ASYNC_COPY_PASSED", source)
        self.assertNotIn("hsa_amd_memory_fill", source)
        self.assertNotIn("hipMemcpy", source)
        self.assertNotIn("hipMemset", source)

    def test_direct_hsa_sdma_peer_async_copy_smoke_uses_two_gpus(self):
        root = Path("tests/test-progs/gpu/xgmi-peer-vram")
        source = (root / "hsa_sdma_peer_async_copy.cpp").read_text()
        makefile = (root / "Makefile").read_text()

        self.assertIn("HSA_SDMA_PEER_ASYNC_COPY_TARGET", makefile)
        self.assertIn("hsa_sdma_peer_async_copy.cpp", makefile)
        self.assertIn("$(HSA_SDMA_PEER_ASYNC_COPY_TARGET):", makefile)
        self.assertIn("$(HSA_CXXFLAGS)", makefile)
        self.assertIn("-lhsa-runtime64", makefile)
        self.assertIn("-lhsakmt", makefile)
        self.assertIn("$(HSA_SDMA_PEER_ASYNC_COPY_TARGET)", makefile)

        self.assertIn("constexpr size_t CopyBytes = 64 * 1024", source)
        self.assertIn("std::vector<hsa_agent_t> gpus", source)
        self.assertIn("gpus.size() < 2", source)
        self.assertIn("gpu0_pool", source)
        self.assertIn("gpu1_pool", source)
        self.assertIn("gpu0_data", source)
        self.assertIn("gpu1_data", source)
        self.assertIn("hsa_amd_memory_async_copy", source)
        self.assertIn('"H2D_GPU0"', source)
        self.assertIn('"GPU0_TO_GPU1"', source)
        self.assertIn('"D2H_GPU1"', source)
        self.assertIn("HSA_SDMA_PEER_ASYNC_COPY_PASSED", source)
        self.assertNotIn("hipMemcpy", source)
        self.assertNotIn("hipMemset", source)

    def test_remote_write_after_cache_read_smoke_has_same_dispatch_reader(self):
        root = Path("tests/test-progs/gpu/xgmi-peer-vram")
        source = (root / "hsa_remote_cache_read.cpp").read_text()
        kernels = (root / "remote_cache_kernels.hip").read_text()
        makefile = (root / "Makefile").read_text()

        self.assertIn("HSA_REMOTE_CACHE_READ_TARGET", makefile)
        self.assertIn("hsa_remote_cache_read.cpp", makefile)
        self.assertIn("REMOTE_CACHE_HSACO_TARGET", makefile)
        self.assertIn("remote_cache_kernels.hip", makefile)
        self.assertIn("$(HSA_REMOTE_CACHE_READ_TARGET)", makefile)

        self.assertIn("std::vector<hsa_agent_t> gpus", source)
        self.assertIn("gpus.size() < 2", source)
        self.assertIn("hsa_queue_create", source)
        self.assertIn("remote_cache_reader", source)
        self.assertIn("hsa_amd_memory_async_copy", source)
        self.assertIn('"GPU0_TO_GPU1_OVERWRITE"', source)
        self.assertIn("REMOTE_CACHE_READ_PASSED_UPDATED", source)
        self.assertIn("REMOTE_CACHE_READ_OBSERVED_STALE", source)
        self.assertIn("REMOTE_CACHE_READ_FAILED_UNEXPECTED", source)
        self.assertIn("--mode", source)
        self.assertIn("REMOTE_CACHE_READ_REPEATED_ROUND", source)
        self.assertIn("REMOTE_CACHE_READ_REPEATED_PASSED_UPDATED", source)
        self.assertIn("REMOTE_CACHE_READ_REVERSE_RESULT", source)
        self.assertIn("REMOTE_CACHE_READ_REVERSE_PASSED_UPDATED", source)
        self.assertIn('"GPU1_TO_GPU0_OVERWRITE"', source)
        self.assertIn("REMOTE_CACHE_READ_MULTI_OFFSET_RESULT", source)
        self.assertIn("REMOTE_CACHE_READ_MULTI_OFFSET_PASSED_UPDATED", source)
        self.assertIn('"GPU0_TO_GPU1_MULTI_OFFSET_OVERWRITE"', source)
        self.assertIn("MultiOffsetDwords", source)
        self.assertIn("REMOTE_CACHE_READ_MULTI_LINE_RESULT", source)
        self.assertIn("REMOTE_CACHE_READ_MULTI_LINE_PASSED_UPDATED", source)
        self.assertIn('"GPU0_TO_GPU1_MULTI_LINE_OVERWRITE"', source)
        self.assertIn("MultiLineDwords", source)
        self.assertIn("REMOTE_CACHE_READ_REVERSE_MULTI_LINE_RESULT", source)
        self.assertIn("REMOTE_CACHE_READ_REVERSE_MULTI_LINE_PASSED_UPDATED",
                      source)
        self.assertIn('"GPU1_TO_GPU0_MULTI_LINE_OVERWRITE"', source)
        self.assertIn("REMOTE_CACHE_READ_LARGE_COPY_RESULT", source)
        self.assertIn("REMOTE_CACHE_READ_LARGE_COPY_PASSED_UPDATED", source)
        self.assertIn('"GPU0_TO_GPU1_LARGE_COPY_OVERWRITE"', source)
        self.assertIn("REMOTE_CACHE_READ_REVERSE_LARGE_COPY_RESULT", source)
        self.assertIn("REMOTE_CACHE_READ_REVERSE_LARGE_COPY_PASSED_UPDATED",
                      source)
        self.assertIn('"GPU1_TO_GPU0_LARGE_COPY_OVERWRITE"', source)
        self.assertIn("LargeCopyDwords", source)
        self.assertIn("REMOTE_CACHE_READ_SHADER_STORE_RESULT", source)
        self.assertIn("REMOTE_CACHE_READ_SHADER_STORE_PASSED_UPDATED",
                      source)
        self.assertIn("REMOTE_CACHE_READ_SHADER_STORE_OBSERVED_STALE",
                      source)
        self.assertIn('"GPU0_TO_GPU1_SHADER_STORE"', source)
        self.assertIn("REMOTE_CACHE_READ_REVERSE_SHADER_STORE_RESULT",
                      source)
        self.assertIn("REMOTE_CACHE_READ_REVERSE_SHADER_STORE_PASSED_UPDATED",
                      source)
        self.assertIn('"GPU1_TO_GPU0_SHADER_STORE"', source)
        self.assertIn("REMOTE_CACHE_READ_SHADER_MULTI_LINE_RESULT", source)
        self.assertIn("REMOTE_CACHE_READ_SHADER_MULTI_LINE_PASSED_UPDATED",
                      source)
        self.assertIn('"GPU0_TO_GPU1_SHADER_MULTI_LINE_STORE"', source)
        self.assertIn(
            "REMOTE_CACHE_READ_REVERSE_SHADER_MULTI_LINE_RESULT", source)
        self.assertIn(
            "REMOTE_CACHE_READ_REVERSE_SHADER_MULTI_LINE_PASSED_UPDATED",
            source)
        self.assertIn('"GPU1_TO_GPU0_SHADER_MULTI_LINE_STORE"', source)
        self.assertIn("REMOTE_CACHE_READ_SHADER_LARGE_RESULT", source)
        self.assertIn("REMOTE_CACHE_READ_SHADER_LARGE_PASSED_UPDATED",
                      source)
        self.assertIn('"GPU0_TO_GPU1_SHADER_LARGE_STORE"', source)
        self.assertIn("REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_RESULT",
                      source)
        self.assertIn("REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_PASSED_UPDATED",
                      source)
        self.assertIn('"GPU1_TO_GPU0_SHADER_LARGE_STORE"', source)
        self.assertIn("shader-large-repeated", source)
        self.assertIn("reverse-shader-large-repeated", source)
        self.assertIn("REMOTE_CACHE_READ_SHADER_LARGE_REPEATED_ROUND",
                      source)
        self.assertIn("REMOTE_CACHE_READ_SHADER_LARGE_REPEATED_RESULT",
                      source)
        self.assertIn(
            "REMOTE_CACHE_READ_SHADER_LARGE_REPEATED_PASSED_UPDATED",
            source)
        self.assertIn(
            "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_REPEATED_ROUND",
            source)
        self.assertIn(
            "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_REPEATED_RESULT",
            source)
        self.assertIn(
            "REMOTE_CACHE_READ_REVERSE_SHADER_LARGE_REPEATED_PASSED_UPDATED",
            source)
        self.assertIn("shader-separate-dispatch", source)
        self.assertIn("reverse-shader-separate-dispatch", source)
        self.assertIn("ModeShaderSeparateDispatch", source)
        self.assertIn("ModeReverseShaderSeparateDispatch", source)
        self.assertIn("run_remote_cache_shader_separate_dispatch_round",
                      source)
        self.assertIn("--disable-second-reader-launch-acquire", source)
        self.assertIn("disable_next_launch_acquire_marker_path", source)
        self.assertIn("request_disable_next_launch_acquire", source)
        self.assertIn("cleanup_disable_next_launch_acquire_markers", source)
        self.assertIn("REMOTE_CACHE_READ_SHADER_SEPARATE_DISPATCH_RESULT",
                      source)
        self.assertIn(
            "REMOTE_CACHE_READ_SHADER_SEPARATE_DISPATCH_PASSED_UPDATED",
            source)
        self.assertIn(
            "REMOTE_CACHE_READ_REVERSE_SHADER_SEPARATE_DISPATCH_RESULT",
            source)
        self.assertIn(
            "REMOTE_CACHE_READ_REVERSE_SHADER_SEPARATE_DISPATCH_PASSED_UPDATED",
            source)
        self.assertIn("second_updated=%u", source)
        self.assertIn("stale=%u", source)
        self.assertIn("unexpected=%u", source)
        self.assertIn("reverse_shader_vector", source)
        self.assertIn('"hsa_queue_create GPU1 writer"', source)
        self.assertNotIn("hipMemcpy", source)
        self.assertNotIn("hipMemset", source)

        self.assertIn("RemoteCacheControl", kernels)
        self.assertIn("extern \"C\" __global__ void remote_cache_reader",
                      kernels)
        self.assertIn("const uint32_t *target", kernels)
        self.assertIn("volatile RemoteCacheControl *control", kernels)
        self.assertIn("control->first_read_done = 1", kernels)
        self.assertIn("while (control->allow_second_read == 0", kernels)
        self.assertIn("const uint32_t second = target[0]", kernels)
        self.assertIn("remote_cache_multi_offset_reader", kernels)
        self.assertIn("remote_cache_snapshot_reader", kernels)
        self.assertIn("volatile uint32_t *values", kernels)
        self.assertIn("values[i] = target[i]", kernels)
        self.assertIn("remote_cache_writer", kernels)
        self.assertIn("remote_cache_multi_offset_writer", kernels)
        self.assertIn("for (uint32_t i = 0; i < count; ++i)", kernels)
        self.assertIn("first_values[i]", kernels)
        self.assertIn("second_values[i]", kernels)
        self.assertIn("*target = value", kernels)
        self.assertIn("target[i] = value_base + i", kernels)
        self.assertIn("RemoteCacheUpdated", kernels)
        self.assertIn("RemoteCacheStale", kernels)

        dispatcher = Path("src/gpu-compute/dispatcher.cc").read_text()
        self.assertIn("gem5-disable-next-gpu-launch-acquire-gpu", dispatcher)
        self.assertIn("#include \"base/output.hh\"", dispatcher)
        self.assertIn("simout.resolve", dispatcher)
        self.assertIn("\"fs/tmp/\"", dispatcher)
        self.assertIn("consumeNextLaunchAcquireDisableMarker", dispatcher)
        self.assertIn("Targeted launch acquire skip", dispatcher)
        self.assertIn("std::remove(marker.c_str())", dispatcher)
        self.assertIn("skipLaunchAcquire", dispatcher)

    def test_remote_shader_resident_flag_smoke_is_standalone(self):
        root = Path("tests/test-progs/gpu/xgmi-peer-vram")
        host = (root / "hsa_remote_shader_resident_flag.cpp").read_text()
        kernels = (root / "resident_flag_kernels.hip").read_text()
        makefile = (root / "Makefile").read_text()

        self.assertIn("hsa_remote_shader_resident_flag", makefile)
        self.assertIn("hsa_remote_shader_resident_flag.cpp", makefile)
        self.assertIn("resident_flag_kernels.hip", makefile)
        self.assertIn("resident_flag_kernels.hsaco", makefile)
        self.assertIn("RESIDENT_FLAG_HSACO_PATH", host)

        self.assertIn("ResidentFlagControl", host)
        self.assertIn("LayoutSameLine", host)
        self.assertIn("LayoutSplitLine", host)
        self.assertIn("SyncStrict", host)
        self.assertIn("SyncWriterFenceOnly", host)
        self.assertIn("SyncNoFence", host)
        self.assertIn('"--reverse"', host)
        self.assertIn('"--layout"', host)
        self.assertIn('"--sync"', host)
        self.assertIn("RESIDENT_FLAG_RESULT", host)
        self.assertIn("RESIDENT_FLAG_PASSED_UPDATED", host)
        self.assertIn("RESIDENT_FLAG_OBSERVED_STALE_VALUE", host)
        self.assertIn("RESIDENT_FLAG_FLAG_TIMEOUT", host)
        self.assertIn("RESIDENT_FLAG_FAILED_UNEXPECTED", host)
        self.assertNotIn("hipMemcpy", host)
        self.assertNotIn("hipMemset", host)

        self.assertIn("resident_flag_reader", kernels)
        self.assertIn("resident_flag_writer", kernels)
        self.assertIn("__threadfence_system", kernels)
        self.assertIn("value_dword", kernels)
        self.assertIn("flag_dword", kernels)
        self.assertIn("resident_flag_reader(const uint32_t *target", kernels)
        self.assertIn("volatile_load_u32(&target[flag_dword])", kernels)

        value_store = kernels.index("target[value_dword] = updated_value")
        first_fence = kernels.index("__threadfence_system", value_store)
        flag_store = kernels.index("target[flag_dword] = 1", first_fence)
        self.assertLess(value_store, first_fence)
        self.assertLess(first_fence, flag_store)

        first_read = kernels.index(
            "const uint32_t first = target[value_dword]"
        )
        flag_poll = kernels.index("target[flag_dword]", first_read)
        second_read = kernels.index(
            "const uint32_t second = target[value_dword]",
            flag_poll,
        )
        self.assertLess(first_read, flag_poll)
        self.assertLess(flag_poll, second_read)

    def test_xgmi_multigpu_config_can_disable_launch_acquire_per_gpu(self):
        config = Path(
            "configs/example/gem5_library/x86-vega-xgmi-multigpu-se.py"
        ).read_text()

        self.assertIn("--disable-gpu-kernel-launch-acquire", config)
        self.assertIn("action=\"append\"", config)
        self.assertIn("disabled_launch_acquire_gpus", config)
        self.assertIn("for gpu_index in disabled_launch_acquire_gpus", config)
        self.assertIn("gpus[gpu_index].impl_kern_launch_acq = False",
                      config)
        self.assertIn("cache-persistence diagnostics", config)
        self.assertNotIn("--disable-gpu0-kernel-launch-acquire", config)
        self.assertNotIn("args.disable_gpu0_kernel_launch_acquire", config)

    def test_gpu_viper_directory_completes_dma_write_when_l3hit_arrives_after_probe_acks(self):
        directory = Path(
            "src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm"
        ).read_text()

        self.assertIn("transition(BDW_PM, L3Hit, BDW_Pm)", directory)
        self.assertIn("transition(BDW_PM, ProbeAcksComplete, BDW_M)",
                      directory)
        self.assertIn("transition(BDW_M, MemData, U)", directory)

        match = re.search(
            r"transition\(BDW_M, L3Hit, U\) \{(?P<body>.*?)\n  \}",
            directory,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        body = match.group("body")
        expected_actions = [
            "wd_writeBackData;",
            "da_sendResponseDmaAck;",
            "wada_wakeUpAllDependentsAddr;",
            "dt_deallocateTBE;",
            "ptl_popTriggerQueue;",
        ]
        position = -1
        for action in expected_actions:
            next_position = body.find(action)
            self.assertGreater(next_position, position)
            position = next_position

    def test_se_viper_gpu_exposes_sdma_dma_port(self):
        se_viper_gpu = Path(
            "src/python/gem5/components/devices/gpus/se_viper_gpu.py"
        ).read_text()
        hsa_device = Path("src/dev/hsa/HSADevice.py").read_text()
        hsa_sconscript = Path("src/dev/hsa/SConscript").read_text()

        self.assertIn("SESDMAEngine", hsa_device)
        self.assertIn("SESDMAEngine", hsa_sconscript)
        self.assertIn("Source('se_sdma_engine.cc')", hsa_sconscript)
        self.assertIn("DebugFlag('SESDMAEngine')", hsa_sconscript)

        self.assertIn("SESDMAEngine", se_viper_gpu)
        self.assertIn("self.se_sdma_engine", se_viper_gpu)
        self.assertIn("gpuId=config.gpu_id", se_viper_gpu)
        self.assertIn(
            "self._cpu_dma_ports.append(self.se_sdma_engine.dma)",
            se_viper_gpu,
        )

    def test_kfd_queue_type_constants_match_ioctl_uapi(self):
        kfd_ioctl = Path("src/dev/hsa/kfd_ioctl.h").read_text()
        driver_hh = Path("src/gpu-compute/gpu_compute_driver.hh").read_text()
        driver_cc = Path("src/gpu-compute/gpu_compute_driver.cc").read_text()
        hsa_pp_hh = Path("src/dev/hsa/hsa_packet_processor.hh").read_text()

        self.assertIn("KFD_IOC_QUEUE_TYPE_COMPUTE      0", kfd_ioctl)
        self.assertIn("KFD_IOC_QUEUE_TYPE_SDMA         1", kfd_ioctl)
        self.assertIn("KFD_IOC_QUEUE_TYPE_COMPUTE_AQL  2", kfd_ioctl)
        self.assertIn("KFD_IOC_QUEUE_TYPE_SDMA_XGMI    3", kfd_ioctl)
        self.assertIn("KFD ioctl UAPI", kfd_ioctl)
        self.assertNotIn("HSA_QUEUE_TYPE ABI", kfd_ioctl)

        self.assertIn("enum class QueueBackend", driver_hh)
        self.assertIn("QueueBackend::Compute", driver_cc)
        self.assertIn("QueueBackend::Sdma", driver_cc)
        self.assertIn("queueIdToBackend", driver_hh)

        self.assertIn("AMDKFD_IOC_CREATE_QUEUE request", driver_cc)
        self.assertIn("AMDKFD_IOC_CREATE_QUEUE assigned", driver_cc)
        self.assertIn("backend %s", driver_cc)
        self.assertIn("doorbell_offset", driver_cc)
        self.assertIn("write_pointer_address", driver_cc)

        self.assertIn("KFD_IOC_QUEUE_TYPE_SDMA", driver_cc)
        self.assertIn("registerSDMAQueue", driver_cc)
        self.assertIn("unregisterSDMAQueue", driver_cc)
        self.assertIn("registerSDMAQueue", hsa_pp_hh)
        self.assertIn("unregisterSDMAQueue", hsa_pp_hh)

        self.assertIn("KFD_IOC_QUEUE_TYPE_SDMA_XGMI", driver_cc)
        self.assertNotIn("SE SDMA_XGMI queue is Phase 2", driver_cc)
        self.assertIn('backend_name = "sdma_xgmi"', driver_cc)

        sdma_case = driver_cc.index("KFD_IOC_QUEUE_TYPE_SDMA")
        sdma_register = driver_cc.index("registerSDMAQueue", sdma_case)
        next_compute_register = driver_cc.find(
            "setDeviceQueueDesc", sdma_case, sdma_register
        )
        self.assertEqual(next_compute_register, -1)

        sdma_xgmi_case = driver_cc.index("KFD_IOC_QUEUE_TYPE_SDMA_XGMI")
        sdma_xgmi_register = driver_cc.index(
            "registerSDMAQueue", sdma_xgmi_case
        )
        next_compute_register = driver_cc.find(
            "setDeviceQueueDesc", sdma_xgmi_case, sdma_xgmi_register
        )
        self.assertEqual(next_compute_register, -1)

    def test_hsa_packet_processor_routes_sdma_doorbells(self):
        hsa_pp_hh = Path("src/dev/hsa/hsa_packet_processor.hh").read_text()
        hsa_pp_cc = Path("src/dev/hsa/hsa_packet_processor.cc").read_text()
        se_sdma_hh = Path("src/dev/hsa/se_sdma_engine.hh").read_text()
        se_sdma_cc = Path("src/dev/hsa/se_sdma_engine.cc").read_text()

        for symbol in (
            "registerSDMAQueue",
            "unregisterSDMAQueue",
            "sdmaQueues",
        ):
            self.assertIn(symbol, hsa_pp_hh)
            self.assertIn(symbol, hsa_pp_cc)

        self.assertIn("writeDoorbell", se_sdma_hh)
        self.assertIn("writeDoorbell", hsa_pp_cc)
        self.assertIn("doorbell_size", hsa_pp_cc)
        self.assertIn("queue_id = daddr / doorbell_size", hsa_pp_cc)
        self.assertIn("sdmaQueues.find(queue_id)", hsa_pp_cc)
        self.assertIn("registerSDMAQueue queue_id", hsa_pp_cc)
        self.assertIn("setDeviceQueueDesc queue_id", hsa_pp_cc)
        self.assertIn("doorbell route daddr", hsa_pp_cc)
        self.assertIn("raw_value", hsa_pp_cc)
        self.assertIn("adjusted_value", hsa_pp_cc)
        self.assertIn("sdma_hit", hsa_pp_cc)
        self.assertIn("to SESDMAEngine", hsa_pp_cc)
        self.assertIn("to HWScheduler", hsa_pp_cc)
        self.assertIn("first_ring_dword", se_sdma_cc)

        sdma_lookup = hsa_pp_cc.index("sdmaQueues.find(queue_id)")
        sdma_doorbell = hsa_pp_cc.index("writeDoorbell", sdma_lookup)
        compute_doorbell = hsa_pp_cc.index("hwSchdlr->write", sdma_doorbell)
        self.assertLess(sdma_doorbell, compute_doorbell)

    def test_se_sdma_engine_has_phase1_packet_handlers(self):
        se_sdma_hh = Path("src/dev/hsa/se_sdma_engine.hh").read_text()
        se_sdma_cc = Path("src/dev/hsa/se_sdma_engine.cc").read_text()

        for opcode in (
            "SDMA_OP_NOP",
            "SDMA_OP_FENCE",
            "SDMA_OP_TRAP",
            "SDMA_OP_POLL_REGMEM",
            "SDMA_OP_CONST_FILL",
            "SDMA_OP_COPY",
            "SDMA_OP_ATOMIC",
            "SDMA_ATOMIC_ADD64",
            "SDMA_SUBOP_WRITE_LINEAR",
            "SDMA_SUBOP_COPY_LINEAR",
        ):
            self.assertIn(opcode, se_sdma_cc)

        for method in (
            "processQueue",
            "decodeHeader",
            "finishPacket",
            "executeFence",
            "executeTrap",
            "executePollRegMem",
            "executePollRegMemData",
            "pollRegMemFunc",
            "executeConstFill",
            "executeCopy",
            "executeAtomic",
            "dmaAtomicAddr",
            "dumpRingDwords",
            "dumpRingDwordsData",
            "writeDoorbell",
            "translateRange",
        ):
            self.assertIn(method, se_sdma_cc)

        self.assertIn("TranslationGenPtr translate", se_sdma_hh)
        self.assertIn("dmaReadVirt", se_sdma_cc)
        self.assertIn("dmaWriteVirt", se_sdma_cc)
        self.assertIn("unsupported opcode", se_sdma_cc)
        self.assertIn("unsupported WRITE sub-opcode", se_sdma_cc)
        self.assertIn("unsupported COPY sub-opcode", se_sdma_cc)
        self.assertIn("unsupported ATOMIC opcode", se_sdma_cc)
        self.assertIn("unsupported POLL_REGMEM operation", se_sdma_cc)
        self.assertIn("unsupported POLL_REGMEM comparison function", se_sdma_cc)
        self.assertIn("if (header == 0)", se_sdma_cc)
        self.assertIn("empty ring space", se_sdma_cc)
        self.assertIn("queue.readIndex = queue.writeIndex", se_sdma_cc)
        self.assertIn("dumpedDoorbellRing", se_sdma_hh)
        self.assertIn("dumpedZeroHeaderRing", se_sdma_hh)
        self.assertIn("lastDoorbellIndex", se_sdma_hh)
        self.assertIn("last_doorbell", se_sdma_cc)
        self.assertIn("doorbell ring base", se_sdma_cc)
        self.assertIn("zero header neighborhood", se_sdma_cc)
        self.assertIn("after unsupported WRITE variant", se_sdma_cc)
        self.assertIn("SDMA ring dump queue", se_sdma_cc)

        zero_header = se_sdma_cc.index("if (header == 0)")
        header_advance = se_sdma_cc.index(
            "advanceReadIndex(queue, sizeof(uint32_t))",
            zero_header,
        )
        self.assertLess(zero_header, header_advance)

    def test_se_sdma_engine_pio_port_is_connected_to_iobus(self):
        se_viper_gpu = Path(
            "src/python/gem5/components/devices/gpus/se_viper_gpu.py"
        ).read_text()

        connect_iobus = se_viper_gpu[
            se_viper_gpu.index("def connect_iobus")
        :]
        self.assertIn("self.gpu_cmd_proc.pio = iobus.mem_side_ports",
                      connect_iobus)
        self.assertIn("self.gpu_cmd_proc.hsapp.pio = iobus.mem_side_ports",
                      connect_iobus)
        self.assertIn("self.se_sdma_engine.pio = iobus.mem_side_ports",
                      connect_iobus)


if __name__ == "__main__":
    unittest.main()
