##############################################################
# Copyright 2026 Lawrence Livermore National Security, LLC
# (c.f. AUTHORS, NOTICE.LLNS, COPYING)
#
# This file is part of the Flux resource manager framework.
# For details, see https://github.com/flux-framework.
#
# SPDX-License-Identifier: LGPL-3.0
##############################################################

import argparse
import curses
import json
import locale
import logging
import shutil
import sys
import time
from datetime import datetime

import flux
import flux.constants
from flux.hostlist import Hostlist
from flux.idset import IDset
from flux.job import JobID, job_info_lookup

LOGGER = logging.getLogger("flux-sd")

UNIT_PATH_BASE = "/org/freedesktop/systemd1/unit/"
IFACE_UNIT = "org.freedesktop.systemd1.Unit"
IFACE_SERVICE = "org.freedesktop.systemd1.Service"
UINT64_MAX = (1 << 64) - 1

# Color pair indices
COLOR_HEADER = 1
# Job identity colors: 7 foreground colors × normal+bold = 14 distinct appearances
_JOB_FG_COLORS = (
    curses.COLOR_GREEN,
    curses.COLOR_CYAN,
    curses.COLOR_MAGENTA,
    curses.COLOR_BLUE,
    curses.COLOR_WHITE,
    curses.COLOR_YELLOW,
    curses.COLOR_RED,
)
_JOB_PAIRS = tuple(range(2, 2 + len(_JOB_FG_COLORS)))

# Sort columns: key → (row field, reverse-by-default)
SORT_COLS = {
    "n": ("name", False),
    "p": ("pid", True),
    "m": ("mem", True),
    "t": ("tasks", True),
    "c": ("cpu", True),
}
WATCH_HELP = "◄/►=select column  r=reverse  ▲/▼=select row  Enter=properties  q=quit"
# Map column key → sort key for arrow-key navigation
COL_TO_SORT = {"name": "n", "pid": "p", "mem": "m", "tasks": "t", "cpu": "c"}


def fmt_bytes(n):
    for suffix in ("", "K", "M", "G", "T"):
        if n < 1024:
            return f"{n}{suffix}"
        n //= 1024
    return f"{n}P"


def fmt_mem(current, maxval):
    if 0 < maxval < UINT64_MAX:
        return f"{fmt_bytes(current)}/{fmt_bytes(maxval)}"
    return fmt_bytes(current)


def fmt_cpuset(byte_array):
    """Convert CPU affinity byte array (little-endian bitmask) to idset string."""
    if not byte_array:
        return ""
    cpus = [
        i * 8 + bit
        for i, byte in enumerate(byte_array)
        for bit in range(8)
        if byte & (1 << bit)
    ]
    if not cpus:
        return ""
    return str(IDset(",".join(str(c) for c in cpus)))


def fmt_cpuset_nodes(cpus, nodes):
    """Format AllowedCPUs with optional AllowedMemoryNodes as cpuset[/nodeset]."""
    cpu_str = fmt_cpuset(cpus)
    if not cpu_str:
        return ""
    node_str = fmt_cpuset(nodes)
    return f"{cpu_str}/{node_str}" if node_str else cpu_str


def fmt_device(policy, allow_count):
    """Format DevicePolicy[/N] where N is the number of DeviceAllow entries."""
    if policy == "auto" and allow_count == 0:
        return ""
    if allow_count > 0:
        return f"{policy}/{allow_count}"
    return policy


def fit_cols(cols, width):
    """Return the leading subset of cols that fits within width characters."""
    total = 0
    result = []
    for col in cols:
        sep = 2 if result else 0
        if total + sep + col[2] > width:
            break
        total += sep + col[2]
        result.append(col)
    return result or cols[:1]


def fmt_cpu(nsec):
    sec = nsec / 1e9
    if sec < 60:
        return f"{sec:.1f}s"
    if sec < 3600:
        return f"{sec / 60:.1f}m"
    return f"{sec / 3600:.1f}h"


def unit_path(name):
    return UNIT_PATH_BASE + name


def sdbus_call(handle, payload, rank=flux.constants.FLUX_NODEID_ANY):
    return handle.rpc("sdbus.call", payload, nodeid=rank)


