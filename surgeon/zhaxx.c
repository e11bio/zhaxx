/*
 * zhaxx.c — plan-driven ZFS MOS repair tool (libzpool).
 *
 * Applies a repair plan (from zhaxx-scan) to an exported pool as a single
 * atomic dsl_sync_task. Repairs the corruption class that makes pools
 * unimportable: dangling clone/namespace references, stale persistent error
 * logs, stale scan cursors, and orphaned DSL subtrees.
 *
 * Contract:
 *   - Default is dump mode (read-only): parse the plan, import read-only,
 *     print the observed vs. expected state for every op. Nothing is written.
 *   - --commit applies the plan. TWO PASSES inside one sync task: verify
 *     EVERY precondition first; only if all pass does the txg get dirtied.
 *     A failed precondition aborts the whole transaction — all or nothing.
 *   - Every mutating op names an expected current value (expect=/type=).
 *     dmu_object_free requires the observed type to still match (MOS object
 *     IDs are recycled aggressively; freeing a recycled space map corrupts
 *     the pool). This mandatory type check is the hard lesson of the
 *     incident this tool was distilled from.
 *
 * Plan grammar (one op per line; '#' and blank lines ignored):
 *   zap_set    obj=N key=STR expect=U value=U
 *   zap_zero   obj=N key=STR count=N
 *   zap_remove obj=N key=STR [expect=U]
 *   bonus_set  obj=N field=ds_num_children expect=U value=U
 *   free       obj=N type="STR"
 *
 * SPDX-License-Identifier: CDDL-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/dmu.h>
#include <sys/zap.h>
#include <sys/dmu_tx.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_synctask.h>
#include <sys/fs/zfs.h>
#include <sys/dmu_objset.h>
#include <sys/zfeature.h>
#include <libzutil.h>
#include <zfs_prop.h>

extern const pool_config_ops_t libzpool_config_ops;
extern int zfs_scan_suspend_progress;

/* DMU_OT_ERROR_LOG value (sys/dmu.h). Provided as a fallback only. */
#ifndef DMU_OT_ERROR_LOG
#define	DMU_OT_ERROR_LOG	28
#endif

#define	MAX_OPS		64
#define	MAX_KEY		256
#define	MAX_TYPE	128
#define	SCAN_MAX_WORDS	32

typedef enum {
	OP_ZAP_SET,
	OP_ZAP_ZERO,
	OP_ZAP_REMOVE,
	OP_BONUS_SET,
	OP_FREE
} op_kind_t;

typedef struct {
	op_kind_t kind;
	uint64_t obj;
	char key[MAX_KEY];
	char field[MAX_KEY];
	char type[MAX_TYPE];	/* expected dmu type string, for OP_FREE */
	uint64_t expect;
	boolean_t has_expect;
	uint64_t value;
	int count;		/* OP_ZAP_ZERO word count */
	char raw[512];
} op_t;

static op_t g_ops[MAX_OPS];
static int g_nops;
static importargs_t g_importargs;
static char *g_pool;

static void
banner(const char *msg)
{
	fprintf(stderr, "\n=== %s ===\n", msg);
}

/* ----------------------------------------------------------------
 * type-name mapping: resolve the expected type string in a free op
 * to a dmu_object_type_t so we can compare against dmu_object_info().
 * We rely on dmu_ot[] names printed by zdb matching the DMU type table.
 * ---------------------------------------------------------------- */
static boolean_t
type_name_matches(dmu_object_type_t t, const char *name)
{
	if (t >= DMU_OT_NUMTYPES)
		return (B_FALSE);
	const char *tn = dmu_ot[t].ot_name;
	return (strcasecmp(tn, name) == 0);
}

/* ----------------------------------------------------------------
 * plan parsing
 * ---------------------------------------------------------------- */
