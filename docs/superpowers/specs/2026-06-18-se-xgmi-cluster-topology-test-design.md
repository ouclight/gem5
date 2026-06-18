# SE XGMI Cluster Topology Behavior Test Design

## Objective

Add a direct behavior test for the Ruby network topology used by the SE-mode
multi-GPU platform. The test must validate the constructed
`ClusteredXGMINetwork`, not the unrelated generic `SimplePt2Pt` topology.

## Required Topology

The network contains one cluster for the CPU and one cluster for each GPU:

- router 0 owns all CPU-cluster Ruby controllers;
- router `i + 1` owns all Ruby controllers for GPU `i`;
- the CPU router has bidirectional links to every GPU router;
- every pair of distinct GPU routers has bidirectional XGMI links;
- each Ruby controller has exactly one external link to its cluster router.

CPU-to-GPU links use the configured PCIe latency and weight. GPU-to-GPU links
use the configured XGMI latency and weight.

## Test Construction

The test will instantiate a `ClusteredXGMINetwork` with one CPU cluster and
three GPU clusters. It will use distinguishable controller counts per cluster
so incorrect controller slicing or router assignment is observable.

The test will call `connect()` and inspect the resulting routers, external
links, and internal links.

## Assertions

The test will verify:

1. Four routers exist with IDs `0..3`.
2. Every controller is connected to exactly one router.
3. CPU controllers connect only to router 0.
4. Controllers for GPU `i` connect only to router `i + 1`.
5. CPU-to-GPU internal links contain both directions for every GPU.
6. GPU-to-GPU internal links form a complete directed graph without
   self-links.
7. CPU-to-GPU links have the configured PCIe latency and weight.
8. GPU-to-GPU links have the configured XGMI latency and weight.
9. External and internal link IDs are globally unique and contiguous.

## Scope

This change modifies only the regression test unless the direct test exposes a
defect in `ClusteredXGMINetwork`. It does not change the validated topology,
retest the completed peer-VRAM simulation, or use `SimplePt2Pt` as a proxy for
the SE multi-GPU network.

The existing `_cluster_int_link_specs()` helper test may remain as a focused
unit test, but the direct network behavior test is the authoritative topology
regression.

## Verification

Verification will consist of:

- a focused run of the new direct topology test;
- the complete `test_se_viper_multigpu` lightweight suite;
- `git diff --check`.

No gem5 rebuild or full simulation is required.
