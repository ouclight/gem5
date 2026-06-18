import pathlib
import re
import unittest


class TestGpuViperWbL2Transitions(unittest.TestCase):
    def test_l3_hit_after_probe_completion_finishes_dma_read(self):
        protocol = (
            pathlib.Path(__file__).parents[2]
            / "src/mem/ruby/protocol/MOESI_AMD_Base-dir.sm"
        ).read_text()

        transition = re.search(
            r"transition\(BL2_M,\s*L3Hit,\s*U\)\s*\{(?P<body>.*?)\n\s*\}",
            protocol,
            re.DOTALL,
        )
        self.assertIsNotNone(
            transition,
            "WB L2 must handle an L3 hit arriving after probe completion",
        )

        body = transition.group("body")
        for action in (
            "dd_sendResponseDmaData",
            "wada_wakeUpAllDependentsAddr",
            "pd_popDmaRequestQueue",
            "dt_deallocateTBE",
            "ptl_popTriggerQueue",
        ):
            self.assertIn(action, body)

    def test_se_kernarg_preload_uses_process_address_space(self):
        command_processor = (
            pathlib.Path(__file__).parents[2]
            / "src/gpu-compute/gpu_command_processor.cc"
        ).read_text()

        read_preload = re.search(
            r"GPUCommandProcessor::readPreload\(.*?\)\n\{"
            r"(?P<body>.*?)\n\}\n\nvoid\n"
            r"GPUCommandProcessor::initPreload",
            command_processor,
            re.DOTALL,
        )
        self.assertIsNotNone(read_preload)
        body = read_preload.group("body")
        self.assertIn("if (!FullSystem)", body)
        self.assertIn("SETranslatingPortProxy virt_proxy(tc)", body)
        self.assertIn("virt_proxy.readBlob(", body)
        self.assertIn("initPreload(akc, task)", body)
        self.assertIn("return;", body)

    def test_only_gfx900_ignores_new_descriptor_preload_fields(self):
        command_processor = (
            pathlib.Path(__file__).parents[2]
            / "src/gpu-compute/gpu_command_processor.cc"
        ).read_text()

        dispatch = re.search(
            r"GPUCommandProcessor::dispatchKernelObject\(.*?\)\n\{"
            r"(?P<body>.*?)\n\}\n\nvoid\n"
            r"GPUCommandProcessor::sendCompletionSignal",
            command_processor,
            re.DOTALL,
        )
        self.assertIsNotNone(dispatch)
        body = dispatch.group("body")
        legacy_guard = re.search(
            r"if \((?P<condition>.*?)\) \{\s*"
            r"akc->kernarg_preload_spec_length = 0;",
            body,
            re.DOTALL,
        )
        self.assertIsNotNone(legacy_guard)
        condition = " ".join(legacy_guard.group("condition").split())
        self.assertEqual(
            condition,
            "gfxVersion == GfxVersion::gfx900",
        )
        self.assertIn("akc->kernarg_preload_spec_length = 0", body)
        self.assertIn("akc->kernarg_preload_spec_offset = 0", body)

    def test_peer_vram_result_uses_cpu_mapped_gpu1_vram(self):
        benchmark = (
            pathlib.Path(__file__).parents[2]
            / "tests/test-progs/gpu/xgmi-peer-vram/peer_vram_hip.cpp"
        ).read_text()

        gpu1_select = benchmark.index("HIP_CHECK(hipSetDevice(1))")
        errors_alloc = benchmark.index(
            "HIP_CHECK(hipMalloc(&errors, bytes))"
        )
        gpu0_select = benchmark.index("HIP_CHECK(hipSetDevice(0))")

        self.assertLess(gpu1_select, errors_alloc)
        self.assertLess(errors_alloc, gpu0_select)
        self.assertIn("volatile const uint32_t *cpu_errors", benchmark)
        self.assertIn("host_errors[i] = cpu_errors[i]", benchmark)
        self.assertNotIn("hipMemcpy(host_errors.data()", benchmark)


if __name__ == "__main__":
    unittest.main()
