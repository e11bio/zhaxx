#!/usr/bin/env bash
# SPDX-License-Identifier: CDDL-1.0
#
# build.sh — compile the zhaxx surgeon against the installed ZFS on this host.
#
# The surgeon links libzpool so its on-disk-format interpretation matches the
# running pool. That means it must build against the SAME ZFS version that
# owns the pool. This script autodetects the source tree and libraries and
# links by soname directly (-l:libzpool.so.N), avoiding the historical hack
# of creating dev symlinks in /usr/lib.
#
# Env overrides:
#   ZFS_SRC   path to the matching ZFS source/headers (default: autodetect)
#   LIBDIR    directory holding libzpool.so.* (default: autodetect)
#   CC        compiler (default: gcc)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-gcc}"

fail() { echo "build.sh: $*" >&2; exit 1; }

# --- locate the ZFS headers, preferring the version that owns a live pool ---
detect_zfs_version() {
	command -v zfs >/dev/null 2>&1 || return 0
	zfs version 2>/dev/null | head -1 | sed 's/^zfs-//'
}

if [[ -z "${ZFS_SRC:-}" ]]; then
	want="$(detect_zfs_version || true)"
	for c in \
		${want:+/usr/src/zfs-${want}} \
		/usr/src/zfs-*/ \
		/usr/src/zfs \
		./zfs; do
		if [[ -d "$c/include/sys" ]]; then ZFS_SRC="$c"; break; fi
	done
fi
[[ -n "${ZFS_SRC:-}" && -d "$ZFS_SRC/include/sys" ]] || \
	fail "no ZFS source tree found; set ZFS_SRC=/usr/src/zfs-<ver>"

# --- locate libzpool ---
if [[ -z "${LIBDIR:-}" ]]; then
	for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib /lib64; do
		if compgen -G "$d/libzpool.so.*" >/dev/null; then LIBDIR="$d"; break; fi
	done
fi
[[ -n "${LIBDIR:-}" ]] && compgen -G "$LIBDIR/libzpool.so.*" >/dev/null || \
	fail "libzpool.so not found; set LIBDIR to the directory containing it"

# --- resolve exact sonames so we can link without dev symlinks ---
soname() {
	local base="$1" f
	f="$(compgen -G "$LIBDIR/${base}.so.*" | sort -V | tail -1)" || true
	[[ -n "$f" ]] || fail "missing $base in $LIBDIR"
	basename "$f"
}
L_ZPOOL="$(soname libzpool)"
L_NVPAIR="$(soname libnvpair)"
L_UUTIL="$(soname libuutil)"
L_ZFSCORE="$(soname libzfs_core)"

# --- version sanity: headers vs library ---
hdr_ver="$(basename "$ZFS_SRC" | sed 's/^zfs-//')"
lib_ver="${L_ZPOOL#libzpool.so.}"
echo "ZFS source : $ZFS_SRC  (label: $hdr_ver)"
echo "libzpool   : $LIBDIR/$L_ZPOOL"
if [[ -n "$(detect_zfs_version || true)" ]]; then
	live="$(detect_zfs_version)"
	case "$hdr_ver" in
	"$live"*) : ;;
	*) echo "WARNING: source tree ($hdr_ver) != live zfs ($live). The" >&2
	   echo "         surgeon MUST match the version that owns the pool." >&2;;
	esac
fi

echo "=== compiling zhaxx ==="
# Include order: vendored compat (userspace libspl shims) first, then the
# ZFS source headers. -include stdint.h forces fixed-width types ahead of
# stdlib.h. We deliberately do NOT include the kernel-space os/linux/{spl,zfs}
# trees — compat/ replaces them for userspace.
set -x
"$CC" -o "$here/zhaxx" "$here/zhaxx.c" \
	-include stdint.h \
	-I"$here/compat" \
	-I"$here/compat/os/linux" \
	-I"$ZFS_SRC/include" \
	-D_GNU_SOURCE \
	-DLIB_ZPOOL_BUILD \
	-Wall \
	-l:"$L_ZPOOL" -l:"$L_NVPAIR" -l:"$L_UUTIL" -l:"$L_ZFSCORE" \
	-luuid -lblkid -lm -lz -ltirpc -lpthread \
	-L"$LIBDIR" -Wl,-rpath,"$LIBDIR"
set +x

echo "=== built: $here/zhaxx ==="
echo "Usage: sudo $here/zhaxx --plan PLAN <poolname>   # dump (read-only)"
echo "       sudo $here/zhaxx --commit --plan PLAN <poolname>"
