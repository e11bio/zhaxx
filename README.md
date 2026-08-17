# zhaxx

Forensics and surgical repair for ZFS pools made unimportable by MOS-level
metadata corruption — dangling clone/namespace references, stale persistent
error logs, stale scan cursors, and orphaned DSL subtrees left by a dead
objset.

Distilled from a real incident: a clone's objset became unreadable during
unclean shutdowns; the persistent error log referencing it deadlocked
`txg_sync` in `spa_errlog_sync` on every import, and clone enumeration kept
the pool from importing even after the on-disk error log was cleared.

Two tools, one contract — the scanner produces a plan, the surgeon executes it:

- **`scan/zhaxx-scan`** (Python 3, stdlib only) — read-only. Walks the Meta
  Object Set via `zdb`, cross-checks the DSL namespace, and emits a report
  plus a repair *plan*. No build step; runs anywhere `zdb` exists, so it is
  usable in the first minutes of triage.
- **`surgeon/zhaxx`** (C, libzpool) — applies a plan to an *exported* pool as
  a single atomic `dsl_sync_task`. Every operation carries an expected
  pre-state; the surgeon re-verifies all preconditions before writing
  anything, and refuses to free an object whose type no longer matches
  (MOS IDs are recycled aggressively — freeing a recycled space map corrupts
  the pool; that mistake happened once and must not recur).

## Workflow

```sh
# 1. Triage (read-only). --capture saves every zdb dump as a pre-surgery
#    backup and an offline dump set.
scan/zhaxx-scan --pool POOL --exported --capture ./capture --plan repair.plan

# 2. Review. Read the report and repair.plan line by line against ./capture.

# 3. Build the surgeon against the ZFS that owns the pool (see below).
surgeon/build.sh

# 4. Dry run: import read-only, verify every precondition, write nothing.
sudo surgeon/zhaxx --plan repair.plan POOL

# 5. Apply (pool must be exported; services masked — see docs/triage.md).
sudo surgeon/zhaxx --commit --plan repair.plan POOL

# 6. Re-scan to confirm clean, then guarded import + scrub before service.
scan/zhaxx-scan --pool POOL --exported
```

## Building the surgeon

`surgeon/build.sh` links by soname (no dev symlinks in `/usr/lib`) and supports
two header layouts, autodetected or set via `ZFS_SRC=` / `LIBDIR=` / `CC=`:

- **Source tree** (`/usr/src/zfs-*`, a git checkout, or a release tarball) —
  the supported path. The vendored `surgeon/compat/` headers supply the libspl
  userspace shims, so the build needs only the ZFS header sources plus the
  runtime `libzpool` — no `./configure`, no full ZFS build. This is the
  appliance layout.
- **Distro `-dev` package** (`/usr/include/libzfs` + `/usr/include/libspl`) —
  a convenience. Note that some distributions (e.g. Ubuntu `libzfslinux-dev`)
  ship internal headers that reference `sys/zfs_ioctl.h` without shipping it;
  when that build fails, use a source tree instead (see `.github/workflows/`
  for the exact steps CI uses to fetch a matching one).

The surgeon must be built against the **same ZFS version that owns the pool**;
the on-disk format interpretation comes from that libzpool.

## Plan format

One operation per line (`#` comments and blanks ignored). Every mutating op
names its expected current value:

```
zap_set    obj=N key=STR expect=U value=U     # set a ZAP uint64 entry
zap_zero   obj=N key=STR count=N              # zero a fixed-width ZAP entry
zap_remove obj=N key=STR [expect=U]           # remove a ZAP entry
bonus_set  obj=N field=ds_num_children expect=U value=U
free       obj=N type="STR"                   # free; type must still match
```

## Tests

```sh
python3 tests/test_scan.py        # offline: no ZFS, no root
tests/live_integration.sh         # optional: file-backed pool, needs root+ZFS
```

The offline tests run the scanner against anonymized fixtures in
`tests/fixtures/` (invented pool, IDs, and names; only the corruption *shape*
is real). `tests/fixtures/example-orphan-clone/` and
`plans/example-orphan-clone.plan` are the worked example.

## Scope

Repairs referential damage in the MOS DSL layer. It does **not** recover a pool
whose MOS directory, labels, or uberblocks are gone — that needs
label/uberblock-level recovery. See `docs/triage.md` for the decision path and
the operational guardrails (mask services, export first, guarded re-import).

`surgeon/` is CDDL-1.0 (derived from OpenZFS). Everything else is MIT.
