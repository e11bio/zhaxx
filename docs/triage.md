# Triage runbook

The order of operations matters as much as the surgery. The original incident
turned a one-dataset problem into a two-node HA cascade by unmasking services
on a pool that still carried stale ZFS state.

## Symptom signatures

| Symptom | Likely cause | Plan ops |
|---|---|---|
| `txg_sync` hung (D state) on import; `zpool status -v` hangs; import never completes | Persistent error log references a dead/unreadable objset; `spa_errlog_sync` deadlocks | `zap_set errlog_last=0`, free the errlog head + per-objset logs |
| Import succeeds but hangs the moment a clone is enumerated | Dangling clone reference in a `next_clones` / `dd_clones` ZAP | `zap_remove` the clone key(s); `bonus_set ds_num_children` |
| `VERIFY3`/assertion in `dsl_scan_sync` → `dsl_dataset_hold_obj` ENOENT | Stale in-progress scan cursor referencing freed objects | `zap_zero scan` |
| Dataset object returns EIO under `zdb -dddd` | Objset corruption (the orphan signature) | full orphan removal (namespace + clone refs + subtree free) |

## Guardrails

1. **Mask services before touching the pool.** On an HA appliance, unmasking
   storage services while the pool has stale state (pending scans, errlog
   entries, dirty txgs) invites automatic failover. Keep the storage stack and
   its import units masked until a clean guarded import succeeds.
2. **The pool must be exported for surgery.** The surgeon imports read-only for
   dump mode and read-write for `--commit`; both fail or misbehave against a
   pool already active on this or a partner node.
3. **Clear the in-memory error log too.** Zeroing on-disk `errlog_last` is not
   enough: a stale in-memory `head_errlog` entry makes `spa_errlog_sync` reopen
   the dead objset and deadlock again. The surgeon clears
   `spa_errlog_last`/`spa_errlog_scrub` in memory right after `spa_open`.
4. **Suspend scan progress in userspace.** `zfs_scan_suspend_progress=1` set via
   modprobe affects only the kernel module, not libzpool. The surgeon sets the
   libzpool global itself before import so a stale cursor cannot resume and
   panic mid-repair.
5. **Never free an object without a type match.** MOS object IDs are recycled
   aggressively; an ID that was an error log last week may be a space map now.
   The scanner never emits a `free` for a wrong-typed object, and the surgeon
   refuses one at apply time.
6. **Capture before you cut.** Run the scanner with `--capture DIR`; that dump
   set is the before-image and the input for offline re-analysis. If a commit
   goes wrong, `zpool import -F` rolls back to the previous txg.

## After a commit

1. Guarded import: `zpool import -N -o readonly=on POOL`.
2. `zpool status -v POOL` — confirm no dangling references, no hung sync.
3. Re-run `zhaxx-scan` against the pool; expect zero findings.
4. Import read-write and **scrub** before returning to service.
5. Only then unmask services and, on HA, re-establish the partner.

## Root-cause note

The incident's trigger was `autotrim=on` reaching special-vdev SSDs: TRIM/UNMAP
erased a metadata block identically on all mirror members during unclean
shutdowns, so the mirror could not self-heal. If you are repairing this class of
damage, also find and disable the mechanism that produced it — repair without
that is a countdown to recurrence.