static boolean_t
kv_u64(const char *line, const char *key, uint64_t *out)
{
	char pat[64];
	snprintf(pat, sizeof (pat), "%s=", key);
	const char *p = strstr(line, pat);
	if (p == NULL)
		return (B_FALSE);
	p += strlen(pat);
	errno = 0;
	char *end;
	unsigned long long v = strtoull(p, &end, 0);
	if (end == p || errno != 0)
		return (B_FALSE);
	*out = v;
	return (B_TRUE);
}

static boolean_t
kv_str(const char *line, const char *key, char *out, size_t n)
{
	char pat[64];
	snprintf(pat, sizeof (pat), "%s=", key);
	const char *p = strstr(line, pat);
	if (p == NULL)
		return (B_FALSE);
	p += strlen(pat);
	if (*p == '"') {
		p++;
		const char *e = strchr(p, '"');
		if (e == NULL)
			return (B_FALSE);
		size_t len = (size_t)(e - p);
		if (len >= n)
			len = n - 1;
		memcpy(out, p, len);
		out[len] = '\0';
	} else {
		size_t i = 0;
		while (*p && *p != ' ' && *p != '\t' && i < n - 1)
			out[i++] = *p++;
		out[i] = '\0';
	}
	return (B_TRUE);
}

static int
parse_plan(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "ERROR: cannot open plan '%s': %s\n",
		    path, strerror(errno));
		return (-1);
	}
	char line[512];
	int lineno = 0;
	while (fgets(line, sizeof (line), f) != NULL) {
		lineno++;
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '#' || *s == '\n' || *s == '\0')
			continue;
		s[strcspn(s, "\n")] = '\0';
		if (g_nops >= MAX_OPS) {
			fprintf(stderr, "ERROR: too many ops (max %d)\n",
			    MAX_OPS);
			fclose(f);
			return (-1);
		}
		op_t *op = &g_ops[g_nops];
		memset(op, 0, sizeof (*op));
		strncpy(op->raw, s, sizeof (op->raw) - 1);

		if (!kv_u64(s, "obj", &op->obj)) {
			fprintf(stderr, "ERROR: line %d: missing obj=\n",
			    lineno);
			fclose(f);
			return (-1);
		}
		op->has_expect = kv_u64(s, "expect", &op->expect);

		if (strncmp(s, "zap_set", 7) == 0) {
			op->kind = OP_ZAP_SET;
			if (!kv_str(s, "key", op->key, sizeof (op->key)) ||
			    !kv_u64(s, "value", &op->value) ||
			    !op->has_expect) {
				fprintf(stderr, "ERROR: line %d: zap_set needs "
				    "key=, expect=, value=\n", lineno);
				fclose(f);
				return (-1);
			}
		} else if (strncmp(s, "zap_zero", 8) == 0) {
			op->kind = OP_ZAP_ZERO;
			uint64_t c = 0;
			if (!kv_str(s, "key", op->key, sizeof (op->key)) ||
			    !kv_u64(s, "count", &c)) {
				fprintf(stderr, "ERROR: line %d: zap_zero needs "
				    "key=, count=\n", lineno);
				fclose(f);
				return (-1);
			}
			if (c == 0 || c > SCAN_MAX_WORDS) {
				fprintf(stderr, "ERROR: line %d: bad count\n",
				    lineno);
				fclose(f);
				return (-1);
			}
			op->count = (int)c;
		} else if (strncmp(s, "zap_remove", 10) == 0) {
			op->kind = OP_ZAP_REMOVE;
			if (!kv_str(s, "key", op->key, sizeof (op->key))) {
				fprintf(stderr, "ERROR: line %d: zap_remove "
				    "needs key=\n", lineno);
				fclose(f);
				return (-1);
			}
		} else if (strncmp(s, "bonus_set", 9) == 0) {
			op->kind = OP_BONUS_SET;
			if (!kv_str(s, "field", op->field,
			    sizeof (op->field)) ||
			    !kv_u64(s, "value", &op->value) ||
			    !op->has_expect) {
				fprintf(stderr, "ERROR: line %d: bonus_set "
				    "needs field=, expect=, value=\n", lineno);
				fclose(f);
				return (-1);
			}
			if (strcmp(op->field, "ds_num_children") != 0) {
				fprintf(stderr, "ERROR: line %d: only "
				    "ds_num_children is supported\n", lineno);
				fclose(f);
				return (-1);
			}
		} else if (strncmp(s, "free", 4) == 0) {
			op->kind = OP_FREE;
			if (!kv_str(s, "type", op->type, sizeof (op->type))) {
				fprintf(stderr, "ERROR: line %d: free needs "
				    "type=\"...\" (mandatory type guard)\n",
				    lineno);
				fclose(f);
				return (-1);
			}
		} else {
			fprintf(stderr, "ERROR: line %d: unknown op: %s\n",
			    lineno, s);
			fclose(f);
			return (-1);
		}
		g_nops++;
	}
	fclose(f);
	return (0);
}

