# Copyright (c) 2026
# All rights reserved.

from __future__ import annotations

"""Pure helpers for SE XGMI multi-GPU Ruby network construction."""

from dataclasses import dataclass
from typing import List

from m5.objects import (
    SimpleExtLink,
    SimpleIntLink,
    SimpleNetwork,
    Switch,
)


@dataclass(frozen=True)
class ClusterIntLinkSpec:
    src_router: int
    dst_router: int
    latency: int
    weight: int


def _cluster_int_link_specs(
    *,
    num_gpus: int,
    pcie_link_latency: int = 32,
    pcie_link_weight: int = 4,
    xgmi_link_latency: int = 8,
    xgmi_link_weight: int = 1,
) -> List[ClusterIntLinkSpec]:
    """Return unidirectional router-router links for CPU + full-mesh GPUs.

    Router 0 is the CPU cluster. Router i + 1 is GPU i.
    """

    if num_gpus < 1:
        raise ValueError("num_gpus must be positive")

    specs: List[ClusterIntLinkSpec] = []
    gpu_router_ids = range(1, num_gpus + 1)

    for gpu_router in gpu_router_ids:
        specs.append(
            ClusterIntLinkSpec(
                src_router=0,
                dst_router=gpu_router,
                latency=pcie_link_latency,
                weight=pcie_link_weight,
            )
        )
        specs.append(
            ClusterIntLinkSpec(
                src_router=gpu_router,
                dst_router=0,
                latency=pcie_link_latency,
                weight=pcie_link_weight,
            )
        )

    for src_router in gpu_router_ids:
        for dst_router in gpu_router_ids:
            if src_router == dst_router:
                continue
            specs.append(
                ClusterIntLinkSpec(
                    src_router=src_router,
                    dst_router=dst_router,
                    latency=xgmi_link_latency,
                    weight=xgmi_link_weight,
                )
            )

    return specs


class ClusteredXGMINetwork(SimpleNetwork):
    """CPU/GPU-clustered SimpleNetwork for SE XGMI multi-GPU systems."""

    def __init__(
        self,
        ruby_system,
        *,
        cpu_controller_count,
        gpu_controller_counts,
        pcie_link_latency=32,
        pcie_link_weight=4,
        xgmi_link_latency=8,
        xgmi_link_weight=1,
    ):
        super().__init__()
        self.netifs = []
        self.ruby_system = ruby_system
        self._cpu_controller_count = cpu_controller_count
        self._gpu_controller_counts = list(gpu_controller_counts)
        self._pcie_link_latency = pcie_link_latency
        self._pcie_link_weight = pcie_link_weight
        self._xgmi_link_latency = xgmi_link_latency
        self._xgmi_link_weight = xgmi_link_weight

    def set_cluster_counts(self, cpu_controller_count, gpu_controller_counts):
        self._cpu_controller_count = cpu_controller_count
        self._gpu_controller_counts = list(gpu_controller_counts)

    def connect(self, controllers):
        expected = self._cpu_controller_count + sum(
            self._gpu_controller_counts
        )
        if len(controllers) != expected:
            raise ValueError(
                f"Expected {expected} Ruby controllers, got "
                f"{len(controllers)}"
            )

        self.routers = [
            Switch(router_id=i)
            for i in range(1 + len(self._gpu_controller_counts))
        ]

        ext_links = []
        link_id = 0
        controller_index = 0
        for _ in range(self._cpu_controller_count):
            ext_links.append(
                SimpleExtLink(
                    link_id=link_id,
                    ext_node=controllers[controller_index],
                    int_node=self.routers[0],
                )
            )
            link_id += 1
            controller_index += 1

        for gpu_index, gpu_count in enumerate(self._gpu_controller_counts):
            router = self.routers[gpu_index + 1]
            for _ in range(gpu_count):
                ext_links.append(
                    SimpleExtLink(
                        link_id=link_id,
                        ext_node=controllers[controller_index],
                        int_node=router,
                    )
                )
                link_id += 1
                controller_index += 1

        self.ext_links = ext_links

        int_links = []
        for spec in _cluster_int_link_specs(
            num_gpus=len(self._gpu_controller_counts),
            pcie_link_latency=self._pcie_link_latency,
            pcie_link_weight=self._pcie_link_weight,
            xgmi_link_latency=self._xgmi_link_latency,
            xgmi_link_weight=self._xgmi_link_weight,
        ):
            int_links.append(
                SimpleIntLink(
                    link_id=link_id,
                    src_node=self.routers[spec.src_router],
                    dst_node=self.routers[spec.dst_router],
                    latency=spec.latency,
                    weight=spec.weight,
                )
            )
            link_id += 1

        self.int_links = int_links
