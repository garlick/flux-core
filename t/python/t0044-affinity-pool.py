#!/usr/bin/env python3
###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

"""Unit tests for AffinityPool: NUMA-affinity GPU+core allocation."""

import copy
import json
import unittest

import subflux  # noqa: F401,E402 - configures PYTHONPATH for flux imports
from flux.resource import InfeasibleRequest, InsufficientResources  # noqa: E402
from flux.resource.AffinityPool import AffinityPool  # noqa: E402
from flux.resource.ResourceCount import ResourceCount  # noqa: E402
from flux.resource.Rv1Pool import ResourceRequest  # noqa: E402
from pycotap import TAPTestRunner  # noqa: E402

_UNSET = object()


def rr(
    nnodes=0,
    nslots=1,
    slot_size=1,
    gpu_per_slot=0,
    exclusive=False,
    nnodes_max=_UNSET,
    nslots_max=_UNSET,
):
    """Build a ResourceRequest from flat parameters."""
    if nnodes_max is _UNSET:
        nnodes_max = nnodes
    if nslots_max is _UNSET:
        nslots_max = nslots
    if nnodes > 0:
        spn = nslots // nnodes
        node_count = ResourceCount(nnodes, nnodes_max)
        slot_count = ResourceCount(spn, spn)
    else:
        node_count = None
        slot_count = ResourceCount(nslots, nslots_max)
    return ResourceRequest(
        node_count, slot_count, slot_size, gpu_per_slot, 0.0, None, exclusive, None
    )


# 2 nodes, 8 cores (0-7) + 2 GPUs (0-1) each.
# 2 sockets per node, each with 1 NUMA child:
#   socket 0: cores 0-3, gpu 0
#   socket 1: cores 4-7, gpu 1
R_2NODE = {
    "version": 1,
    "execution": {
        "R_lite": [{"rank": "0-1", "children": {"core": "0-7", "gpu": "0-1"}}],
        "starttime": 0,
        "expiration": 0,
        "nodelist": ["gpu0", "gpu1"],
    },
    "scheduling": {
        "writer": "AffinityPool",
        "topology": {
            "0": {
                "socket": [
                    {"numa": [{"cores": "0-3", "gpus": "0"}]},
                    {"numa": [{"cores": "4-7", "gpus": "1"}]},
                ]
            },
            "1": {
                "socket": [
                    {"numa": [{"cores": "0-3", "gpus": "0"}]},
                    {"numa": [{"cores": "4-7", "gpus": "1"}]},
                ]
            },
        },
    },
}

# 1 node, 2 sockets, each with 4 cores + 1 GPU.
R_1NODE = {
    "version": 1,
    "execution": {
        "R_lite": [{"rank": "0", "children": {"core": "0-7", "gpu": "0-1"}}],
        "starttime": 0,
        "expiration": 0,
        "nodelist": ["gpu0"],
    },
    "scheduling": {
        "writer": "AffinityPool",
        "topology": {
            "0": {
                "socket": [
                    {"numa": [{"cores": "0-3", "gpus": "0"}]},
                    {"numa": [{"cores": "4-7", "gpus": "1"}]},
                ]
            },
        },
    },
}