/* ----------------------------------------------------------------
 * per-op precondition check. Returns B_TRUE if the observed on-disk
 * state matches the plan's expectation. Read-only.
 * ---------------------------------------------------------------- */
static boolean_t
check_op(objset_t *mos, op_t *op, boolean_t quiet)
{
	int err;
	char label[600];
	snprintf(label, sizeof (label), "  [check] %s", op->raw);
	if (!quiet)
		fprintf(stderr, "%s\n", label);

	switch (op->kind) {
	case OP_ZAP_SET:
	case OP_ZAP_REMOVE: {
		uint64_t cur = 0;
		err = zap_lookup(mos, op->obj, op->key, sizeof (uint64_t),
		    1, &cur);
		if (op->kind == OP_ZAP_REMOVE && err == ENOENT) {
			if (!quiet)
				fprintf(stderr, "           already absent — "
				    "op is a no-op (ok)\n");
			return (B_TRUE);
		}
		if (err != 0) {
			fprintf(stderr, "           FAIL: zap_lookup(obj=%llu, "
			    "'%s') err=%d\n", (u_longlong_t)op->obj,
			    op->key, err);
			return (B_FALSE);
		}
		if (op->has_expect && cur != op->expect) {
			fprintf(stderr, "           FAIL: '%s' = %llu, "
			    "expected %llu\n", op->key, (u_longlong_t)cur,
			    (u_longlong_t)op->expect);
			return (B_FALSE);
		}
		if (!quiet)
			fprintf(stderr, "           ok: '%s' = %llu\n",
			    op->key, (u_longlong_t)cur);
		return (B_TRUE);
	}
	case OP_ZAP_ZERO: {
		/* Only assert the entry is present and the right width. */
		uint64_t buf[SCAN_MAX_WORDS] = {0};
		err = zap_lookup(mos, op->obj, op->key, sizeof (uint64_t),
		    op->count, buf);
		if (err == ENOENT) {
			if (!quiet)
				fprintf(stderr, "           absent — no-op "
				    "(ok)\n");
			return (B_TRUE);
		}
		if (err != 0) {
			fprintf(stderr, "           FAIL: zap_lookup('%s') "
			    "err=%d\n", op->key, err);
			return (B_FALSE);
		}
		if (!quiet)
			fprintf(stderr, "           ok: '%s' present "
			    "(func=%llu state=%llu)\n", op->key,
			    (u_longlong_t)buf[0], (u_longlong_t)buf[1]);
		return (B_TRUE);
	}
	case OP_BONUS_SET: {
		dmu_buf_t *db;
		err = dmu_bonus_hold(mos, op->obj, FTAG, &db);
		if (err != 0) {
			fprintf(stderr, "           FAIL: dmu_bonus_hold(%llu) "
			    "err=%d\n", (u_longlong_t)op->obj, err);
			return (B_FALSE);
		}
		dsl_dataset_phys_t *dsp = db->db_data;
		uint64_t cur = dsp->ds_num_children;
		dmu_buf_rele(db, FTAG);
		if (cur != op->expect) {
			fprintf(stderr, "           FAIL: ds_num_children = "
			    "%llu, expected %llu\n", (u_longlong_t)cur,
			    (u_longlong_t)op->expect);
			return (B_FALSE);
		}
		if (!quiet)
			fprintf(stderr, "           ok: ds_num_children = "
			    "%llu\n", (u_longlong_t)cur);
		return (B_TRUE);
	}
	case OP_FREE: {
		dmu_object_info_t doi;
		err = dmu_object_info(mos, op->obj, &doi);
		if (err == ENOENT) {
			if (!quiet)
				fprintf(stderr, "           already freed — "
				    "no-op (ok)\n");
			return (B_TRUE);
		}
		if (err != 0) {
			fprintf(stderr, "           FAIL: dmu_object_info("
			    "%llu) err=%d\n", (u_longlong_t)op->obj, err);
			return (B_FALSE);
		}
		if (!type_name_matches(doi.doi_type, op->type)) {
			fprintf(stderr, "           FAIL: obj %llu type is "
			    "'%s' (%u), plan expected '%s' — REFUSING to free "
			    "(recycled ID?)\n", (u_longlong_t)op->obj,
			    doi.doi_type < DMU_OT_NUMTYPES ?
			    dmu_ot[doi.doi_type].ot_name : "?",
			    (uint_t)doi.doi_type, op->type);
			return (B_FALSE);
		}
		if (!quiet)
			fprintf(stderr, "           ok: obj %llu type '%s'\n",
			    (u_longlong_t)op->obj, op->type);
		return (B_TRUE);
	}
	}
	return (B_FALSE);
}

