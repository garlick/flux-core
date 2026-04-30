###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################

"""AffinityPool: Rv1Pool extension for topology-aware GPU+core allocation.

When a slot requests GPUs (``gpu_per_slot > 0``), each slot's cores and GPUs
are allocated from the finest topology level that can satisfy the request, so
they share the tightest available PCIe affinity.  CPU-only slots use best-fit
packing at the finest fitting level to preserve intact groups for GPU jobs.

Topology is read from the ``scheduling.topology`` key of R.  Each rank entry
is a nested structure of named levels (e.g. ``"socket"`` → ``"numa"``).
Leaf nodes carry ``"cores"`` (and optionally ``"gpus"``).  Any nesting depth
is supported; the scheduler auto-discovers fine-to-coarse levels::

    {
      "scheduling": {
        "writer": "AffinityPool",
        "topology": {
          "0": {"socket": [
                    {"numa": [{"cores": "0-14",  "gpus": "0"},
                              {"cores": "15-29", "gpus": "1"}]},
                    {"numa": [{"cores": "30-44", "gpus": "2"},
                              {"cores": "45-59", "gpus": "3"}]}
                ]}
        }
      }
    }

The outer key is a broker rank string (or IDset range).  ``cores`` and
``gpus`` are RFC 22 IDset strings using node-local IDs.  A leaf entry
without a ``gpus`` key has no GPUs.

For each rank the scheduler builds a list of levels ordered finest→coarsest.
Level 0 is the leaf granularity (e.g. NUMA node); deeper levels are unions of
their children (e.g. socket, then whole node).  Adjacent identical levels
(e.g. when a socket has exactly one NUMA child) are deduplicated.

GPU slots: try the finest level first; fall through to coarser levels
(including cross-socket) on a best-effort basis so that resources are never
withheld merely because tight affinity cannot be satisfied.

CPU slots: best-fit (fewest-available-first) within the finest level where any
group can accommodate the slot, preserving intact groups for GPU jobs.

Allocated R carries a trimmed ``topology`` containing only the allocated ranks
so that a sub-instance scheduler inherits only its portion of the topology.
The sub-instance resource module re-ranks nodes from 0; :class:`_AffinityPoolV1`
re-maps topology rank keys to match by sorted-zip (same pattern as RackPool).
Core and GPU IDs within each group are node-local and do not change.

The pool is selected via ``scheduling.writer`` auto-discovery or explicitly::

    flux module load sched-simple pool-class=AffinityPool
"""

import json
from collections.abc import Mapping

from flux.idset import IDset
from flux.resource import InsufficientResources
from flux.resource.ResourcePool import ResourcePool
from flux.resource.Rv1Pool import Rv1Pool


def _extract_levels(node):
    """Recursively extract affinity groups at each topology level.

    Returns ``(levels, total_cores, total_gpus)`` where ``levels[0]`` is the
    finest granularity and ``levels[-1]`` is a synthetic whole-node group
    (union of all children at this node).  Each entry in *levels* is a list
    of ``(cores_frozenset, gpus_frozenset)`` groups.

    Adjacent identical levels (e.g. a node with a single child whose resources
    equal the parent's union) are deduplicated so that the synthetic whole-node
    level is only present when it is genuinely coarser than the level below.
    """
    if "cores" in node:
        cores = frozenset(IDset(node["cores"]))
        gpus = frozenset(IDset(node["gpus"])) if "gpus" in node else frozenset()
        return [[(cores, gpus)]], cores, gpus

    merged = []
    total_cores = frozenset()
    total_gpus = frozenset()

    for val in node.values():
        if not isinstance(val, list):
            continue
        for child in val:
            if not isinstance(child, dict):
                continue
            child_levels, cc, cg = _extract_levels(child)
            total_cores |= cc
            total_gpus |= cg
            for i, groups in enumerate(child_levels):
                if i >= len(merged):
                    merged.append([])
                merged[i].extend(groups)

    if total_cores:
        this_group = (total_cores, total_gpus)
        # Skip if this level is identical to the one just below (e.g. a node
        # with a single child whose union equals the parent).
        if not (merged and set(merged[-1]) == {this_group}):
            merged.append([this_group])

    return merged, total_cores, total_gpus


def _parse_topology(topology):
    """Return a dict mapping rank (int) -> list-of-levels.

    *topology* is the ``scheduling.topology`` dict from R.  Keys may be
    single rank strings (``"0"``) or IDset range strings (``"0-15"``).

    Each rank maps to a list of levels ordered finest→coarsest; each level
    is a list of ``(cores_frozenset, gpus_frozenset)`` groups.
    """
    groups_by_rank = {}
    for rank_str, rank_data in (topology or {}).items():
        levels, _, _ = _extract_levels(rank_data)
        if levels:
            for rank in IDset(rank_str):
                groups_by_rank[rank] = levels
    return groups_by_rank