def get_job_ranks(handle, jobid):
    """Return sorted list of broker ranks allocated to jobid."""
    data = job_info_lookup(handle, JobID(jobid), keys=["R"]).get()
    R = json.loads(data["R"])
    ranks = IDset()
    for entry in R.get("execution", {}).get("R_lite", []):
        ranks |= IDset(entry["rank"])
    return sorted(ranks)


def fetch_data(handle, ranks, multi, hostlist, pattern):
    """Fetch unit list and extended properties; return list of row dicts."""
    payload = {"member": "ListUnitsByPatterns", "params": [[], [pattern]]}
    rpcs = [(rank, sdbus_call(handle, payload, rank)) for rank in ranks]

    raw = []
    for rank, rpc in rpcs:
        host = hostlist[rank] if multi else None
        try:
            resp = rpc.get()
        except OSError as exc:
            print(f"{host}: {exc}", file=sys.stderr)
            continue
        for unit in resp.get("params", [[]])[0]:
            name, _desc, _load, active, sub, _follow, path = unit[:7]
            raw.append(
                {
                    "host": host,
                    "rank": rank,
                    "path": path,
                    "name": name.removesuffix(".service"),
                    "active": active,
                    "sub": sub,
                }
            )

    if not raw:
        return []

    # Fire all extended-property RPCs in parallel
    ext_rpcs = [
        (
            r["rank"],
            r["path"],
            sdbus_call(
                handle,
                {
                    "path": r["path"],
                    "interface": "org.freedesktop.DBus.Properties",
                    "member": "GetAll",
                    "params": [IFACE_SERVICE],
                },
                r["rank"],
            ),
        )
        for r in raw
    ]
    ext = {}
    for rank, path, rpc in ext_rpcs:
        try:
            ext[(rank, path)] = rpc.get().get("params", [{}])[0]
        except OSError:
            pass

    def get_prop(rank, path, key):
        v = ext.get((rank, path), {}).get(key)
        return v[1] if isinstance(v, list) and len(v) == 2 else None

    rows = []
    for r in raw:
        rank, path = r["rank"], r["path"]
        rows.append(
            {
                **r,
                "pid": get_prop(rank, path, "MainPID") or 0,
                "mem": get_prop(rank, path, "MemoryCurrent") or 0,
                "mem_max": get_prop(rank, path, "MemoryMax") or UINT64_MAX,
                "tasks": get_prop(rank, path, "TasksCurrent") or 0,
                "cpu": get_prop(rank, path, "CPUUsageNSec") or 0,
                "cpus": get_prop(rank, path, "AllowedCPUs") or [],
                "nodes": get_prop(rank, path, "AllowedMemoryNodes") or [],
                "device": fmt_device(
                    get_prop(rank, path, "DevicePolicy") or "auto",
                    len(get_prop(rank, path, "DeviceAllow") or []),
                ),
            }
        )
    return rows


def build_cols(rows, multi):
    """Return list of (key, header, width, fmt_fn) for columns with valid data."""
    cols = []
    if multi:
        hostw = max(max(len(r["host"]) for r in rows), len("HOST"))
        cols.append(("host", "HOST", hostw, str))

    unitw = max(max(len(r["name"]) for r in rows), len("UNIT"))
    cols.append(("name", "UNIT", unitw, str))

    if any(r["pid"] > 0 for r in rows):
        cols.append(("pid", "MainPID", 7, lambda v: str(v) if v > 0 else ""))

    if any(0 < r["mem"] < UINT64_MAX for r in rows):
        mem_vals = [fmt_mem(r["mem"], r["mem_max"]) for r in rows]
        memw = max(max(len(v) for v in mem_vals), len("Memory"))
        cols.append(
            (
                "mem",
                "Memory",
                memw,
                lambda v, mx=None: fmt_mem(v, mx) if 0 < v < UINT64_MAX else "",
            )
        )

    if any(0 < r["tasks"] < UINT64_MAX for r in rows):
        cols.append(
            ("tasks", "Tasks", 5, lambda v: str(v) if 0 < v < UINT64_MAX else "")
        )

    if any(0 < r["cpu"] < UINT64_MAX for r in rows):
        cols.append(
            ("cpu", "CPUUsage", 8, lambda v: fmt_cpu(v) if 0 < v < UINT64_MAX else "")
        )

    cpu_vals = [fmt_cpuset_nodes(r["cpus"], r["nodes"]) for r in rows]
    cpusw = max(max((len(v) for v in cpu_vals), default=0), len("AllowedCPUs"))
    cols.append(("cpus", "AllowedCPUs", cpusw, fmt_cpuset_nodes))

    dev_vals = [r["device"] for r in rows]
    devw = max(max((len(v) for v in dev_vals), default=0), len("Device"))
    cols.append(("device", "Device", devw, str))

    return cols