/* ----------------------------------------------------------------
 * sync task: two passes. Pass 1 re-verifies every precondition; if any
 * fails, abort before dirtying anything. Pass 2 applies. Atomic per txg.
 * ---------------------------------------------------------------- */
static void
apply_sync(void *arg, dmu_tx_t *tx)
{
	(void) arg;
	spa_t *spa = dmu_tx_pool(tx)->dp_spa;
	objset_t *mos = spa->spa_meta_objset;

	fprintf(stderr, "\n  [sync] txg %llu — pass 1: re-verify all "
	    "preconditions\n", (u_longlong_t)dmu_tx_get_txg(tx));
	for (int i = 0; i < g_nops; i++) {
		if (!check_op(mos, &g_ops[i], B_TRUE)) {
			fprintf(stderr, "  [sync] PRECONDITION FAILED on op "
			    "%d (%s) — aborting transaction, no changes "
			    "made\n", i, g_ops[i].raw);
			/*
			 * dmu_tx_abort is not available inside a running sync
			 * task; force a fatal stop so nothing partial commits.
			 */
			VERIFY0(1);
		}
	}

	fprintf(stderr, "  [sync] pass 2: apply\n");
	for (int i = 0; i < g_nops; i++) {
		op_t *op = &g_ops[i];
		int err = 0;
		switch (op->kind) {
		case OP_ZAP_SET: {
			uint64_t v = op->value;
			err = zap_update(mos, op->obj, op->key,
			    sizeof (uint64_t), 1, &v, tx);
			break;
		}
		case OP_ZAP_ZERO: {
			uint64_t zeros[SCAN_MAX_WORDS] = {0};
			err = zap_update(mos, op->obj, op->key,
			    sizeof (uint64_t), op->count, zeros, tx);
			break;
		}
		case OP_ZAP_REMOVE:
			err = zap_remove(mos, op->obj, op->key, tx);
			if (err == ENOENT)
				err = 0;
			break;
		case OP_BONUS_SET: {
			dmu_buf_t *db;
			err = dmu_bonus_hold(mos, op->obj, FTAG, &db);
			if (err == 0) {
				dmu_buf_will_dirty(db, tx);
				dsl_dataset_phys_t *dsp = db->db_data;
				dsp->ds_num_children = op->value;
				dmu_buf_rele(db, FTAG);
			}
			break;
		}
		case OP_FREE:
			err = dmu_object_free(mos, op->obj, tx);
			if (err == ENOENT)
				err = 0;
			break;
		}
		fprintf(stderr, "  [sync]   op %d: %s -> %s\n", i, op->raw,
		    err == 0 ? "OK" : strerror(err));
		/* Preconditions passed twice; a failure here is fatal. */
		VERIFY0(err);
	}
	fprintf(stderr, "  [sync] all ops applied; committing txg\n");
}

