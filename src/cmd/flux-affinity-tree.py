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

Example output for a 1-core + 1-GPU slot on rank 0 of a 2-socket node
(allocated IDs are shown in green in a colour-capable terminal)::

    node0 (0)  [1 core, 1 GPU allocated]
    ├─ socket 0
    │  ├─ numa 0: cores 0,1-14  gpu 0          ← core 0 and gpu 0 in green
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

import flux
from flux.hostlist import Hostlist
from flux.idset import IDset

_RESET = "\033[0m"
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


def _fmt_colored_idset(node_str, alloc_ids):
    """Render *node_str* idset with allocated IDs in green, rest in default.

    Runs are interleaved in numerical order so the output reads naturally
    (e.g. "0-2,[green]3[/green],4-14").
    """
    all_ids = _idset(node_str)
    alloc_here = all_ids & alloc_ids
    if not alloc_here:
        return node_str
    if not (all_ids - alloc_here):
        return f"{_GREEN}{node_str}{_RESET}"
    # Mixed: walk sorted values, group into same-colour runs, then join.
    segments = []
    cur_alloc = None
    cur_run = []
    for v in sorted(all_ids):
        is_alloc = v in alloc_here
        if cur_alloc is None or is_alloc != cur_alloc:
            if cur_run:
                segments.append((cur_alloc, cur_run))
            cur_alloc, cur_run = is_alloc, [v]
        else:
            cur_run.append(v)
    if cur_run:
        segments.append((cur_alloc, cur_run))
    parts = []
    for is_alloc, run in segments:
        s = _fmt_idset(set(run))
        parts.append(f"{_GREEN}{s}{_RESET}" if is_alloc else s)
    return ",".join(parts)


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
        # Leaf: colour allocated IDs green inline, unallocated in default.
        cores_str = _fmt_colored_idset(node["cores"], alloc_cores)
        text = f"{label}: cores {cores_str}"
        if "gpus" in node:
            text += f"  gpu {_fmt_colored_idset(node['gpus'], alloc_gpus)}"
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


def _instance_resources(ranks):
    """Return {rank: {"cores": frozenset, "gpus": frozenset}} from instance R.

    Fetches the full instance R via a resource.status RPC so that unallocated
    cores/GPUs on each node are visible alongside the allocated ones.
    Returns an empty dict if the RPC fails for any reason.
    """
    try:
        h = flux.Flux()
        R_inst = h.rpc("resource.status", nodeid=0).get()["R"]
    except Exception:
        return {}
    result = {}
    for entry in R_inst.get("execution", {}).get("R_lite", []):
        ch = entry.get("children", {})
        cores = _idset(ch.get("core", ""))
        gpus = _idset(ch.get("gpu", ""))
        for rank in IDset(entry["rank"]):
            if rank in ranks:
                result[rank] = {"cores": cores, "gpus": gpus}
    return result


def _print_flat(sorted_ranks, hostname_by_rank, alloc_by_rank):
    """Print a flat (non-NUMA) summary when topology data is unavailable.

    Full per-node resources are fetched from the instance R via RPC so that
    unallocated cores/GPUs are shown in default colour alongside the allocated
    ones (in green), matching the behaviour of the tree output.
    """
    inst = _instance_resources(set(sorted_ranks))
    for rank in sorted_ranks:
        hostname = hostname_by_rank.get(rank, f"rank{rank}")
        alloc = alloc_by_rank.get(rank, {})
        alloc_cores = alloc.get("cores", frozenset())
        alloc_gpus = alloc.get("gpus", frozenset())
        full = inst.get(rank, {})
        all_cores = full.get("cores") or alloc_cores
        all_gpus = full.get("gpus") or alloc_gpus
        print(f"{hostname} ({rank})")
        if all_cores:
            print(f"  cores {_fmt_colored_idset(_fmt_idset(all_cores), alloc_cores)}")
        if all_gpus:
            print(f"  gpus  {_fmt_colored_idset(_fmt_idset(all_gpus), alloc_gpus)}")
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

    # Map rank → hostname.  nodelist is an array of hostlist strings whose
    # expansions, concatenated in order, give one hostname per rank.
    sorted_ranks = sorted(r for entry in R_lite for r in IDset(entry["rank"]))
    all_hosts = [h for hl in nodelist for h in Hostlist(hl)]
    hostname_by_rank = {
        rank: all_hosts[i] for i, rank in enumerate(sorted_ranks) if i < len(all_hosts)
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

        print(f"{hostname} ({rank}){summary}")
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
