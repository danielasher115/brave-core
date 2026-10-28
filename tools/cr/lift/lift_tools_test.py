#!/usr/bin/env vpython3
# Copyright (c) 2026 The Brave Authors. All rights reserved.
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at https://mozilla.org/MPL/2.0/.

import subprocess
import unittest
from unittest import mock

import lift_tools

_FULL_HASH = '5691202e214341a5d96a3296c1b9811cce19572b'


class StripAnsiTest(unittest.TestCase):
    def test_removes_colour_escapes(self):
        self.assertEqual(
            lift_tools.strip_ansi('\x1b[31;1mFAILED:\x1b[0m x'), 'FAILED: x'
        )


class SummarizeBuildTest(unittest.TestCase):
    def test_reports_status_failed_steps_and_deduped_errors(self):
        log = '\n'.join(
            [
                '...\x1b[31;1mFAILED: 82c9 "./obj/a.o" CXX obj/a.o\x1b[0m',
                '../../a.cc:29:49: error: no member named \'CURRENT_TAB\'',
                'FAILED: 92d1 "./obj/b.o" CXX obj/b.o',
                '../../a.cc:29:49: error: no member named \'CURRENT_TAB\'',
                '../../b.cc:3:1: error: unknown type name \'Foo\'',
                ' 14m56.78s Build Failure: 26405 done, 10 failed, 3 remaining',
            ]
        )
        summary = lift_tools.summarize_build(log)
        self.assertEqual(
            summary.status, 'Build Failure: 26405 done, 10 failed, 3 remaining'
        )
        self.assertEqual(
            summary.failed_steps,
            ['"./obj/a.o" CXX obj/a.o', '"./obj/b.o" CXX obj/b.o'],
        )
        self.assertEqual(
            summary.errors,
            [
                (2, '../../a.cc:29:49: error: no member named \'CURRENT_TAB\''),
                (1, '../../b.cc:3:1: error: unknown type name \'Foo\''),
            ],
        )

    def test_recognises_non_compiler_errors(self):
        log = '\n'.join(
            [
                'ERROR at //build/config/BUILDCONFIG.gn:588:5: Dependency not '
                'allowed.',
                '../../Foo.java:39: error: symbol not found Bar',
                'gen/app.ts:6:41 - error TS2307: Cannot find module \'./x.js\'.',
                '  25:65  error  Invalid boolean attribute expression  '
                '@webui-eslint/lit-element-expressions',
                'mold: error: undefined symbol: internal::kHistogram',
                'Type com.android.tools.r8.RecordTag is defined multiple times: '
                'a.jar, b.jar',
                ' "../../x.jar", needed by "obj/y.jar", missing and no known rule '
                'to make it',
                "ModuleNotFoundError: No module named 'utils.Table'",
            ]
        )
        errors = [e for _, e in lift_tools.summarize_build(log).errors]
        self.assertEqual(len(errors), 8)
        self.assertIn(
            'Invalid boolean attribute expression  '
            '@webui-eslint/lit-element-expressions',
            errors,
        )

    def test_success(self):
        summary = lift_tools.summarize_build(
            '.The build has finished successfully.\n'
        )
        self.assertEqual(summary.status, 'The build has finished successfully.')
        self.assertEqual(summary.failed_steps, [])
        self.assertEqual(summary.errors, [])


class ParseCulpritHashesTest(unittest.TestCase):
    def test_extracts_unique_hashes_from_culprit_block(self):
        message = (
            '[cr158] Fix something\n\nBody.\n\nChromium changes:\n'
            f'https://chromium.googlesource.com/chromium/src/+/{_FULL_HASH}\n\n'
            f'commit {_FULL_HASH}\n'
            '    Reviewed-on: https://chromium-review.googlesource.com/c/'
            'chromium/src/+/8620801\n'
            f'https://chromium.googlesource.com/chromium/src/+/{_FULL_HASH}\n'
        )
        self.assertEqual(lift_tools.parse_culprit_hashes(message), [_FULL_HASH])

    def test_ignores_links_outside_the_block(self):
        message = (
            'Subject\n\nSee [3c06ac856fbd0](https://chromium.googlesource.com/'
            'chromium/src/+/3c06ac856fbd0).\n\nChromium changes:\n'
            'https://chromium.googlesource.com/chromium/src/+/a1fc21843e8bb\n'
            'https://chromium.googlesource.com/chromium/src/+/59ee62c696c68\n'
            '\ncommit a1fc21843e8bb3bc0db2fbe4d338e304010bc37b\n'
        )
        self.assertEqual(
            lift_tools.parse_culprit_hashes(message),
            ['a1fc21843e8bb', '59ee62c696c68'],
        )

    def test_no_culprit(self):
        self.assertEqual(lift_tools.parse_culprit_hashes('[cr158] Subject'), [])


class CulpritProblemTest(unittest.TestCase):
    def test_rejects_hash_that_does_not_resolve(self):
        with mock.patch.object(lift_tools.repository, 'chromium') as chromium:
            chromium.run_git.side_effect = subprocess.CalledProcessError(
                1, 'git'
            )
            self.assertEqual(
                lift_tools.culprit_problem(
                    _FULL_HASH, '158.0.8117.1..158.0.8127.2'
                ),
                f'{_FULL_HASH} is not a Chromium commit',
            )

    def test_rejects_commit_outside_range(self):
        with mock.patch.object(lift_tools.repository, 'chromium') as chromium:
            # rev-parse succeeds, then the commit is already in the old tag.
            chromium.run_git.side_effect = [_FULL_HASH, '', '']
            self.assertEqual(
                lift_tools.culprit_problem(
                    '5691202e', '158.0.8117.1..158.0.8127.2'
                ),
                '5691202e is not in 158.0.8117.1..158.0.8127.2',
            )

    def test_accepts_commit_in_range(self):
        with mock.patch.object(lift_tools.repository, 'chromium') as chromium:
            chromium.run_git.side_effect = [
                _FULL_HASH,
                '',
                subprocess.CalledProcessError(1, 'git'),
            ]
            self.assertIsNone(
                lift_tools.culprit_problem(
                    _FULL_HASH, '158.0.8117.1..158.0.8127.2'
                )
            )

    def test_rejects_malformed_range(self):
        with self.assertRaises(ValueError):
            lift_tools.culprit_problem(_FULL_HASH, '158.0.8127.2')


if __name__ == '__main__':
    unittest.main()