def fmt_row(row, cols):
    """Format a row as a plain string using the given columns."""
    parts = []
    for key, _hdr, width, fmt in cols:
        if key == "mem":
            val = fmt(row[key], row.get("mem_max", UINT64_MAX))
        elif key == "cpus":
            val = fmt(row["cpus"], row.get("nodes", []))
        else:
            val = fmt(row[key])
        if key in ("host", "name"):
            parts.append(f"{val:<{width}}")
        else:
            parts.append(f"{val:>{width}}")
    return "  ".join(parts)


def fmt_header(cols, sort_key, sort_rev):
    """Format the header line with a sort indicator on the active column.

    The indicator (▲/▼) is appended flush after the sorted column's content,
    consuming one of the two separator spaces so data rows stay aligned.
    """
    sort_col = SORT_COLS.get(sort_key, (None, False))
    sort_field = sort_col[0]
    effective_rev = sort_rev if sort_rev is not None else sort_col[1]
    parts = []
    for key, hdr, width, _fmt in cols:
        if key == sort_field:
            indicator = "▼" if effective_rev else "▲"
            if key in ("host", "name"):
                # left-aligned: indicator flush after text, remainder padded
                parts.append(hdr + indicator + " " * (width - len(hdr)))
            else:
                parts.append(f"{hdr:>{width}}" + indicator)
        else:
            if key in ("host", "name"):
                parts.append(f"{hdr:<{width}}")
            else:
                parts.append(f"{hdr:>{width}}")

    # Join: one space after sorted col (indicator consumed the other), two elsewhere
    result = parts[0]
    for i in range(1, len(parts)):
        sep = " " if cols[i - 1][0] == sort_field else "  "
        result += sep + parts[i]
    return result


def shell_jobid(name):
    """For names matching *-shell-RANK-JOBID, return JOBID; else return name."""
    parts = name.rsplit("-", 2)
    if len(parts) == 3 and parts[0].endswith("-shell") and parts[1].isdigit():
        return parts[2]
    return name


def sort_rows(rows, sort_key, sort_rev):
    field, default_rev = SORT_COLS.get(sort_key, ("name", False))
    reverse = sort_rev if sort_rev is not None else default_rev
    if field == "name":
        return sorted(rows, key=lambda r: shell_jobid(r["name"]), reverse=reverse)
    return sorted(rows, key=lambda r: r[field], reverse=reverse)


def plain_output(rows, cols, sort_key="n", sort_rev=False):
    """Print rows as plain text (non-watch mode)."""
    rows = sort_rows(rows, sort_key, sort_rev)
    if sys.stdout.isatty():
        cols = fit_cols(cols, shutil.get_terminal_size().columns)
    print(fmt_header(cols, sort_key, sort_rev))
    for row in rows:
        print(fmt_row(row, cols))


def init_colors():
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(COLOR_HEADER, curses.COLOR_BLACK, curses.COLOR_CYAN)
    for i, fg in enumerate(_JOB_FG_COLORS):
        curses.init_pair(_JOB_PAIRS[i], fg, -1)


def job_id(name):
    """Extract job identity token (last dash-delimited field) from unit name."""
    return name.rsplit("-", 1)[-1]


def row_color(row, job_colors=None):
    if job_colors is not None:
        attr = job_colors.get(job_id(row["name"]))
        if attr is not None:
            return attr
    return curses.A_NORMAL


def safe_addstr(stdscr, y, x, s, attr=curses.A_NORMAL):
    try:
        stdscr.addstr(y, x, s, attr)
    except curses.error:
        pass