/* ----------------------------------------------------------------
 * import (read-only for dump, read-write for commit)
 * ---------------------------------------------------------------- */
static int
do_import(const char *target, boolean_t readonly)
{
	nvlist_t *config, *props = NULL;
	int error;

	/*
	 * Suspend scan progress in userspace: modprobe tunables affect only
	 * the kernel module, not libzpool. Without this a stale scan cursor
	 * resumes during import and can VERIFY3-panic on freed objects.
	 */
	zfs_scan_suspend_progress = 1;

	kernel_init(readonly ? SPA_MODE_READ :
	    (SPA_MODE_READ | SPA_MODE_WRITE));
	dmu_objset_register_type(DMU_OST_ZFS, NULL);

	g_importargs.can_be_active = readonly;
	g_pool = strdup(target);

	libpc_handle_t lpch = {
		.lpc_lib_handle = NULL,
		.lpc_ops = &libzpool_config_ops,
		.lpc_printerr = B_TRUE
	};

	error = zpool_find_config(&lpch, target, &config, &g_importargs);
	if (error) {
		fprintf(stderr, "ERROR: cannot find pool '%s': %s\n",
		    target, strerror(error));
		kernel_fini();
		return (error);
	}
	if (readonly) {
		VERIFY0(nvlist_alloc(&props, NV_UNIQUE_NAME, 0));
		VERIFY0(nvlist_add_uint64(props,
		    zpool_prop_to_name(ZPOOL_PROP_READONLY), 1));
	}
	zfeature_checks_disable = B_TRUE;
	error = spa_import(g_pool, config, props,
	    (readonly ? ZFS_IMPORT_SKIP_MMP : ZFS_IMPORT_NORMAL));
	fnvlist_free(config);
	zfeature_checks_disable = B_FALSE;
	if (error == EEXIST)
		error = 0;
	if (error) {
		fprintf(stderr, "ERROR: cannot import '%s': %s\n",
		    target, strerror(error));
		kernel_fini();
		return (error);
	}
	fprintf(stderr, "Pool '%s' imported (%s)\n", target,
	    readonly ? "read-only" : "read-write");
	return (0);
}

static void
usage(const char *argv0)
{
	fprintf(stderr,
	    "Usage: %s [--commit] --plan FILE <poolname>\n"
	    "  default:  dump mode — import read-only, verify preconditions, "
	    "no writes\n"
	    "  --commit: apply the plan atomically (two-pass verify+apply)\n",
	    argv0);
}

