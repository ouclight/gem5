# Copyright (c) 2026
# All rights reserved.

from __future__ import annotations

"""SE-mode ROCm KFD topology helpers for VIPER multi-GPU runs.

The helpers in this module intentionally avoid importing ``m5`` so topology
generation can be unit-tested with the host Python interpreter.
"""

from dataclasses import dataclass
from pathlib import Path
import shutil
from typing import (
    Iterable,
    Sequence,
)


@dataclass(frozen=True)
class KfdGpuNode:
    node_id: int
    gpu_id: int
    render_minor: int
    vram_pool_id: int
    local_mem_size: int
    hive_id: int = 1
    name: str = "gfx900"
    num_sdma_xgmi_engines: int = 1


def default_xgmi_gpu_nodes(
    num_gpus: int,
    gpu_memory_size: int,
    base_gpu_id: int = 22124,
    base_render_minor: int = 128,
    hive_id: int = 1,
) -> list[KfdGpuNode]:
    if num_gpus < 1:
        raise ValueError("num_gpus must be positive")

    return [
        KfdGpuNode(
            node_id=gpu_index + 1,
            gpu_id=base_gpu_id + gpu_index,
            render_minor=base_render_minor + gpu_index,
            vram_pool_id=gpu_index + 1,
            local_mem_size=gpu_memory_size,
            hive_id=hive_id,
        )
        for gpu_index in range(num_gpus)
    ]


def _write_properties(path: Path, properties: Iterable[tuple[str, object]]):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as file:
        for key, value in properties:
            file.write(f"{key} {value}\n")


