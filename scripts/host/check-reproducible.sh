#!/usr/bin/env bash
# Check that the rootfs build is reproducible at the content level.
#
# Why this is not just "compare image hashes"
# -----------------------------------------
# tools/pack-rootfs.sh populates the ext4 image with `mke2fs -d`, which records
# each file's *ctime* as well as its mtime. ctime is maintained by the kernel and
# cannot be set from userspace, so two rootfs trees built at different times are
# never byte-identical even when their contents match exactly. Re-packing one
# fixed tree is deterministic, and file mtimes are pinned by
# SOURCE_DATE_EPOCH, but a fresh build always lands on a fresh ctime.
#
# What is actually verified here is the property that matters: two independent
# runs of the builder produce the same tree, byte for byte, ignoring the
# timestamps the kernel owns. That is what "reproducible" can mean without a
# privileged or containerised build.
set -Eeuo pipefail
export LC_ALL=C

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

ROOTFS_DIR=${ROOTFS_DIR:-"$ROOT/artifacts/generated/halium-rootfs-arm64"}
BUILDER=${BUILDER:-"$SCRIPT_DIR/build-rootfs-arm64.sh"}
WORK=${WORK:-/tmp/d2q-repro}

usage() {
    cat <<'EOF'
Usage: tools/check-reproducible.sh

Builds the ARM64 rootfs twice into separate directories and compares both trees
by path, type, mode, symlink target and file content hash. ctime is excluded
because the kernel owns it.
EOF
}

[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && { usage; exit 0; }

command -v sha256sum >/dev/null 2>&1 || { printf 'error: sha256sum missing\n' >&2; exit 1; }

# manifest <rootfs> <outfile>
# One line per entry: type, mode, path, and either the symlink target or the
# content hash. Traversal is sorted so the ordering is stable.
manifest() {
    local root=$1 out=$2 path
    (
        cd "$root"
        while IFS= read -r -d '' path; do
            if [[ -L "$path" ]]; then
                printf 'l %s\t%s\t-> %s\n' "$(stat -c '%a' "$path")" "$path" "$(readlink "$path")"
            elif [[ -d "$path" ]]; then
                printf 'd %s\t%s\n' "$(stat -c '%a' "$path")" "$path"
            else
                printf 'f %s\t%s\t%s\n' "$(stat -c '%a' "$path")" "$path" \
                    "$(sha256sum "$path" | cut -d' ' -f1)"
            fi
        done < <(find . \( -type d -o -type f -o -type l \) -print0 | sort -z)
    ) > "$out"
}

rm -rf "$WORK"
mkdir -p "$WORK"

printf '==> Build 1 of 2\n'
ROOTFS_DIR="$WORK/a" "$BUILDER" >/dev/null
printf '==> Build 2 of 2\n'
ROOTFS_DIR="$WORK/b" "$BUILDER" >/dev/null

printf '==> Comparing\n'
manifest "$WORK/a" "$WORK/a.manifest"
manifest "$WORK/b" "$WORK/b.manifest"

na=$(wc -l < "$WORK/a.manifest")
nb=$(wc -l < "$WORK/b.manifest")
printf '    build 1: %s entries\n' "$na"
printf '    build 2: %s entries\n' "$nb"

if ! diff -u "$WORK/a.manifest" "$WORK/b.manifest" > "$WORK/diff.txt"; then
    printf '==> MISMATCH: the two builds differ\n' >&2
    head -40 "$WORK/diff.txt" >&2
    printf '    full diff: %s\n' "$WORK/diff.txt" >&2
    exit 1
fi

printf '==> Reproducible: both builds are byte-identical by\n'
printf '    path, type, mode, symlink target and content hash.\n'
printf '    (ctime is excluded: the kernel sets it and mke2fs -d records it,\n'
printf '     so two separate builds can never yield identical image bytes.)\n'
printf '    reference: %s\n' "$ROOTFS_DIR"
rm -rf "$WORK"