class TestAffinityPoolConstruct(unittest.TestCase):
    def test_valid_init(self):
        pool = AffinityPool(copy.deepcopy(R_2NODE))
        self.assertIn(0, pool.impl._levels)
        self.assertIn(1, pool.impl._levels)

    def test_from_json_string(self):
        pool = AffinityPool(json.dumps(R_2NODE))
        self.assertEqual(len(pool.impl._levels), 2)

    def test_missing_scheduling_raises(self):
        R = copy.deepcopy(R_2NODE)
        del R["scheduling"]
        with self.assertRaises(ValueError):
            AffinityPool(R)

    def test_missing_topology_raises(self):
        R = copy.deepcopy(R_2NODE)
        R["scheduling"] = {"writer": "AffinityPool"}
        with self.assertRaises(ValueError):
            AffinityPool(R)

    def test_empty_topology_raises(self):
        R = copy.deepcopy(R_2NODE)
        R["scheduling"]["topology"] = {}
        with self.assertRaises(ValueError):
            AffinityPool(R)

    def test_rank_count_mismatch_raises(self):
        R = copy.deepcopy(R_2NODE)
        R["scheduling"]["topology"]["2"] = {
            "socket": [{"numa": [{"cores": "0-3", "gpus": "0"}]}]
        }
        with self.assertRaises(ValueError):
            AffinityPool(R)

    def test_topology_rank_keys_remapped(self):
        """Topology rank keys are re-mapped to match pool-rank space."""
        R = copy.deepcopy(R_2NODE)
        # Shift original ranks 0,1 → 10,11 in topology; pool still has 2 ranks.
        topo = R["scheduling"]["topology"]
        topo["10"] = topo.pop("0")
        topo["11"] = topo.pop("1")
        pool = AffinityPool(R)
        # After remapping: pool ranks are 0,1; _levels keys must be 0,1.
        self.assertIn(0, pool.impl._levels)
        self.assertIn(1, pool.impl._levels)
        self.assertNotIn(10, pool.impl._levels)

    def test_idset_range_key_equivalent_to_per_rank(self):
        """A single IDset range key covers all ranks identically to per-rank keys."""
        sockets = [
            {"numa": [{"cores": "0-3", "gpus": "0"}]},
            {"numa": [{"cores": "4-7", "gpus": "1"}]},
        ]
        R_range = copy.deepcopy(R_2NODE)
        R_range["scheduling"]["topology"] = {"0-1": {"socket": sockets}}
        pool_range = AffinityPool(R_range)
        pool_perrank = AffinityPool(copy.deepcopy(R_2NODE))
        self.assertEqual(pool_range.impl._levels, pool_perrank.impl._levels)

    def test_idset_range_key_with_remapping(self):
        """IDset range key with non-zero original ranks is correctly re-mapped."""
        sockets = [
            {"numa": [{"cores": "0-3", "gpus": "0"}]},
            {"numa": [{"cores": "4-7", "gpus": "1"}]},
        ]
        R = copy.deepcopy(R_2NODE)
        # Topology uses ranks 10-11; pool still has 2 ranks (0-1 in pool space).
        R["scheduling"]["topology"] = {"10-11": {"socket": sockets}}
        pool = AffinityPool(R)
        # sorted-zip maps 10→0, 11→1; _levels must use pool ranks.
        self.assertIn(0, pool.impl._levels)
        self.assertIn(1, pool.impl._levels)
        self.assertNotIn(10, pool.impl._levels)
        self.assertNotIn(11, pool.impl._levels)
        # Rewritten topology keys must also be in pool-rank space.
        topo_keys = set(pool.impl.scheduling["topology"].keys())
        self.assertEqual(topo_keys, {"0", "1"})


