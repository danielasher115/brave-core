#!/usr/bin/env vpython3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.
"""Helpers for the autonomous Chromium lift described in tools/cr/lift/chromium_lift.md.

Subcommands:
    prepare N [--no-rebase] [--force]
        Resets the local cr<N> branch to origin/cr<N>, sets its upstream to the
        base (origin/cr<N-1>, or origin/master once master is on milestone N-1)
        and starts `brockit rebase --squash-minor-bumps --discard-regen-changes`.
    build-summary LOG
        Summarises a `pnpm run build` log: the final status line, the steps
        that failed, and each distinct error with how often it occurred.
    culprits RANGE [--symbol S] [PATH ...]
        Lists the upstream commits in RANGE (e.g. 158.0.8117.1..158.0.8127.2)
        that touch PATH, or that add or remove the string S, with full hashes.
    verify-culprits BASE RANGE
        Checks that every culprit hash cited in the brave-core commits after
        BASE names a real Chromium commit inside RANGE.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys

import _boot  # noqa: F401  -- adds parent tools/cr to sys.path
import repository  # noqa: E402  -- after _boot
from versioning import Version  # noqa: E402  -- after _boot

_BROCKIT = Path(__file__).resolve().parent.parent / 'brockit.py'

_ANSI_ESCAPE = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')

_STATUS_LINE = re.compile(
    r'Build (?:Failure|Succeeded):.*|The build has finished successfully\.'
)
_FAILED_STEP = re.compile(r'FAILED: \S+ (.*)')
# Leading progress dots, but not the dots of a relative path such as ../../a.
_PROGRESS_DOTS = re.compile(r'^\.+(?=[^./])')

# Each pattern captures the part of an error line worth deduplicating on. Paths
# and line numbers are kept for compile errors so that each failing location is
# reported once.
_ERROR_PATTERNS = (
    # clang: ../../foo/bar.cc:12:3: error: message
    re.compile(r'^(\S+:\d+:\d+: error: .*)$'),
    # javac / turbine: ../../Foo.java:12: error: message
    re.compile(r'^(\S+\.java:\d+: error: .*)$'),
    # gn gen / gn check
    re.compile(r'^(ERROR at .*)$'),
    # TypeScript: file.ts:6:41 - error TS2307: message
    re.compile(r'^(\S+\.ts:\d+:\d+ - error TS\d+: .*)$'),
    # eslint: "  25:65  error  message  rule-name"
    re.compile(r'^\s+\d+:\d+\s+error\s+(.*)$'),
    # linkers
    re.compile(r'^(\S+: error: undefined symbol: .*)$'),
    # d8 / r8
    re.compile(r'^(Type \S+ is defined multiple times: .*)$'),
    # siso scheduling
    re.compile(r'^(.* missing and no known rule to make it)$'),
    # Python tracebacks from build actions
    re.compile(r'^((?:ModuleNotFoundError|ImportError|AssertionError): .*)$'),
)

_CULPRIT_URL = re.compile(
    r'chromium\.googlesource\.com/chromium/src/\+/'
    r'([0-9a-f]{7,40})\b'
)


def strip_ansi(text: str) -> str:
    """Removes terminal colour escapes from build output."""
    return _ANSI_ESCAPE.sub('', text)


@dataclass(frozen=True)
class BuildSummary:
    status: str | None
    failed_steps: list[str]
    errors: list[tuple[int, str]]


def summarize_build(log: str) -> BuildSummary:
    """Extracts the outcome, failed steps and distinct errors from a log."""
    status = None
    failed_steps = []
    counts: dict[str, int] = {}
    for line in strip_ansi(log).splitlines():
        # siso prints progress dots before some messages on the same line.
        line = _PROGRESS_DOTS.sub('', line)
        if match := _STATUS_LINE.search(line):
            status = match.group(0)
        if match := _FAILED_STEP.search(line):
            if match.group(1) not in failed_steps:
                failed_steps.append(match.group(1))
            continue
        for pattern in _ERROR_PATTERNS:
            if match := pattern.match(line):
                error = match.group(1).strip()
                counts[error] = counts.get(error, 0) + 1
                break
    errors = sorted(
        ((n, e) for e, n in counts.items()),
        key=lambda item: (-item[0], item[1]),
    )
    return BuildSummary(status, failed_steps, errors)


def parse_culprit_hashes(message: str) -> list[str]:
    """Returns the hashes in the "Chromium changes:" block of a message.

    `git cr commit --culprit` writes that block, one URL per culprit, ending at
    the first blank line. Links elsewhere in the body are not culprits.
    """
    hashes = []
    in_block = False
    for line in message.splitlines():
        if line.strip() == 'Chromium changes:':
            in_block = True
            continue
        if not in_block:
            continue
        if not line.strip():
            break
        for found in _CULPRIT_URL.findall(line):
            if found not in hashes:
                hashes.append(found)
    return hashes


def _split_range(revision_range: str) -> tuple[str, str]:
    old, sep, new = revision_range.partition('..')
    if not sep or not old or not new:
        raise ValueError(f'Expected OLD..NEW, got {revision_range!r}')
    return old, new


def _is_ancestor(ancestor: str, descendant: str) -> bool:
    try:
        repository.chromium.run_git(
            'merge-base', '--is-ancestor', ancestor, descendant
        )
    except subprocess.CalledProcessError:
        return False
    return True


def culprit_problem(culprit: str, revision_range: str) -> str | None:
    """Explains why `culprit` isn't a valid culprit, or returns None.

    Short hashes are accepted when they resolve, since teammates sometimes pass
    them; a mistyped or invented hash fails to resolve or falls outside the
    range.
    """
    old, new = _split_range(revision_range)
    try:
        full = repository.chromium.run_git(
            'rev-parse', '--verify', '--quiet', f'{culprit}^{{commit}}'
        )
    except subprocess.CalledProcessError:
        return f'{culprit} is not a Chromium commit'
    if not _is_ancestor(full, new) or _is_ancestor(full, old):
        return f'{culprit} is not in {revision_range}'
    return None


def choose_base(milestone: int, master_major: int) -> str:
    """Returns the branch `cr<milestone>` should be rebased onto.

    The previous milestone lives on `cr<milestone - 1>` until it lands on
    master, at which point master's Chromium version moves to that milestone.
    Whether `origin/cr<milestone - 1>` still exists, or is an ancestor of
    master, says nothing: branches outlive their merges, and merges can be
    squashed.
    """
    if master_major >= milestone - 1:
        return 'origin/master'
    return f'origin/cr{milestone - 1}'


def _cmd_prepare(args: argparse.Namespace) -> int:
    brave = repository.brave
    branch = f'cr{args.milestone}'
    if brave.is_rebase_in_progress():
        print('A rebase is in progress; finish or abort it first.')
        return 1
    if not args.force and brave.run_git(
        'status', '--porcelain', '--untracked-files=no'
    ):
        print(
            f'Tracked files have changes, and resetting {branch} would '
            'discard them. Commit or stash them, or pass --force.'
        )
        return 1

    brave.run_git('fetch', 'origin', 'master', branch)
    master_version = Version.from_git('origin/master')
    base = choose_base(args.milestone, master_version.major)
    if base != 'origin/master':
        brave.run_git('fetch', 'origin', f'cr{args.milestone - 1}')

    brave.run_git('checkout', '-B', branch, f'origin/{branch}')
    brave.run_git('branch', f'--set-upstream-to={base}')
    upstream = brave.run_git('rev-parse', '--abbrev-ref', '@{upstream}')
    if upstream != base:
        print(f'Upstream is {upstream}, expected {base}.')
        return 1
    print(
        f'master is on Chromium {master_version}; {branch} reset to '
        f'origin/{branch}, upstream {base}.'
    )

    if args.no_rebase:
        return 0
    return subprocess.run(
        [
            sys.executable,
            str(_BROCKIT),
            'rebase',
            '--squash-minor-bumps',
            '--discard-regen-changes',
        ],
        cwd=brave.root,
        check=False,
    ).returncode


def _cmd_build_summary(args: argparse.Namespace) -> int:
    log = args.log.read_bytes().decode('utf-8', errors='replace')
    summary = summarize_build(log)
    print(f'Status: {summary.status or "unknown (no final status line)"}')
    print(f'Failed steps ({len(summary.failed_steps)}):')
    for step in summary.failed_steps:
        print(f'  {step}')
    print(f'Distinct errors ({len(summary.errors)}):')
    for count, error in summary.errors[: args.limit]:
        print(f'  {count:4d}x {error}')
    if len(summary.errors) > args.limit:
        print(f'  ... {len(summary.errors) - args.limit} more')
    return 0


def _cmd_culprits(args: argparse.Namespace) -> int:
    _split_range(args.range)
    cmd = ['log', '--format=%H %s', args.range]
    if args.symbol:
        cmd.append(f'-S{args.symbol}')
    if args.paths:
        cmd += ['--', *args.paths]
    output = repository.chromium.run_git(*cmd)
    if output:
        print(output)
    return 0


def _cmd_verify_culprits(args: argparse.Namespace) -> int:
    output = repository.brave.run_git(
        'log', '--format=%H%x00%s%x00%B%x01', f'{args.base}..HEAD', no_trim=True
    )
    problems = 0
    for entry in output.split('\x01'):
        if not entry.strip():
            continue
        commit, subject, body = entry.strip('\n').split('\x00', 2)
        for culprit in parse_culprit_hashes(body):
            if problem := culprit_problem(culprit, args.range):
                problems += 1
                print(f'{commit[:12]} {subject}: {problem}')
    print(f'{problems} problem(s) found.')
    return 1 if problems else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    subparsers = parser.add_subparsers(dest='command', required=True)

    prepare = subparsers.add_parser(
        'prepare',
        help='Reset cr<N>, set its base as upstream, and start the rebase.',
    )
    prepare.add_argument('milestone', type=int, help='e.g. 158')
    prepare.add_argument(
        '--no-rebase',
        action='store_true',
        help='Stop before running brockit rebase.',
    )
    prepare.add_argument(
        '--force',
        action='store_true',
        help='Reset even if tracked files have changes (discards them).',
    )
    prepare.set_defaults(func=_cmd_prepare)

    summary = subparsers.add_parser(
        'build-summary', help='Summarise a build log.'
    )
    summary.add_argument('log', type=Path)
    summary.add_argument(
        '--limit',
        type=int,
        default=60,
        help='Maximum number of distinct errors to print.',
    )
    summary.set_defaults(func=_cmd_build_summary)

    culprits = subparsers.add_parser(
        'culprits', help='List upstream commits in a range touching paths.'
    )
    culprits.add_argument('range', help='e.g. 158.0.8117.1..158.0.8127.2')
    culprits.add_argument(
        '--symbol', help='Only commits adding or removing this string.'
    )
    culprits.add_argument(
        'paths', nargs='*', help='Chromium-relative paths, e.g. net/http/x.cc'
    )
    culprits.set_defaults(func=_cmd_culprits)

    verify = subparsers.add_parser(
        'verify-culprits',
        help='Check the culprit hashes cited by the lift commits.',
    )
    verify.add_argument('base', help='e.g. origin/cr157')
    verify.add_argument('range', help='e.g. 158.0.8117.1..158.0.8127.2')
    verify.set_defaults(func=_cmd_verify_culprits)

    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
