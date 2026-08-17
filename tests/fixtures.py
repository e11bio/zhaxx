# SPDX-License-Identifier: MIT
"""
Anonymized MOS fixtures for zhaxx-scan tests.

Each scenario is a set of synthetic zdb `-dddd` object dumps written as
obj_<N>.txt files, consumed by `zhaxx-scan --dumps`. Object IDs, names, and
the pool are invented; only the *shape* of the corruption is real. This is
also the repo's illustrative case: a clone whose objset died, leaving
dangling clone/namespace references, a stale persistent error log, and a
stale scan cursor — the pattern that makes a pool unimportable.
"""

import os

# zdb prints the per-object data row with these columns:
#   Object  lvl  iblk  dblk  dsize  dnsize  lsize  %full  type
_HEADER_COLS = "    {obj}    1   128K    512    12K     512    512  {full}  {type}"
_MOS_BANNER = "Dataset mos [META], ID 0, cr_txg 4, 1.00G, 1000 objects\n"


def _fmt_val(v):
    if isinstance(v, (list, tuple)):
        return " ".join(str(x) for x in v)
    return str(v)


class Obj:
    def __init__(self, obj, type_str, kv=None, unreadable=False, full="100.00"):
        self.obj = obj
        self.type = type_str
        self.kv = kv or {}          # ordered dict of field/zap name -> value
        self.unreadable = unreadable
        self.full = full

    def render(self):
        lines = [_MOS_BANNER.rstrip("\n"), ""]
        lines.append("    Object  lvl   iblk   dblk  dsize  dnsize  "
                     "lsize   %full  type")
        lines.append(_HEADER_COLS.format(obj=self.obj, type=self.type,
                                         full=self.full))
        if self.unreadable:
            lines.append(f"zdb: can't open object {self.obj}: "
                         "Input/output error")
            return "\n".join(lines) + "\n"
        for k, v in self.kv.items():
            lines.append(f"\t\t{k} = {_fmt_val(v)} ")
        return "\n".join(lines) + "\n"


def write_scenario(objs, outdir):
    os.makedirs(outdir, exist_ok=True)
    for o in objs:
        with open(os.path.join(outdir, f"obj_{o.obj}.txt"), "w") as f:
            f.write(o.render())


# ---------------------------------------------------------------------------
# Scenario A: orphaned clone with dead objset (the full incident shape)
# ---------------------------------------------------------------------------
# A snapshot (obj 48) has two clones. One clone's head dataset objset (673)
# became unreadable. Its DSL directory (670) still sits in the namespace
# (child map 34) and its head is still listed in both the snapshot's
# next_clones ZAP (58) and the origin dir's dd_clones ZAP (59). A persistent
# error log (910 -> 912) references the dead objset, and a scan (resilver) is
# mid-flight with a queue object that no longer exists.
SCENARIO_A = [
    Obj(1, "object directory", {
        "root_dataset": 32,
        "errlog_scrub": 0,
        "errlog_last": 910,
        # dsl_scan_phys_t: func=resilver(2) state=SCANNING(1) queue=999(gone)
        "scan": [2, 1, 999] + [0] * 21,
    }),
    # root
    Obj(32, "DSL directory", {
        "head_dataset_obj": 55, "parent_dir_obj": 0, "origin_obj": 48,
        "child_dir_zapobj": 34, "clones": 0,
    }),
    Obj(55, "DSL dataset", {"prev_snap_obj": 0, "next_clones_obj": 0}),
    Obj(34, "DSL directory child map", {
        "$MOS": 35, "$FREE": 38, "$ORIGIN": 42,
        "data": 1792, "orphan-9f3c1d": 670,
    }),
    # $ORIGIN dir + its clone bookkeeping
    Obj(42, "DSL directory", {
        "head_dataset_obj": 45, "parent_dir_obj": 32, "origin_obj": 0,
        "child_dir_zapobj": 44, "clones": 59,
    }),
    Obj(45, "DSL dataset", {"prev_snap_obj": 0, "next_clones_obj": 0}),
    Obj(44, "DSL directory child map", {}),
    # the origin snapshot: 2 children counted (healthy + orphan), should be 1
    Obj(48, "DSL dataset", {
        "dir_obj": 42, "prev_snap_obj": 0, "next_snap_obj": 45,
        "num_children": 6, "next_clones_obj": 58,
    }),
    Obj(58, "DSL dataset next clones", {"36": 54, "2a1": 673}),
    Obj(59, "DSL dir clones", {"36": 54, "2a1": 673}),
    Obj(54, "DSL dataset", {"prev_snap_obj": 48, "next_clones_obj": 0}),
    # healthy data share
    Obj(1792, "DSL directory", {
        "head_dataset_obj": 1793, "parent_dir_obj": 32, "origin_obj": 0,
        "child_dir_zapobj": 1794, "clones": 0,
    }),
    Obj(1793, "DSL dataset", {"prev_snap_obj": 0, "next_clones_obj": 0}),
    Obj(1794, "DSL directory child map", {}),
    # the orphan clone subtree (directory readable, head objset DEAD)
    Obj(670, "DSL directory", {
        "head_dataset_obj": 673, "parent_dir_obj": 32, "origin_obj": 48,
        "child_dir_zapobj": 672, "props_zapobj": 671,
    }),
    Obj(671, "DSL props", {"refreservation": 0}),
    Obj(672, "DSL directory child map", {}),
    Obj(673, "DSL dataset", unreadable=True),
    # persistent error log referencing the dead objset (hex key 2a1 = 673)
    Obj(910, "persistent error log", {"2a1": 912}),
    Obj(912, "persistent error log", {"0:ffffffffffffffff:0": 260923}),
]

# Expected repair-plan op lines for Scenario A (order-independent set).
SCENARIO_A_PLAN = {
    "zap_set obj=1 key=errlog_last expect=910 value=0",
    'free obj=910 type="persistent error log"',
    'free obj=912 type="persistent error log"',
    "zap_zero obj=1 key=scan count=24",
    "zap_remove obj=34 key=orphan-9f3c1d expect=670",
    "zap_remove obj=58 key=2a1 expect=673",
    "zap_remove obj=59 key=2a1 expect=673",
    "bonus_set obj=48 field=ds_num_children expect=6 value=5",
    'free obj=670 type="DSL directory"',
    'free obj=671 type="DSL props"',
    'free obj=672 type="DSL directory child map"',
}

# ---------------------------------------------------------------------------
# Scenario B: errlog_last points at a RECYCLED object (now a space map)
# ---------------------------------------------------------------------------
# The hard lesson of the incident: MOS object IDs are recycled aggressively.
# A stale errlog_last can point at an ID that has since been reallocated as a
# space map. Detaching the reference is correct; freeing the object is not.
SCENARIO_B = [
    Obj(1, "object directory", {
        "root_dataset": 32, "errlog_scrub": 0, "errlog_last": 920,
    }),
    Obj(32, "DSL directory", {
        "head_dataset_obj": 55, "parent_dir_obj": 0, "origin_obj": 0,
        "child_dir_zapobj": 34, "clones": 0,
    }),
    Obj(55, "DSL dataset", {"prev_snap_obj": 0, "next_clones_obj": 0}),
    Obj(34, "DSL directory child map", {"$MOS": 35, "$FREE": 38}),
    Obj(920, "SPA space map", {}),  # recycled — must NOT be freed
]

SCENARIO_B_PLAN = {
    "zap_set obj=1 key=errlog_last expect=920 value=0",
}
