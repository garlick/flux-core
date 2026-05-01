###############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
###############################################################
"""flux-affinity-tree — show AffinityPool topology for a job allocation.

Usage:
    flux affinity-tree JOBID [JOBID ...]

Reads the job's R and prints the topology from R.scheduling.topology
as a text tree.  Leaves (NUMA nodes) that contributed cores or GPUs
to the allocation are highlighted in green.

When R.scheduling.topology is absent or the writer is not recognised,
a flat summary (cores and GPUs per rank) is shown instead.

Example output for a 1-core + 1-GPU slot on rank 0 of a 2-socket node::

    rank 0 (gpu0)  [1 core, 1 GPU allocated]
    ├─ socket 0
    │  ├─ numa 0: cores 0-14  gpu 0  ← core 0, gpu 0
    │  ├─ numa 1: cores 15-29  gpu 1
    │  ├─ numa 2: cores 30-44  gpu 2
    │  └─ numa 3: cores 45-59  gpu 3
    └─ socket 1
       ├─ numa 0: cores 60-74  gpu 4
       ├─ numa 1: cores 75-89  gpu 5
       ├─ numa 2: cores 90-104  gpu 6
       └─ numa 3: cores 105-119  gpu 7
"""

import json
import subprocess
import sys

from flux.idset import IDset

_RESET = "\033[0m"
_BOLD = "\033[1m"
_GREEN = "\033[32m"


def _idset(s):
    """Return frozenset of ints from an IDset string, or empty frozenset."""
    try:
        return frozenset(IDset(s)) if s else frozenset()
    except Exception:
        return frozenset()


def _fmt_idset(ids):
    """Format a set of ints as a compact IDset string."""
    return str(IDset(ids)) if ids else ""


def _print_node(node, label, alloc_cores, alloc_gpus, prefix, is_last):
    """Recursively print one topology node.

    node       -- topology dict; leaf if it has "cores", interior otherwise
    label      -- display name for this node (e.g. "socket 0", "numa 2")
    alloc_cores -- frozenset of allocated core IDs on this rank
    alloc_gpus  -- frozenset of allocated GPU IDs on this rank
    prefix     -- tree-connector string inherited from parent lines
    is_last    -- True when this is the last sibling (selects └─ vs ├─)
    """
    connector = "└─ " if is_last else "├─ "
    child_prefix = prefix + ("   " if is_last else "│  ")

    if "cores" in node:
        # Leaf: print with allocation annotation and optional colour.
        node_cores = _idset(node.get("cores", ""))
        node_gpus = _idset(node.get("gpus", ""))
        hit_c = node_cores & alloc_cores
        hit_g = node_gpus & alloc_gpus
        allocated = bool(hit_c or hit_g)

        text = f"{label}: cores {node['cores']}"
        if "gpus" in node:
            text += f"  gpu {node['gpus']}"
        if allocated:
            parts = []
            if hit_c:
                parts.append(f"core {_fmt_idset(hit_c)}")
            if hit_g:
                parts.append(f"gpu {_fmt_idset(hit_g)}")
            text += f"  ← {', '.join(parts)}"
            print(f"{_BOLD}{_GREEN}{prefix}{connector}{text}{_RESET}")
        else:
            print(f"{prefix}{connector}{text}")
    else:
        # Interior: print label, then recurse into the child list.
        print(f"{prefix}{connector}{label}")
        for key, children in node.items():
            if not isinstance(children, list):
                continue
            n = len(children)
            for i, child in enumerate(children):
                child_label = f"{key} {i}" if n > 1 else key
                _print_node(
                    child,
                    child_label,
                    alloc_cores,
                    alloc_gpus,
                    child_prefix,
                    i == n - 1,
                )


def _print_flat(sorted_ranks, hostname_by_rank, alloc_by_rank):
    """Print a flat (non-NUMA) summary when topology data is unavailable."""
    for rank in sorted_ranks:
        hostname = hostname_by_rank.get(rank, f"rank{rank}")
        alloc = alloc_by_rank.get(rank, {})
        cores = alloc.get("cores", frozenset())
        gpus = alloc.get("gpus", frozenset())
        print(f"rank {rank} ({hostname})")
        if cores:
            print(f"  cores {_fmt_idset(cores)}")
        if gpus:
            print(f"  gpus  {_fmt_idset(gpus)}")
        print()


def affinity_tree(jobid):
    """Print the AffinityPool topology tree for *jobid*."""
    try:
        out = subprocess.check_output(
            ["flux", "job", "info", jobid, "R"],
            universal_newlines=True,
            stderr=subprocess.PIPE,
        )
    except subprocess.CalledProcessError as exc:
        msg = exc.stderr.strip() if exc.stderr else "unknown error"
        print(f"error: flux job info {jobid} R: {msg}", file=sys.stderr)
        sys.exit(1)

    R = json.loads(out)
    scheduling = R.get("scheduling", {})
    topology = scheduling.get("topology")
    use_flat = not topology or scheduling.get("writer") != "AffinityPool"

    R_lite = R.get("execution", {}).get("R_lite", [])
    nodelist = R.get("execution", {}).get("nodelist", [])

    # Map rank → hostname: R_lite entries are in sorted-rank order,
    # which matches the nodelist index.
    sorted_ranks = sorted(r for entry in R_lite for r in IDset(entry["rank"]))
    hostname_by_rank = {
        rank: nodelist[i] for i, rank in enumerate(sorted_ranks) if i < len(nodelist)
    }

    # Map rank → (alloc_cores, alloc_gpus) from R_lite children.
    alloc_by_rank = {}
    for entry in R_lite:
        ch = entry.get("children", {})
        cores = _idset(ch.get("core", ""))
        gpus = _idset(ch.get("gpu", ""))
        for rank in IDset(entry["rank"]):
            alloc_by_rank[rank] = {"cores": cores, "gpus": gpus}

    if use_flat:
        _print_flat(sorted_ranks, hostname_by_rank, alloc_by_rank)
        return

    # Print one tree per allocated rank, in rank order.
    for rank_str, rank_data in sorted(topology.items(), key=lambda x: int(x[0])):
        rank = int(rank_str)
        hostname = hostname_by_rank.get(rank, f"rank{rank}")
        alloc = alloc_by_rank.get(rank, {})
        alloc_cores = alloc.get("cores", frozenset())
        alloc_gpus = alloc.get("gpus", frozenset())

        parts = []
        if alloc_cores:
            n = len(alloc_cores)
            parts.append(f"{n} core{'s' if n != 1 else ''}")
        if alloc_gpus:
            n = len(alloc_gpus)
            parts.append(f"{n} GPU{'s' if n != 1 else ''}")
        summary = f"  [{', '.join(parts)} allocated]" if parts else ""

        print(f"rank {rank} ({hostname}){summary}")
        for key, children in rank_data.items():
            if not isinstance(children, list):
                continue
            n = len(children)
            for i, child in enumerate(children):
                child_label = f"{key} {i}" if n > 1 else key
                _print_node(child, child_label, alloc_cores, alloc_gpus, "", i == n - 1)
        print()


def main():
    args = sys.argv[1:]
    if not args or "-h" in args or "--help" in args:
        print(__doc__)
        sys.exit(0 if args else 1)
    for jobid in args:
        affinity_tree(jobid)


if __name__ == "__main__":
    main()
