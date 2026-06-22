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


class SEViperMultiGPUTest(unittest.TestCase):
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

    def test_se_hip_memset_compatibility_layer_contract(self):
        root = Path(
            "tests/test-progs/gpu/hip-api-smoke/se_hip_compat"
        )
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
            "--disable-gpu0-kernel-launch-acquire",
            config,
        )
        self.assertIn(
            "gpus[0].impl_kern_launch_acq = False",
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


if __name__ == "__main__":
    unittest.main()
