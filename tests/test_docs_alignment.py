"""Verify docs/user/cli-contract.md lists every CLI command registered in src/cli.cpp.

This test is a pure file-system check; it does not need a flashed device or
network connectivity. Marked as `contract` so it runs alongside the rest of
the contract suite.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
CLI_SOURCE = REPO_ROOT / "src" / "cli.cpp"
CLI_CONTRACT = REPO_ROOT / "docs" / "user" / "cli-contract.md"

# Names that are intentionally not part of the user-facing contract surface.
# Add a justification line here if a new entry shows up — never silence the
# test by dropping a real command in.
INTERNAL_ONLY: set[str] = set()

# kCliCommands rows look like:
#   {"name", mutating, tcp_readonly, "usage", handler},
_CMD_LINE = re.compile(r'^\s*\{"([A-Za-z0-9_.]+)"\s*,')


def _extract_command_table_block(source: str) -> str:
    """Return the substring spanning `kCliCommands[] = {` ... `};`."""
    start = source.find("kCliCommands[]")
    assert start != -1, "could not find kCliCommands[] in cli.cpp"
    brace = source.find("{", start)
    assert brace != -1, "kCliCommands[] is missing its opening brace"
    depth = 0
    end = brace
    for idx in range(brace, len(source)):
        ch = source[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = idx
                break
    assert end > brace, "kCliCommands[] closing brace not found"
    return source[brace : end + 1]


def _registered_commands() -> set[str]:
    text = CLI_SOURCE.read_text(encoding="utf-8")
    block = _extract_command_table_block(text)
    names: set[str] = set()
    for line in block.splitlines():
        match = _CMD_LINE.match(line)
        if match:
            names.add(match.group(1))
    assert names, "parsed zero command rows from kCliCommands[]"
    return names


def _documented_commands() -> set[str]:
    text = CLI_CONTRACT.read_text(encoding="utf-8")
    # Doc rows look like `| `cmd_name` | ... |` — pull the first backticked
    # token from any table row that has at least three pipes.
    pattern = re.compile(r"^\|\s*`([A-Za-z0-9_.]+)`\s*\|.*\|.*\|")
    names: set[str] = set()
    for line in text.splitlines():
        match = pattern.match(line)
        if match:
            names.add(match.group(1))
    assert names, "parsed zero command rows from cli-contract.md"
    return names


@pytest.mark.contract
def test_every_registered_command_is_documented() -> None:
    registered = _registered_commands()
    documented = _documented_commands()
    missing = registered - documented - INTERNAL_ONLY
    assert not missing, (
        "cli.cpp registers commands that are absent from "
        "docs/user/cli-contract.md: " + ", ".join(sorted(missing))
    )


@pytest.mark.contract
def test_no_documented_command_is_phantom() -> None:
    registered = _registered_commands()
    documented = _documented_commands()
    phantom = documented - registered
    assert not phantom, (
        "docs/user/cli-contract.md lists commands that are not in cli.cpp: "
        + ", ".join(sorted(phantom))
    )