def draw_screen(stdscr, rows, cols, sort_key, sort_rev, has_color, selected=0):
    height, width = stdscr.getmaxyx()
    cols = fit_cols(cols, width)
    stdscr.erase()

    # Title bar
    ts = datetime.now().strftime("%H:%M:%S")
    title = f" flux-sd list-units  {ts} "
    safe_addstr(
        stdscr,
        0,
        0,
        title[:width].ljust(width),
        curses.color_pair(COLOR_HEADER) if has_color else curses.A_REVERSE,
    )

    # Column header
    hdr = fmt_header(cols, sort_key, sort_rev)
    safe_addstr(
        stdscr,
        1,
        0,
        hdr[:width].ljust(width),
        (
            (curses.color_pair(COLOR_HEADER) | curses.A_BOLD)
            if has_color
            else curses.A_BOLD
        ),
    )

    # Build job-color map: 7 colors × normal+bold gives 14 distinct appearances
    job_ids = []
    seen = set()
    for row in rows:
        jid = job_id(row["name"])
        if jid not in seen:
            seen.add(jid)
            job_ids.append(jid)
    nbase = len(_JOB_FG_COLORS)
    job_colors = {
        jid: curses.color_pair(_JOB_PAIRS[i % nbase])
        | (curses.A_BOLD if i >= nbase else 0)
        for i, jid in enumerate(job_ids)
    }

    # Data rows
    sorted_rows = sort_rows(rows, sort_key, sort_rev)
    for i, row in enumerate(sorted_rows[: height - 3], start=2):
        line = fmt_row(row, cols)
        if i - 2 == selected:
            attr = curses.A_REVERSE
        else:
            attr = row_color(row, job_colors) if has_color else curses.A_NORMAL
        safe_addstr(stdscr, i, 0, line[: width - 1], attr)

    # Status bar
    help_text = WATCH_HELP
    safe_addstr(
        stdscr,
        height - 1,
        0,
        help_text[:width].ljust(width),
        curses.color_pair(COLOR_HEADER) if has_color else curses.A_REVERSE,
    )

    stdscr.refresh()


def is_useful(value):
    """Return True if a D-Bus property value is worth displaying."""
    if value is None or value == "" or value == []:
        return False
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        return 0 < value < UINT64_MAX
    return True


def draw_properties(stdscr, title, prop_lines, scroll, has_color):
    height, width = stdscr.getmaxyx()
    stdscr.erase()
    safe_addstr(
        stdscr,
        0,
        0,
        f" {title} "[:width].ljust(width),
        curses.color_pair(COLOR_HEADER) if has_color else curses.A_REVERSE,
    )
    visible = prop_lines[scroll : scroll + height - 2]
    for i, line in enumerate(visible, start=1):
        safe_addstr(stdscr, i, 0, line[: width - 1])
    help_text = "Space/b=page  ▲/▼/j/k=line  g/G=top/bot  f=filter  q/ESC=back"
    safe_addstr(
        stdscr,
        height - 1,
        0,
        help_text[:width].ljust(width),
        curses.color_pair(COLOR_HEADER) if has_color else curses.A_REVERSE,
    )
    stdscr.refresh()


def properties_screen(stdscr, handle, row, has_color):
    """Show all (or filtered) properties for a unit. Returns on q/ESC."""
    stdscr.nodelay(False)

    def fetch_props():
        try:
            resp = sdbus_call(
                handle,
                {
                    "path": row["path"],
                    "interface": "org.freedesktop.DBus.Properties",
                    "member": "GetAll",
                    "params": [IFACE_SERVICE],
                },
                row["rank"],
            ).get()
            return resp.get("params", [{}])[0]
        except OSError:
            return {}

    props = fetch_props()
    host = row.get("host") or ""
    unit = row["name"]
    title = f"Properties: {unit}" + (f" on {host}" if host else "")
    filter_useful = True

    def build_lines(filt):
        lines = []
        for key in sorted(props):
            v = props[key]
            value = v[1] if isinstance(v, list) and len(v) == 2 else v
            if filt and not is_useful(value):
                continue
            lines.append(f"  {key} = {value}")
        return lines or ["  (no properties to display)"]

    prop_lines = build_lines(filter_useful)
    scroll = 0

    while True:
        height = stdscr.getmaxyx()[0]
        draw_properties(stdscr, title, prop_lines, scroll, has_color)
        ch = stdscr.getch()
        page = max(1, height - 3)
        maxscroll = max(0, len(prop_lines) - (height - 2))
        if ch in (ord("q"), ord("Q"), 27):  # 27 = ESC
            break
        elif ch in (curses.KEY_UP, ord("k")):
            scroll = max(0, scroll - 1)
        elif ch in (curses.KEY_DOWN, ord("j")):
            scroll = min(maxscroll, scroll + 1)
        elif ch in (ord(" "), curses.KEY_NPAGE):
            scroll = min(maxscroll, scroll + page)
        elif ch in (ord("b"), curses.KEY_PPAGE):
            scroll = max(0, scroll - page)
        elif ch in (ord("g"), curses.KEY_HOME):
            scroll = 0
        elif ch in (ord("G"), curses.KEY_END):
            scroll = maxscroll
        elif ch in (ord("f"), ord("F")):
            filter_useful = not filter_useful
            prop_lines = build_lines(filter_useful)
            scroll = 0

    stdscr.nodelay(True)


