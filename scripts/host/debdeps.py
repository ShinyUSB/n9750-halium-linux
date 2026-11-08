#!/usr/bin/env python3
"""Resolve a Debian package dependency closure from Packages indexes.

Runs entirely on the host: it reads compressed ``Packages`` indexes, computes the
closure of the requested packages, and prints one ``name version filename
sha256`` record per line. It never unpacks anything and never executes a
target-architecture binary, so it works without qemu, dpkg, or root.

The caller is responsible for downloading the files and verifying the SHA256.
"""

import argparse
import gzip
import sys

# Version comparison. Debian versions are epoch:upstream-revision with a
# possible trailing debian_revision, compared per Debian policy.
def parse_version(v):
    epoch = 0
    if ":" in v:
        e, v = v.split(":", 1)
        epoch = int(e)
    if "-" in v:
        upstream, _, revision = v.rpartition("-")
    else:
        upstream, revision = v, ""
    return epoch, split_version(upstream), split_version(revision)


def split_version(s):
    out = []
    for part in s.split("."):
        digits = ""
        i = 0
        while i < len(part) and part[i].isdigit():
            digits += part[i]
            i += 1
        if digits:
            out.append((1, int(digits), part[i:]))
        else:
            out.append((0, 0, part))
    return out


def cmp_part(a, b):
    # Non-digit parts sort before digit parts; "" sorts before anything else.
    ka, kb = a[0], b[0]
    if ka != kb:
        return -1 if ka < kb else 1
    if ka == 1:
        if a[1] != b[1]:
            return -1 if a[1] < b[1] else 1
    if a[2] == b[2]:
        return 0
    if a[2] == "":
        return -1
    if b[2] == "":
        return 1
    return -1 if a[2] < b[2] else 1


def cmp_version(a, b):
    ea, va, ra = parse_version(a)
    eb, vb, rb = parse_version(b)
    if ea != eb:
        return -1 if ea < eb else 1
    for x, y in zip(va, vb):
        c = cmp_part(x, y)
        if c:
            return c
    if len(va) != len(vb):
        return -1 if len(va) < len(vb) else 1
    for x, y in zip(ra, rb):
        c = cmp_part(x, y)
        if c:
            return c
    if len(ra) != len(rb):
        return -1 if len(ra) < len(rb) else 1
    return 0


class Pkg:
    __slots__ = ("name", "version", "arch", "filename", "sha256", "depends",
                 "pre_depends", "provides", "priority", "section", "size")

    def __repr__(self):
        return f"{self.name}={self.version}/{self.arch}"


def parse_stanza(lines):
    if not lines:
        return None
    d = {}
    key = None
    for raw in lines:
        if not raw.strip():
            continue
        if raw[0] in " \t":
            if key:
                d[key] += " " + raw.strip()
            continue
        if ":" in raw:
            key, _, val = raw.partition(":")
            key = key.strip()
            d[key] = val.strip()
    p = Pkg()
    p.name = d.get("Package")
    if not p.name:
        return None
    p.version = d.get("Version", "")
    p.arch = d.get("Architecture", "all")
    p.filename = d.get("Filename", "")
    p.sha256 = d.get("SHA256", "")
    p.depends = d.get("Depends", "")
    p.pre_depends = d.get("Pre-Depends", "")
    p.provides = d.get("Provides", "")
    p.priority = d.get("Priority", "")
    p.section = d.get("Section", "")
    try:
        p.size = int(d.get("Size", "0"))
    except ValueError:
        p.size = 0
    return p


def load_index(paths):
    """Load every stanza from the given Packages[.gz] files.

    Later files override earlier ones for the same package name, which is how
    -updates and -security suites are layered on top of the release.
    """
    by_name = {}
    for path in paths:
        opener = gzip.open if path.endswith(".gz") else open
        with opener(path, "rt", encoding="utf-8", errors="replace") as fh:
            stanza = []
            for line in fh:
                if line.strip() == "":
                    p = parse_stanza(stanza)
                    if p:
                        by_name[p.name] = p
                    stanza = []
                else:
                    stanza.append(line.rstrip("\n"))
            p = parse_stanza(stanza)
            if p:
                by_name[p.name] = p
    return by_name


def parse_relations(field):
    """Split a dependency field into a list of alternatives.

    ``a (>= 1) | b`` becomes ``[['a', (>=, '1')], ['b']]``.
    """
    groups = []
    if not field:
        return groups
    for clause in field.split(","):
        clause = clause.strip()
        if not clause:
            continue
        alts = []
        for alt in clause.split("|"):
            alt = alt.strip()
            if not alt:
                continue
            if "(" in alt:
                name = alt.split("(", 1)[0].strip()
                rest = alt.split("(", 1)[1].rstrip(") ").strip()
                op = ver = None
                for candidate in (">=", "<=", ">>", "<<", "=", ">", "<"):
                    if rest.startswith(candidate):
                        op, ver = candidate, rest[len(candidate):].strip()
                        break
                alts.append((name, (op, ver) if op else None))
            else:
                alts.append((alt, None))
        if alts:
            groups.append(alts)
    return groups


