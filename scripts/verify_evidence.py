#!/usr/bin/env python3
"""Validate committed LUCE local-evidence manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = REPO_ROOT / "docs" / "governance" / "health" / "evidence-manifest.json"
PARENT_ONLY_FILES = {
    "docs/governance/health/evidence-latest.json",
    "docs/governance/health/evidence-manifest.json",
}


def git(args: list[str]) -> str:
    return subprocess.check_output(["git", *args], cwd=REPO_ROOT, text=True).strip()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def accept_source_sha(source_sha: str) -> tuple[bool, str]:
    head = git(["rev-parse", "HEAD"])
    if source_sha == head:
        return True, "exact-head"
    try:
        parent = git(["rev-parse", "HEAD^"])
    except subprocess.CalledProcessError:
        return False, f"manifest git_sha {source_sha} does not match HEAD {head}"
    if source_sha != parent:
        return False, f"manifest git_sha {source_sha} matches neither HEAD {head} nor HEAD^ {parent}"
    changed = set(git(["diff", "--name-only", f"{parent}..HEAD"]).splitlines())
    if changed and changed.issubset(PARENT_ONLY_FILES):
        return True, "parent-plus-manifest"
    return False, f"manifest git_sha is parent {parent}, but HEAD also changed non-manifest files: {sorted(changed)}"


def is_release_context(manifest: dict, forced: bool) -> bool:
    if forced or bool(manifest.get("release", False)):
        return True
    target = str(manifest.get("source", {}).get("target", ""))
    if target.startswith("release"):
        return True
    ref_name = os.getenv("GITHUB_REF_NAME", "")
    ref = os.getenv("GITHUB_REF", "")
    return ref_name == "main" or ref_name.startswith("release/") or ref == "refs/heads/main" or ref.startswith("refs/heads/release/")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", default=str(DEFAULT_MANIFEST))
    parser.add_argument("--release", action="store_true", help="Require release-grade hardware HIL evidence.")
    args = parser.parse_args(argv)

    manifest_path = Path(args.manifest)
    if not manifest_path.is_absolute():
        manifest_path = REPO_ROOT / manifest_path
    if not manifest_path.exists():
        print(f"missing evidence manifest: {manifest_path}", file=sys.stderr)
        return 1

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    source_sha = str(manifest.get("source", {}).get("git_sha", ""))
    ok, reason = accept_source_sha(source_sha)
    if not ok:
        print(reason, file=sys.stderr)
        return 1

    failures: list[str] = []
    checks = manifest.get("checks", [])
    for check in checks:
        name = check.get("name", "<unnamed>")
        required = bool(check.get("required", True))
        status = check.get("status", "")
        if required and status != "PASS":
            failures.append(f"{name}: required status is {status!r}, expected 'PASS'")

    if is_release_context(manifest, args.release):
        hil = next((check for check in checks if check.get("name") == "hardware-hil"), None)
        if hil is None:
            failures.append("hardware-hil: missing release evidence check")
        elif not bool(hil.get("required", False)) or hil.get("status") != "PASS":
            failures.append("hardware-hil: release evidence must be required=true and status='PASS'")

    for artifact in manifest.get("artifacts", []):
        path = REPO_ROOT / artifact["path"]
        expected = artifact["sha256"]
        if not path.exists():
            failures.append(f"artifact missing: {artifact['path']}")
            continue
        actual = sha256_file(path)
        if actual != expected:
            failures.append(f"artifact checksum mismatch: {artifact['path']} expected {expected} got {actual}")

    if failures:
        for item in failures:
            print(item, file=sys.stderr)
        return 1

    print(f"evidence manifest OK ({reason})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