class TestAffinityPoolAlloc(unittest.TestCase):
    def setUp(self):
        self.pool = AffinityPool(copy.deepcopy(R_2NODE))

    def _assert_affinity(self, result):
        """Assert each rank's allocated GPUs and cores share an affinity group."""
        for rank, rinfo in result.impl._ranks.items():
            alloc_cores = rinfo["cores"]
            alloc_gpus = rinfo["gpus"]
            if not alloc_gpus:
                continue
            levels = self.pool.impl._levels.get(rank, [])
            self.assertTrue(
                any(
                    alloc_cores <= g_cores and alloc_gpus <= g_gpus
                    for level_groups in levels
                    for g_cores, g_gpus in level_groups
                ),
                f"rank {rank}: cores {alloc_cores} and gpus {alloc_gpus} "
                f"violate affinity",
            )

    def test_single_gpu_slot_affinity(self):
        """1-core 1-GPU slot: core and GPU must be in the same socket."""
        result = self.pool.alloc(1, rr(nslots=1, slot_size=1, gpu_per_slot=1))
        self._assert_affinity(result)

    def test_full_group_slot_affinity(self):
        """4-core 1-GPU slot: fills one socket, affinity holds."""
        result = self.pool.alloc(1, rr(nslots=1, slot_size=4, gpu_per_slot=1))
        self._assert_affinity(result)
        rank_info = list(result.impl._ranks.values())[0]
        self.assertEqual(len(rank_info["gpus"]), 1)

    def test_cpu_only_ignores_affinity(self):
        """CPU-only request succeeds without NUMA constraint."""
        result = self.pool.alloc(1, rr(nslots=1, slot_size=8, gpu_per_slot=0))
        self.assertEqual(len(result.impl._ranks), 1)
        rank_info = list(result.impl._ranks.values())[0]
        self.assertEqual(len(rank_info["cores"]), 8)
        self.assertEqual(len(rank_info["gpus"]), 0)

    def test_cpu_only_packs_into_partial_group(self):
        """CPU-only slots prefer partially-used groups, leaving intact groups free."""
        # Use 1-node pool so group membership is unambiguous.
        pool = AffinityPool(copy.deepcopy(R_1NODE))
        # Partially consume group 0 (cores 0-3) with a 2-core CPU-only slot.
        pool.alloc(1, rr(nslots=1, slot_size=2, gpu_per_slot=0))
        # Now allocate another 2-core CPU-only slot.  Best-fit should prefer
        # the already-partial group 0 (2 free cores) over intact group 1 (4 free cores).
        result2 = pool.alloc(2, rr(nslots=1, slot_size=2, gpu_per_slot=0))
        cores2 = result2.impl._ranks[0]["cores"]
        # Group 0 spans 0-3; group 1 spans 4-7.  Best-fit lands in group 0.
        self.assertTrue(
            cores2 <= frozenset(range(4)),
            f"expected cores in group 0 (0-3), got {sorted(cores2)}",
        )

    def test_cpu_only_span_group_falls_back_to_base(self):
        """CPU-only slot larger than any single group falls back to base worst-fit."""
        pool = AffinityPool(copy.deepcopy(R_1NODE))
        # slot_size=6 > 4 (max group size) → base worst-fit; must still succeed.
        result = pool.alloc(1, rr(nslots=1, slot_size=6, gpu_per_slot=0))
        rank_info = result.impl._ranks[0]
        self.assertEqual(len(rank_info["cores"]), 6)
        self.assertEqual(len(rank_info["gpus"]), 0)

    def test_two_slots_fill_one_node(self):
        """Two (4-core, 1-GPU) slots consume both sockets on one node."""
        result = self.pool.alloc(1, rr(nslots=2, slot_size=4, gpu_per_slot=1))
        self.assertEqual(len(result.impl._ranks), 1)
        rank_info = list(result.impl._ranks.values())[0]
        self.assertEqual(len(rank_info["cores"]), 8)
        self.assertEqual(len(rank_info["gpus"]), 2)

    def test_node_based_gpu_request(self):
        """Node-based (nnodes=1) GPU request uses affinity allocation."""
        result = self.pool.alloc(1, rr(nnodes=1, nslots=1, slot_size=4, gpu_per_slot=1))
        self._assert_affinity(result)
        self.assertEqual(len(result.impl._ranks), 1)

    def test_cross_socket_gpu_slot_uses_whole_node(self):
        """5-core+1-GPU slot exceeds any socket group but succeeds via whole-node fallback."""
        result = self.pool.alloc(1, rr(nslots=1, slot_size=5, gpu_per_slot=1))
        rank_info = list(result.impl._ranks.values())[0]
        self.assertEqual(len(rank_info["cores"]), 5)
        self.assertEqual(len(rank_info["gpus"]), 1)

    def test_infeasible_slot_too_many_gpus(self):
        """1-core+3-GPU slot is infeasible (groups have at most 1 GPU)."""
        with self.assertRaises(InfeasibleRequest):
            self.pool.alloc(1, rr(nslots=1, slot_size=1, gpu_per_slot=3))

    def test_cpu_only_oversized_is_infeasible(self):
        """CPU-only job exceeding total capacity raises InfeasibleRequest."""
        with self.assertRaises(InfeasibleRequest):
            self.pool.alloc(1, rr(nslots=1, slot_size=64, gpu_per_slot=0))

    def test_topology_trimmed_to_allocated_ranks(self):
        """Allocated R's topology contains only the ranks used by the job."""
        result = self.pool.alloc(1, rr(nnodes=1, nslots=1, slot_size=1, gpu_per_slot=1))
        allocated_ranks = {str(r) for r in result.impl._ranks}
        topo_keys = set(result.impl.scheduling.get("topology", {}).keys())
        self.assertEqual(topo_keys, allocated_ranks)

    def test_full_allocation_topology_complete(self):
        """Allocating all nodes preserves all topology rank keys."""
        result = self.pool.alloc(1, rr(nnodes=2, nslots=2, slot_size=1, gpu_per_slot=1))
        topo_keys = set(result.impl.scheduling.get("topology", {}).keys())
        self.assertEqual(topo_keys, {"0", "1"})

    def test_sequential_single_gpu_allocations_affinity(self):
        """Two sequential single-slot GPU allocations are each affinity-local."""
        r1 = self.pool.alloc(1, rr(nslots=1, slot_size=4, gpu_per_slot=1))
        r2 = self.pool.alloc(2, rr(nslots=1, slot_size=4, gpu_per_slot=1))
        self._assert_affinity(r1)
        self._assert_affinity(r2)
        # The two allocations must not overlap
        for rank in set(r1.impl._ranks) & set(r2.impl._ranks):
            self.assertTrue(
                r1.impl._ranks[rank]["cores"].isdisjoint(r2.impl._ranks[rank]["cores"])
            )

    def test_insufficient_after_full_allocation(self):
        """Slot request fails with InsufficientResources when pool is exhausted."""
        self.pool.alloc(1, rr(nslots=2, slot_size=4, gpu_per_slot=1))
        self.pool.alloc(2, rr(nslots=2, slot_size=4, gpu_per_slot=1))
        with self.assertRaises(InsufficientResources):
            self.pool.alloc(3, rr(nslots=1, slot_size=1, gpu_per_slot=1))

    def test_check_feasibility_infeasible(self):
        """check_feasibility raises InfeasibleRequest when GPUs exceed node capacity."""
        req = rr(nslots=1, slot_size=1, gpu_per_slot=3)
        with self.assertRaises(InfeasibleRequest):
            self.pool.check_feasibility(req)

    def test_check_feasibility_feasible(self):
        """check_feasibility passes for a request that fits within a socket."""
        req = rr(nslots=1, slot_size=4, gpu_per_slot=1)
        self.pool.check_feasibility(req)  # must not raise