def _write_text(path: Path, data: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(data, encoding="utf-8")


def write_xgmi_topology(
    root: Path,
    gpu_nodes: Sequence[KfdGpuNode],
    cpu_memory_size: int,
    *,
    cpu_node_id: int = 0,
    cpu_cores_count: int = 1,
):
    if not gpu_nodes:
        raise ValueError("At least one GPU node is required")

    topology_root = root / "fs/sys/devices/virtual/kfd/kfd/topology"
    if topology_root.exists():
        shutil.rmtree(topology_root)
    nodes_root = topology_root / "nodes"
    nodes_root.mkdir(parents=True)

    _write_text(topology_root / "generation_id", "2\n")
    _write_text(
        topology_root / "system_properties",
        "platform_oem 35498446626881\n"
        "platform_id 71791775140929\n"
        "platform_rev 2\n",
    )

    _write_text(nodes_root / str(cpu_node_id) / "gpu_id", "0\n")
    _write_text(nodes_root / str(cpu_node_id) / "name", "\n")
    _write_properties(
        nodes_root / str(cpu_node_id) / "properties",
        [
            ("cpu_cores_count", cpu_cores_count),
            ("simd_count", 0),
            ("mem_banks_count", 1),
            ("caches_count", 1),
            ("io_links_count", len(gpu_nodes)),
            ("cpu_core_id_base", 0),
            ("simd_id_base", 0),
            ("max_waves_per_simd", 0),
            ("lds_size_in_kb", 0),
            ("gds_size_in_kb", 0),
            ("wave_front_size", 64),
            ("array_count", 0),
            ("simd_arrays_per_engine", 0),
            ("cu_per_simd_array", 0),
            ("simd_per_cu", 0),
            ("max_slots_scratch_cu", 0),
            ("vendor_id", 0),
            ("device_id", 0),
            ("location_id", 0),
            ("drm_render_minor", 0),
            ("max_engine_clk_ccompute", 3400),
        ],
    )
    _write_properties(
        nodes_root / str(cpu_node_id) / "caches/0/properties",
        [
            ("processor_id_low", 0),
            ("level", 3),
            ("size", 8388608),
            ("cache_line_size", 64),
            ("cache_lines_per_tag", 1),
            ("association", 16),
            ("latency", 20),
            ("type", 7),
            ("sibling_map", 1),
        ],
    )
    _write_properties(
        nodes_root / str(cpu_node_id) / "mem_banks/0/properties",
        [
            ("heap_type", 0),
            ("size_in_bytes", cpu_memory_size),
            ("flags", 0),
            ("width", 72),
            ("mem_clk_max", 2400),
        ],
    )

    for peer_index, gpu in enumerate(gpu_nodes):
        _write_properties(
            nodes_root
            / str(cpu_node_id)
            / "io_links"
            / str(peer_index)
            / "properties",
            [
                ("type", 2),
                ("version_major", 0),
                ("version_minor", 0),
                ("node_from", cpu_node_id),
                ("node_to", gpu.node_id),
                ("weight", 20),
                ("min_latency", 0),
                ("max_latency", 0),
                ("min_bandwidth", 0),
                ("max_bandwidth", 0),
                ("recommended_transfer_size", 0),
                ("flags", 13),
            ],
        )

    for gpu_index, gpu in enumerate(gpu_nodes):
        _write_text(nodes_root / str(gpu.node_id) / "gpu_id", f"{gpu.gpu_id}\n")
        _write_text(nodes_root / str(gpu.node_id) / "name", "Vega\n")
        _write_properties(
            nodes_root / str(gpu.node_id) / "properties",
            [
                ("cpu_cores_count", 0),
                ("simd_count", 256),
                ("mem_banks_count", 1),
                ("caches_count", 0),
                ("io_links_count", len(gpu_nodes)),
                ("cpu_core_id_base", 0),
                ("simd_id_base", 2147487744),
                ("max_waves_per_simd", 10),
                ("lds_size_in_kb", 64),
                ("gds_size_in_kb", 0),
                ("wave_front_size", 64),
                ("array_count", 4),
                ("simd_arrays_per_engine", 1),
                ("cu_per_simd_array", 16),
                ("simd_per_cu", 4),
                ("max_slots_scratch_cu", 40),
                ("vendor_id", 0x1002),
                ("device_id", 0x6860),
                ("location_id", 1024 + gpu_index),
                ("domain", 0),
                ("drm_render_minor", gpu.render_minor),
                ("num_sdma_engines", 2),
                ("num_sdma_xgmi_engines", gpu.num_sdma_xgmi_engines),
                ("num_sdma_queues_per_engine", 8),
                ("num_cp_queues", 8),
                ("num_gws", 64),
                ("hive_id", gpu.hive_id),
                ("unique_id", gpu.gpu_id),
                ("max_engine_clk_fcompute", 1500),
                ("local_mem_size", gpu.local_mem_size),
                ("fw_version", 421),
                ("capability", 238208),
                ("debug_prop", 32768),
                ("sdma_fw_version", 430),
                ("max_engine_clk_ccompute", 3400),
            ],
        )
        _write_properties(
            nodes_root / str(gpu.node_id) / "mem_banks/0/properties",
            [
                ("heap_type", 1),
                ("size_in_bytes", gpu.local_mem_size),
                ("flags", 0),
                ("width", 2048),
                ("mem_clk_max", 945),
                ("vram_pool_id", gpu.vram_pool_id),
            ],
        )

        _write_properties(
            nodes_root / str(gpu.node_id) / "io_links/0/properties",
            [
                ("type", 2),
                ("version_major", 0),
                ("version_minor", 0),
                ("node_from", gpu.node_id),
                ("node_to", cpu_node_id),
                ("weight", 20),
                ("min_latency", 0),
                ("max_latency", 0),
                ("min_bandwidth", 0),
                ("max_bandwidth", 0),
                ("recommended_transfer_size", 0),
                ("flags", 1),
            ],
        )

        link_index = 1
        for peer in gpu_nodes:
            if peer.node_id == gpu.node_id:
                continue
            _write_properties(
                nodes_root
                / str(gpu.node_id)
                / "io_links"
                / str(link_index)
                / "properties",
                [
                    ("type", 11),
                    ("version_major", 1),
                    ("version_minor", 0),
                    ("node_from", gpu.node_id),
                    ("node_to", peer.node_id),
                    ("weight", 5),
                    ("min_latency", 1),
                    ("max_latency", 5),
                    ("min_bandwidth", 64),
                    ("max_bandwidth", 128),
                    ("recommended_transfer_size", 0),
                    ("flags", 1),
                ],
            )
            link_index += 1

    _write_text(root / "fs/sys/module/amdgpu/parameters/vm_size", "256\n")
    _write_text(root / "fs/sys/module/amdgpu/parameters/ppfeaturemask", "0\n")
    _write_text(root / "fs/sys/module/amdgpu/parameters/sched_policy", "0\n")