def provides_map(by_name):
    m = {}
    for p in by_name.values():
        for alt in parse_relations(p.provides):
            for name, _ in alt:
                m.setdefault(name, set()).add(p.name)
    return m


def satisfied(name, constraint, by_name, installed, provides, required_by):
    """Return the real package satisfying a dependency, or None."""
    candidates = []
    if name in by_name:
        candidates.append(name)
    for real in provides.get(name, ()):  # virtual packages
        candidates.append(real)
    # Already-installed packages satisfy by name even if not in the index.
    for c in candidates:
        if constraint is None or constraint[0] is None:
            return c
        op, ver = constraint
        p = by_name.get(c) or Pkg()
        pv = p.version if p.version else installed.get(c)
        if pv is None:
            continue
        if op == ">=" and cmp_version(pv, ver) >= 0:
            return c
        if op == "<=" and cmp_version(pv, ver) <= 0:
            return c
        if op == ">" and cmp_version(pv, ver) > 0:
            return c
        if op == "<" and cmp_version(pv, ver) < 0:
            return c
        if op == "=" and cmp_version(pv, ver) == 0:
            return c
        if op == ">>" and cmp_version(pv, ver) >= 0:
            return c
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--index", action="append", required=True,
                    help="Packages or Packages.gz file (repeatable; later wins)")
    ap.add_argument("--installed", help="dpkg status file of the base rootfs")
    ap.add_argument("--package", action="append", default=[],
                    help="package to install (repeatable)")
    ap.add_argument("--ignore", action="append", default=[],
                    help="package name to pretend is already satisfied")
    ap.add_argument("--no-recommends", action="store_true", default=True)
    ap.add_argument("--output", help="write records here instead of stdout")
    args = ap.parse_args()

    by_name = load_index(args.index)
    provides = provides_map(by_name)

    installed = {}
    if args.installed:
        with open(args.installed, encoding="utf-8", errors="replace") as fh:
            current = {}
            for line in fh:
                if line.strip() == "":
                    if current.get("Package"):
                        installed[current["Package"]] = current.get("Version", "")
                    current = {}
                elif line[0] not in " \t" and ":" in line:
                    k, _, v = line.partition(":")
                    current[k.strip()] = v.strip()
            if current.get("Package"):
                installed[current["Package"]] = current.get("Version", "")

    satisfied_already = set(installed) | set(args.ignore)

    # requested -> set of dependent names, for a useful error message
    required_by = {}
    resolved = {}     # real package name -> Pkg
    queue = []

    def want(name, constraint, depender):
        if name in satisfied_already:
            return
        for real in provides.get(name, ()):
            if real in satisfied_already:
                return
        real = satisfied(name, constraint, by_name, installed, provides, required_by)
        if real is None:
            if name in by_name:
                real = name
            else:
                raise SystemExit(
                    f"error: cannot satisfy {name!r} required by {depender!r} "
                    f"(no candidate in the index; is a suite/index missing?)")
        if real in resolved or real in satisfied_already:
            # Already handled, but double-check the version constraint.
            if real in resolved and constraint and constraint[0]:
                op, ver = constraint
                pv = resolved[real].version
                ok = {
                    ">=": lambda: cmp_version(pv, ver) >= 0,
                    "<=": lambda: cmp_version(pv, ver) <= 0,
                    ">": lambda: cmp_version(pv, ver) > 0,
                    "<": lambda: cmp_version(pv, ver) < 0,
                    "=": lambda: cmp_version(pv, ver) == 0,
                    ">>": lambda: cmp_version(pv, ver) >= 0,
                }.get(op, lambda: True)()
                if not ok:
                    raise SystemExit(
                        f"error: {real} {pv} does not satisfy {name}{op}{ver} "
                        f"required by {depender}")
            return
        p = by_name[real]
        resolved[real] = p
        required_by.setdefault(real, []).append(depender)
        queue.append(real)

    for name in args.package:
        want(name, None, "<command line>")

    while queue:
        name = queue.pop()
        p = resolved[name]
        for field in (p.pre_depends, p.depends):
            for alts in parse_relations(field):
                last_error = None
                done = False
                for dep_name, constraint in alts:
                    try:
                        want(dep_name, constraint, name)
                        done = True
                        break
                    except SystemExit as exc:
                        last_error = exc
                if not done:
                    raise SystemExit(
                        f"error: no alternative satisfies a dependency of {name}\n"
                        f"       {last_error}")

    records = []
    for name, p in resolved.items():
        if not p.filename or not p.sha256:
            raise SystemExit(
                f"error: {name} has no Filename/SHA256; refusing to emit an "
                f"unverifiable record")
        records.append(f"{name}\t{p.version}\t{p.arch}\t{p.filename}\t{p.sha256}\t{p.size}")

    records.sort()
    out = "\n".join(records) + "\n"
    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(out)
        total = sum(int(r.split("\t")[5]) for r in records)
        print(f"{len(records)} packages, {total} bytes to download", file=sys.stderr)
    else:
        sys.stdout.write(out)


if __name__ == "__main__":
    main()