def watch_loop(stdscr, handle, ranks, multi, hostlist, pattern, interval):
    curses.curs_set(0)
    stdscr.nodelay(True)
    has_color = curses.has_colors()
    if has_color:
        init_colors()

    sort_key = "n"
    sort_rev = None  # None = use column default
    selected = 0

    rows = fetch_data(handle, ranks, multi, hostlist, pattern)
    cols = build_cols(rows, multi) if rows else []

    while True:
        sorted_rows = sort_rows(rows, sort_key, sort_rev) if rows else []
        selected = min(selected, max(0, len(sorted_rows) - 1))
        if rows and cols:
            draw_screen(stdscr, rows, cols, sort_key, sort_rev, has_color, selected)

        deadline = time.monotonic() + interval
        while time.monotonic() < deadline:
            ch = stdscr.getch()
            redraw = ch != curses.ERR
            if ch in (ord("q"), ord("Q")):
                return
            elif ch == curses.KEY_UP:
                selected = max(0, selected - 1)
            elif ch == curses.KEY_DOWN:
                selected = min(len(sorted_rows) - 1, selected + 1)
            elif ch in (ord("\n"), ord("\r"), curses.KEY_ENTER):
                if sorted_rows:
                    properties_screen(stdscr, handle, sorted_rows[selected], has_color)
                    redraw = True
            elif ch in (ord("r"), ord("R")):
                sort_rev = not (
                    sort_rev
                    if sort_rev is not None
                    else SORT_COLS.get(sort_key, ("", False))[1]
                )
            elif ch in (curses.KEY_LEFT, curses.KEY_RIGHT):
                sortable = [COL_TO_SORT[c[0]] for c in cols if c[0] in COL_TO_SORT]
                if sortable:
                    idx = sortable.index(sort_key) if sort_key in sortable else 0
                    delta = 1 if ch == curses.KEY_RIGHT else -1
                    sort_key = sortable[(idx + delta) % len(sortable)]
                    sort_rev = None
            if redraw and rows and cols:
                sorted_rows = sort_rows(rows, sort_key, sort_rev)
                draw_screen(stdscr, rows, cols, sort_key, sort_rev, has_color, selected)
            time.sleep(0.05)

        rows = fetch_data(handle, ranks, multi, hostlist, pattern)
        if rows:
            cols = build_cols(rows, multi)


def list_units(args):
    handle = flux.Flux()

    if args.jobid is not None and args.rank is not None:
        LOGGER.error("--jobid and --rank are mutually exclusive")
        sys.exit(1)

    if args.jobid is not None:
        ranks = get_job_ranks(handle, args.jobid)
    elif args.rank is not None:
        try:
            ranks = list(IDset(args.rank))
        except ValueError as exc:
            LOGGER.error("invalid rank idset: %s", exc)
            sys.exit(1)
    else:
        ranks = [flux.constants.FLUX_NODEID_ANY]

    multi = len(ranks) > 1
    hostlist = Hostlist(handle.attr_get("hostlist")) if multi else None

    rows = fetch_data(handle, ranks, multi, hostlist, args.pattern)
    if not rows:
        return
    cols = build_cols(rows, multi)

    if args.watch is not None:
        if not sys.stdout.isatty():
            LOGGER.error("-w requires a terminal")
            sys.exit(1)
        locale.setlocale(locale.LC_ALL, "")
        curses.wrapper(
            watch_loop, handle, ranks, multi, hostlist, args.pattern, args.watch
        )
    else:
        plain_output(rows, cols)