class _AffinityPoolV1(Rv1Pool):
    """Rv1Pool subclass that provides best-effort affinity-local core+GPU allocation."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        if self.scheduling is None:
            raise ValueError("AffinityPool requires R.scheduling")
        scheduling = dict(self.scheduling)
        topology = scheduling.get("topology")
        if not topology:
            raise ValueError("AffinityPool requires R.scheduling.topology")

        raw_groups = _parse_topology(topology)
        if not raw_groups:
            raise ValueError("AffinityPool: topology contains no groups")

        # Re-map topology rank keys to pool-rank space.  The sub-instance
        # resource module re-ranks allocated nodes from 0, so the n-th rank
        # in topology (sorted ascending) maps to the n-th pool rank.
        sorted_orig = sorted(raw_groups)
        if len(sorted_orig) != len(self._ranks):
            raise ValueError(
                f"AffinityPool: rank count mismatch: "
                f"{len(sorted_orig)} in topology vs {len(self._ranks)} in pool"
            )
        rank_remap = dict(zip(sorted_orig, range(len(self._ranks))))
        self._levels = {rank_remap[orig]: levels for orig, levels in raw_groups.items()}
        # Rewrite topology with remapped per-rank keys, expanding any IDset
        # range keys (e.g. "0-15") into individual rank entries.
        new_topology = {}
        for rank_str, rank_data in topology.items():
            for orig_rank in IDset(rank_str):
                if orig_rank in rank_remap:
                    new_topology[str(rank_remap[orig_rank])] = rank_data
        scheduling["topology"] = new_topology
        self.scheduling = scheduling

    def alloc(self, jobid, request):
        result = super().alloc(jobid, request)
        # Trim topology to allocated ranks so sub-instance inherits only its slice.
        if result.scheduling:
            result.scheduling = dict(result.scheduling)
            allocated = {str(r) for r in result._ranks}
            result.scheduling["topology"] = {
                k: v
                for k, v in result.scheduling.get("topology", {}).items()
                if k in allocated
            }
        return result

    def _affinity_alloc_from_rank(
        self, rank, free_cores, free_gpus, request, max_slots=None
    ):
        """Allocate affinity-local GPU slots from one rank.

        Iterates levels finest→coarsest on a best-effort basis.  Within each
        level, iterates groups in order, filling as many slots as each group
        allows.  Slots are preferentially placed at the finest level that fits,
        falling back to coarser levels (including cross-socket) when necessary.
        Stops at *max_slots* if given.
        Returns ``(alloc_cores, alloc_gpus, nslots_taken)``.
        """
        slot_size = request.slot_size
        gpu_per_slot = request.gpu_per_slot
        remaining_cores = set(free_cores)
        remaining_gpus = set(free_gpus)
        rank_cores = set()
        rank_gpus = set()
        nslots = 0

        for level_groups in self._levels.get(rank, []):
            for group_cores, group_gpus in level_groups:
                avail_cores = remaining_cores & group_cores
                avail_gpus = remaining_gpus & group_gpus
                while len(avail_cores) >= slot_size and len(avail_gpus) >= gpu_per_slot:
                    if max_slots is not None and nslots >= max_slots:
                        return frozenset(rank_cores), frozenset(rank_gpus), nslots
                    taken_cores = frozenset(sorted(avail_cores)[:slot_size])
                    taken_gpus = frozenset(sorted(avail_gpus)[:gpu_per_slot])
                    rank_cores |= taken_cores
                    rank_gpus |= taken_gpus
                    remaining_cores -= taken_cores
                    remaining_gpus -= taken_gpus
                    avail_cores -= taken_cores
                    avail_gpus -= taken_gpus
                    nslots += 1

        return frozenset(rank_cores), frozenset(rank_gpus), nslots

    def _cpu_alloc_from_rank(self, rank, free_cores, request, max_slots=None):
        """Allocate CPU-only slots from one rank.

        Finds the finest level where any group can accommodate *slot_size*
        cores, then uses best-fit (fewest-available-first) within that level
        to preserve intact groups for future GPU jobs.
        Returns ``(alloc_cores, nslots_taken)``.
        """
        slot_size = request.slot_size
        remaining = set(free_cores)
        rank_cores = set()
        nslots = 0

        chosen_groups = None
        for level_groups in self._levels.get(rank, []):
            if any(len(remaining & gc) >= slot_size for gc, _gg in level_groups):
                chosen_groups = level_groups
                break

        if chosen_groups is None:
            return frozenset(), 0

        # Best-fit: sort ascending by available cores (fewest first) so slots
        # land in already-fragmented groups, preserving intact groups for GPU jobs.
        scored = sorted(
            ((len(remaining & gc), gc) for gc, _gg in chosen_groups),
            key=lambda t: t[0],
        )
        for _avail, group_cores in scored:
            avail = remaining & group_cores
            while len(avail) >= slot_size:
                if max_slots is not None and nslots >= max_slots:
                    return frozenset(rank_cores), nslots
                taken = frozenset(sorted(avail)[:slot_size])
                rank_cores |= taken
                remaining -= taken
                avail -= taken
                nslots += 1

        return frozenset(rank_cores), nslots

    def _select_resources(self, candidates, request):
        """Select candidates with affinity-local core+GPU pairing for GPU slots."""
        if request.exclusive or not self._levels:
            return super()._select_resources(candidates, request)

        if request.gpu_per_slot == 0:
            return self._cpu_select(candidates, request)

        return self._gpu_select(candidates, request)

    def _cpu_select(self, candidates, request):
        """CPU-only selection using finest-level best-fit packing."""
        nnodes = request.nnodes
        nslots = request.nslots
        selected = []

        if nnodes > 0:
            slots_per_node = nslots // nnodes
            nnodes_target = request.nnodes_max
            for rank, info, free_cores, free_gpus in candidates:
                if nnodes_target is not None and len(selected) >= nnodes_target:
                    break
                alloc_cores, n = self._cpu_alloc_from_rank(
                    rank, free_cores, request, max_slots=slots_per_node
                )
                if n < slots_per_node:
                    continue
                selected.append((rank, info, alloc_cores, frozenset()))
            if len(selected) < nnodes:
                self._check_feasibility(request)
                raise InsufficientResources("insufficient resources")
            actual_nslots = slots_per_node * len(selected)
        else:
            nslots_target = request.nslots_max
            allocated_slots = 0
            for rank, info, free_cores, free_gpus in candidates:
                if nslots_target is not None and allocated_slots >= nslots_target:
                    break
                max_slots = (
                    nslots_target - allocated_slots
                    if nslots_target is not None
                    else None
                )
                alloc_cores, n = self._cpu_alloc_from_rank(
                    rank, free_cores, request, max_slots=max_slots
                )
                if n > 0:
                    selected.append((rank, info, alloc_cores, frozenset()))
                    allocated_slots += n
            if allocated_slots < nslots:
                self._check_feasibility(request)
                raise InsufficientResources("insufficient resources")
            actual_nslots = allocated_slots

        return selected, actual_nslots

    def _gpu_select(self, candidates, request):
        """GPU slot selection with affinity-local core+GPU pairing."""
        nnodes = request.nnodes
        nslots = request.nslots
        selected = []

        if nnodes > 0:
            slots_per_node = nslots // nnodes
            nnodes_target = request.nnodes_max
            for rank, info, free_cores, free_gpus in candidates:
                if nnodes_target is not None and len(selected) >= nnodes_target:
                    break
                alloc_cores, alloc_gpus, n = self._affinity_alloc_from_rank(
                    rank, free_cores, free_gpus, request, max_slots=slots_per_node
                )
                if n < slots_per_node:
                    continue
                selected.append((rank, info, alloc_cores, alloc_gpus))
            if len(selected) < nnodes:
                self._check_feasibility(request)
                raise InsufficientResources("insufficient resources")
            actual_nslots = slots_per_node * len(selected)
        else:
            nslots_target = request.nslots_max
            allocated_slots = 0
            for rank, info, free_cores, free_gpus in candidates:
                if nslots_target is not None and allocated_slots >= nslots_target:
                    break
                max_slots = (
                    nslots_target - allocated_slots
                    if nslots_target is not None
                    else None
                )
                alloc_cores, alloc_gpus, n = self._affinity_alloc_from_rank(
                    rank, free_cores, free_gpus, request, max_slots=max_slots
                )
                if n > 0:
                    selected.append((rank, info, alloc_cores, alloc_gpus))
                    allocated_slots += n
            if allocated_slots < nslots:
                self._check_feasibility(request)
                raise InsufficientResources("insufficient resources")
            actual_nslots = allocated_slots

        return selected, actual_nslots


class AffinityPool(ResourcePool):
    """Version-dispatching affinity pool.

    Wraps :class:`_AffinityPoolV1` for Rv1 resources.  Set
    :attr:`~flux.scheduler.Scheduler.pool_class` to this class or pass
    ``pool-class=AffinityPool`` to sched-simple.

    To support a future R version, add a version-specific implementation
    class and extend ``_impl_map``.
    """

    _impl_map = {1: _AffinityPoolV1}

    def __init__(self, R, log=None, **kwargs):
        if isinstance(R, str):
            R = json.loads(R)
        version = R.get("version", 1) if isinstance(R, Mapping) else 1
        impl_class = self._impl_map.get(version)
        if impl_class is None:
            raise ValueError(f"R version {version} not supported by AffinityPool")
        super().__init__(impl_class(R, log=log))


pool_class = AffinityPool
