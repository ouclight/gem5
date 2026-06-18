# SE XGMI Cluster Topology Behavior Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a direct behavior regression test proving that the SE multi-GPU Ruby network constructs one CPU cluster, one cluster per GPU, PCIe CPU-to-GPU links, and a full-mesh XGMI GPU network.

**Architecture:** Load `se_xgmi_network.py` directly from the worktree so the test observes current Python source without requiring a gem5 rebuild. Instantiate `ClusteredXGMINetwork` with distinguishable controller counts, call `connect()`, and inspect the resulting routers and links rather than treating `SimplePt2Pt` or the link-spec helper as a topology proxy.

**Tech Stack:** gem5 embedded Python, Python `unittest`, Ruby `SimpleNetwork` SimObjects.

---

## File Structure

- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
  - Add one worktree-module loader and one direct topology behavior test.
- Modify: `docs/debug/se-multigpu-status.md`
  - Record exact focused/full commands, first result, confirmed topology facts,
    verification, files changed, and the next diagnostic step.
- No production topology source is expected to change.

### Task 1: Add the Direct ClusteredXGMINetwork Behavior Test

**Files:**

- Modify: `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- Reference: `src/python/gem5/prebuilt/viper/se_xgmi_network.py`
- Test: `tests/pyunit/stdlib/test_se_viper_multigpu.py`

- [ ] **Step 1: Add imports for loading the current worktree module**

Add these imports at the top of `tests/pyunit/stdlib/test_se_viper_multigpu.py`:

```python
import importlib.util
import re
import sys
import tempfile
import unittest
from pathlib import Path
```

`sys.modules` registration is required before executing this module because
Python 3.8's `@dataclass` processing resolves the defining module by name.

- [ ] **Step 2: Add a focused worktree-module loader**

Add this helper below the existing imports:

```python
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
```

This intentionally loads only the topology module from the worktree. It avoids
testing a stale Python copy embedded in an existing `gem5.opt`.

- [ ] **Step 3: Add the direct topology behavior test**

Add this method to `SEViperMultiGPUTest` after
`test_clustered_xgmi_network_helper_uses_bidirectional_int_links`:

```python
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
```

The controller counts `[2, 1, 2, 3]` make cluster-boundary errors observable;
equal-sized clusters would not catch all slicing mistakes.

- [ ] **Step 4: Run the focused test against the current implementation**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_clustered_xgmi_network_constructs_cluster_topology
```

Expected result:

```text
Ran 1 test

OK
```

This is a characterization test for an already implemented and full-simulation
validated topology. No production mutation is required to manufacture a RED
state. If it fails, preserve the first assertion/error and investigate whether
the implementation or the inspection assumptions differ before changing code.

- [ ] **Step 5: Run the complete lightweight multi-GPU suite**

Run:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

Expected result: all tests pass, including both the focused link-spec helper
test and the new authoritative constructed-topology test.

- [ ] **Step 6: Check the complete worktree diff for whitespace errors**

Run:

```bash
git diff --check
```

Expected result: no output and exit status 0.

### Task 2: Record the Verification Handoff

**Files:**

- Modify: `docs/debug/se-multigpu-status.md`

- [ ] **Step 1: Append the exact verification record**

Append a status entry containing:

```markdown
### Direct ClusteredXGMINetwork Behavior Test

The direct topology regression constructs two CPU controllers and three GPU
clusters containing one, two, and three controllers. It loads
`se_xgmi_network.py` from the worktree, calls `ClusteredXGMINetwork.connect()`,
and inspects the resulting SimObjects.

Commands:

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest \
  test_se_viper_multigpu.SEViperMultiGPUTest.test_clustered_xgmi_network_constructs_cluster_topology
```

```bash
build/VEGA_X86/gem5.opt \
  -p tests/pyunit/stdlib \
  -m unittest test_se_viper_multigpu
```

```bash
git diff --check
```

First relevant result:

```text
Ran 1 test

OK
```

Facts confirmed:

- router 0 owns every CPU controller;
- router `i + 1` owns every controller for GPU `i`;
- CPU and GPU routers have both directed PCIe links;
- distinct GPU routers form a complete directed XGMI graph;
- PCIe and XGMI links retain their configured latency and weight;
- external and internal link IDs are globally unique and contiguous.

Files changed:

- `tests/pyunit/stdlib/test_se_viper_multigpu.py`
- `docs/debug/se-multigpu-status.md`

Verification completed:

- focused direct topology test;
- complete lightweight SE multi-GPU Python suite;
- `git diff --check`.

Next diagnostic step: none for the established cluster topology. Select the
next protocol behavior to validate before adding another end-to-end workload.
```

If the observed output differs, record the exact first relevant result instead
of the anticipated successful result above. Do not paste full logs.

- [ ] **Step 2: Review only the scoped diff**

Run:

```bash
git diff -- \
  tests/pyunit/stdlib/test_se_viper_multigpu.py \
  docs/debug/se-multigpu-status.md \
  docs/superpowers/specs/2026-06-18-se-xgmi-cluster-topology-test-design.md \
  docs/superpowers/plans/2026-06-18-se-xgmi-cluster-topology-test.md
```

Expected result: only the approved direct topology test, design/plan
documentation, and exact status handoff are present.

No commit is included in this plan because the repository guidance says not to
commit the user's uncommitted work unless explicitly requested.