def show_properties(args):
    handle = flux.Flux()
    rank = args.rank if args.rank is not None else flux.constants.FLUX_NODEID_ANY
    resp = sdbus_call(
        handle,
        {
            "path": unit_path(args.unit),
            "interface": "org.freedesktop.DBus.Properties",
            "member": "GetAll",
            "params": [args.interface],
        },
        rank,
    ).get()
    props = resp.get("params", [{}])[0]
    for key in sorted(props):
        variant = props[key]
        value = (
            variant[1] if isinstance(variant, list) and len(variant) == 2 else variant
        )
        print(f"{key}={value}")


def show_property(args):
    handle = flux.Flux()
    rank = args.rank if args.rank is not None else flux.constants.FLUX_NODEID_ANY
    resp = sdbus_call(
        handle,
        {
            "path": unit_path(args.unit),
            "interface": "org.freedesktop.DBus.Properties",
            "member": "Get",
            "params": [args.interface, args.property],
        },
        rank,
    ).get()
    variant = resp.get("params", [None])[0]
    value = variant[1] if isinstance(variant, list) and len(variant) == 2 else variant
    print(value)


@flux.util.CLIMain(LOGGER)
def main():
    parser = argparse.ArgumentParser(
        prog="flux-sd",
        formatter_class=flux.util.help_formatter(),
    )
    subparsers = parser.add_subparsers(
        title="subcommands", description="", dest="subcommand"
    )
    subparsers.required = True

    # list-units subcommand
    list_parser = subparsers.add_parser(
        "list-units",
        formatter_class=flux.util.help_formatter(),
        help="list systemd --user units",
    )
    list_parser.add_argument(
        "-r",
        "--rank",
        metavar="IDSET",
        help="query broker ranks in IDSET (default: local)",
    )
    list_parser.add_argument(
        "-j",
        "--jobid",
        metavar="JOBID",
        help="query all ranks allocated to JOBID (mutually exclusive with --rank)",
    )
    list_parser.add_argument(
        "-w",
        "--watch",
        nargs="?",
        const=2.0,
        type=float,
        metavar="SECS",
        help="monitor mode: refresh every SECS seconds (default: 2)",
    )
    list_parser.add_argument(
        "pattern",
        nargs="?",
        default="*-shell-*.service",
        help="unit name glob pattern (default: *-shell-*.service)",
    )
    list_parser.set_defaults(func=list_units)

    # properties subcommand
    props_parser = subparsers.add_parser(
        "properties",
        formatter_class=flux.util.help_formatter(),
        help="show all properties of a unit",
    )
    props_parser.add_argument(
        "-r",
        "--rank",
        type=int,
        metavar="N",
        help="query broker rank N (default: local)",
    )
    props_parser.add_argument(
        "-i",
        "--interface",
        default=IFACE_SERVICE,
        metavar="IFACE",
        help=f"D-Bus interface (default: {IFACE_SERVICE})",
    )
    props_parser.add_argument("unit", help="unit name (e.g. flux-123.service)")
    props_parser.set_defaults(func=show_properties)

    # property subcommand
    prop_parser = subparsers.add_parser(
        "property",
        formatter_class=flux.util.help_formatter(),
        help="show a single property of a unit",
    )
    prop_parser.add_argument(
        "-r",
        "--rank",
        type=int,
        metavar="N",
        help="query broker rank N (default: local)",
    )
    prop_parser.add_argument(
        "-i",
        "--interface",
        default=IFACE_SERVICE,
        metavar="IFACE",
        help=f"D-Bus interface (default: {IFACE_SERVICE})",
    )
    prop_parser.add_argument("unit", help="unit name (e.g. flux-123.service)")
    prop_parser.add_argument("property", help="property name (e.g. MainPID)")
    prop_parser.set_defaults(func=show_property)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

# vi: ts=4 sw=4 expandtab