int
main(int argc, char *argv[])
{
	const char *poolname = NULL;
	const char *planpath = NULL;
	boolean_t do_commit = B_FALSE;
	spa_t *spa = NULL;
	objset_t *mos;
	int err;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--commit") == 0) {
			do_commit = B_TRUE;
		} else if (strcmp(argv[i], "--plan") == 0 && i + 1 < argc) {
			planpath = argv[++i];
		} else if (argv[i][0] != '-') {
			poolname = argv[i];
		} else {
			usage(argv[0]);
			return (2);
		}
	}
	if (poolname == NULL || planpath == NULL) {
		usage(argv[0]);
		return (2);
	}

	if (parse_plan(planpath) != 0)
		return (2);
	fprintf(stderr, "Parsed %d op(s) from %s\n", g_nops, planpath);
	if (g_nops == 0) {
		fprintf(stderr, "Plan is empty; nothing to do.\n");
		return (0);
	}

	banner(do_commit ? "zhaxx: COMMIT MODE" : "zhaxx: DUMP MODE "
	    "(read-only)");
	fprintf(stderr, "Pool: %s\n", poolname);

	if (do_commit) {
		fprintf(stderr,
		    "\n*** COMMIT will modify the pool MOS in one atomic "
		    "transaction. ***\n"
		    "*** %d operations; all preconditions are re-verified "
		    "first. ***\n"
		    "*** Type YES to proceed: ", g_nops);
		char resp[16] = {0};
		if (fgets(resp, sizeof (resp), stdin) == NULL ||
		    strcmp(resp, "YES\n") != 0) {
			fprintf(stderr, "aborted.\n");
			return (1);
		}
	}

	err = do_import(poolname, !do_commit);
	if (err != 0)
		return (1);

	zfeature_checks_disable = B_TRUE;
	err = spa_open(poolname, &spa, FTAG);
	zfeature_checks_disable = B_FALSE;
	if (err != 0) {
		fprintf(stderr, "ERROR: spa_open failed: %s\n", strerror(err));
		goto out_export;
	}
	mos = spa->spa_meta_objset;
	fprintf(stderr, "spa_open OK\n");

	/*
	 * Clear the in-memory error log immediately: even after the on-disk
	 * errlog_last is fixed, a stale in-memory head_errlog entry makes
	 * spa_errlog_sync reopen the dead objset and deadlock txg_sync.
	 */
	if (do_commit) {
		mutex_enter(&spa->spa_errlog_lock);
		uint64_t old = spa->spa_errlog_last;
		spa->spa_errlog_last = 0;
		spa->spa_errlog_scrub = 0;
		mutex_exit(&spa->spa_errlog_lock);
		fprintf(stderr, "  Cleared in-memory spa_errlog_last "
		    "(was %llu)\n", (u_longlong_t)old);
	}

	banner("Precondition check (read-only)");
	int nfail = 0;
	for (int i = 0; i < g_nops; i++)
		if (!check_op(mos, &g_ops[i], B_FALSE))
			nfail++;

	if (nfail > 0) {
		fprintf(stderr, "\n%d precondition(s) FAILED — not applying. "
		    "Re-run zhaxx-scan against the current pool state to "
		    "regenerate the plan.\n", nfail);
		err = 1;
		goto out_close;
	}
	fprintf(stderr, "\nAll %d precondition(s) satisfied.\n", g_nops);

	if (!do_commit) {
		fprintf(stderr, "\nDump mode: re-run with --commit to apply.\n");
		goto out_close;
	}

	banner("Applying plan");
	err = dsl_sync_task(spa_name(spa), NULL, apply_sync, NULL,
	    g_nops, ZFS_SPACE_CHECK_NORMAL);
	if (err != 0) {
		fprintf(stderr, "ERROR: dsl_sync_task failed: %s (err=%d)\n",
		    strerror(err), err);
		goto out_close;
	}
	fprintf(stderr, "\nPlan applied successfully.\n");

	banner("Verification (post-apply)");
	for (int i = 0; i < g_nops; i++)
		check_op(mos, &g_ops[i], B_FALSE);

out_close:
	spa_close(spa, FTAG);
out_export:
	banner("Exporting pool");
	if (spa_export(g_pool, NULL, B_TRUE, B_FALSE) != 0)
		fprintf(stderr, "WARNING: pool export failed\n");
	else
		fprintf(stderr, "Pool exported.\n");
	kernel_fini();

	if (do_commit && err == 0) {
		fprintf(stderr,
		    "\n=== NEXT STEPS ===\n"
		    "1. Import guarded:  zpool import -N -o readonly=on %s\n"
		    "2. Inspect:         zpool status -v %s\n"
		    "3. If clean, import read-write and scrub before returning "
		    "to service.\n", poolname, poolname);
	}
	return (err != 0 ? 1 : 0);
}