class TestAffinityPoolSingleNode(unittest.TestCase):
    """Tests against a single-node pool to simplify rank reasoning."""

    def setUp(self):
        self.pool = AffinityPool(copy.deepcopy(R_1NODE))

    def _assert_affinity(self, result):
        for rank, rinfo in result.impl._ranks.items():
            alloc_cores = rinfo["cores"]
            alloc_gpus = rinfo["gpus"]
            if not alloc_gpus:
                continue
            levels = self.pool.impl._levels.get(rank, [])
            self.assertTrue(
                any(
                    alloc_cores <= g_cores and alloc_gpus <= g_gpus
                    for level_groups in levels
                    for g_cores, g_gpus in level_groups
                ),
                f"rank {rank}: cores {alloc_cores} gpus {alloc_gpus} "
                f"not in one affinity group",
            )

    def test_group0_allocation(self):
        """First 4-core+1-GPU slot uses group 0 (cores 0-3, GPU 0)."""
        result = self.pool.alloc(1, rr(nslots=1, slot_size=4, gpu_per_slot=1))
        self._assert_affinity(result)
        rinfo = result.impl._ranks[0]
        gpus = rinfo["gpus"]
        cores = rinfo["cores"]
        if 0 in gpus:
            self.assertTrue(cores <= frozenset(range(4)))
        else:
            self.assertTrue(cores <= frozenset(range(4, 8)))

    def test_second_slot_uses_other_group(self):
        """After first group is used, second allocation uses the other group."""
        r1 = self.pool.alloc(1, rr(nslots=1, slot_size=4, gpu_per_slot=1))
        r2 = self.pool.alloc(2, rr(nslots=1, slot_size=4, gpu_per_slot=1))
        self._assert_affinity(r1)
        self._assert_affinity(r2)
        cores1 = r1.impl._ranks[0]["cores"]
        cores2 = r2.impl._ranks[0]["cores"]
        self.assertTrue(cores1.isdisjoint(cores2), "second alloc overlaps first")

    def test_third_slot_insufficient(self):
        """Third slot request fails after both groups are consumed."""
        self.pool.alloc(1, rr(nslots=1, slot_size=4, gpu_per_slot=1))
        self.pool.alloc(2, rr(nslots=1, slot_size=4, gpu_per_slot=1))
        with self.assertRaises(InsufficientResources):
            self.pool.alloc(3, rr(nslots=1, slot_size=1, gpu_per_slot=1))

    def test_cpu_packing_preserves_group_for_gpu_job(self):
        """CPU-only packing leaves an intact group available for a subsequent GPU job."""
        # Partially fill group 0 (cores 0-3) with a 2-core CPU-only slot.
        # Best-fit targets group 0 (equal availability → stable sort picks first).
        self.pool.alloc(1, rr(nslots=1, slot_size=2, gpu_per_slot=0))
        # Group 1 (cores 4-7, GPU 1) is intact; a 4-core+1-GPU slot must succeed.
        result = self.pool.alloc(2, rr(nslots=1, slot_size=4, gpu_per_slot=1))
        self._assert_affinity(result)
        rinfo = result.impl._ranks[0]
        # Must have landed in group 1 (cores 4-7).
        self.assertTrue(
            rinfo["cores"] <= frozenset(range(4, 8)),
            f"GPU job should use intact group 1, got cores {sorted(rinfo['cores'])}",
        )

    def test_topology_trimmed_single_node(self):
        """Single-node pool: topology trimmed to rank 0."""
        result = self.pool.alloc(1, rr(nslots=1, slot_size=1, gpu_per_slot=1))
        topo = result.impl.scheduling.get("topology", {})
        self.assertEqual(list(topo.keys()), ["0"])


if __name__ == "__main__":
    unittest.main(testRunner=TAPTestRunner())
