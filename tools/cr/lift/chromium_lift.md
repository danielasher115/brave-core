# Autonomous Chromium lift

Instructions for an agent that lifts a `cr<milestone>` branch to the latest
Chromium version for that milestone, without a human to ask. The run:

1. resets the local branch to `origin/cr<milestone>`;
2. rebases it onto the previous milestone's branch;
3. lifts it with 🚀Brockit to the latest tag for the branch;
4. builds Linux and fixes what breaks;
5. builds Android and fixes what breaks.

It ends with the result committed on the local branch. Don't push, open PRs or
comment on GitHub: the CI job that launched the run decides what to do with the
branch. Keep a running report (see [Report](#report)) and finish by printing it.

Commands below run from `src/brave` unless they say otherwise. `..` is `src/`,
the Chromium checkout, where the upstream history lives.

## Inputs

- **Milestone `N`** and the branch, `cr<N>`.
- **Base:** `origin/cr<N-1>` while the previous milestone is still on its own
  branch, and `origin/master` once it has landed there. It is set as the
  branch's upstream (see [step 1](#1-reset-and-rebase)).
- **Bump range:** printed by `brockit lift` as "Changes for this bump"
  (`158.0.8117.1..158.0.8127.2`). Culprits for the lift's own breakage are in
  this range.
- **Branch range:** "Changes since base version"
  (`157.0.8103.15..158.0.8127.2`). It covers every bump already on the branch;
  use it to verify culprit hashes and to look for earlier fixes.
- **Tracker:** `tools/cr/M<N>_codehealth.md`, if it exists. Read it before
  starting: issues it already tracks are left alone (see
  [Ground rules](#ground-rules)).

## Ground rules

These apply to every step. Several came from mistakes that were caught in
review; the reason is given so that edge cases can be judged.

### Scope

- Act only on Brockit sections marked **(action needed)**. Ignore its other
  warnings (e.g. "Patch being flagged as broken, but with unexpected reason",
  "pnpm run init is failing to reset some paths").
- Before fixing a non-trivial runtime or test failure, check
  `M<N>_codehealth.md`. If the issue is already tracked there, leave it and say
  so in the report (cr158: the settings `pref=` → `pref-key` toggle migration).
- Only run tests relevant to the code a fix touches. Full suites are other CI
  jobs' business.
- Don't build while Brockit has open "action needed" items. The tree is
  half-patched and the build only fails at `gn gen`.

### Decisions without a human

When a fix needs a judgement call that would normally go to a reviewer (several
viable designs, a behaviour change, new third-party deps, dropping a Brave
feature), pick the most conservative option that keeps the build going, commit
it with `--tagged=WIP`, and explain the concern and the alternatives in the
commit body and in the report. Then carry on. Examples from cr158:

- Upstream dropped jars Brave's Android icon rasterizer needs from its CIPD
  bundle. Pulling the per-library CIPD packages from `brave/DEPS` keeps the
  rasterizer working without vendoring binaries. That is a new dependency
  source, so it deserves a reviewer's look.
- Migrating 17 settings toggles off a removed binding had three viable designs.
  It was already tracked in the codehealth file, so it was left alone.

### Commits

- Commit with `git cr commit`, never plain `git commit` (except the mangler
  snapshot, below). Pass `--culprit <hash>` whenever there is an upstream
  culprit. The hook writes the "Chromium changes:" block and the `[cr<N>]`
  prefix; don't hand-write either.
- Pass the culprit as a full 40-character hash obtained from
  `git -C .. rev-parse <short>` or copied verbatim from tool output. Never type
  or complete a hash from memory: an invented ending once nearly got committed.
  Confirm that it is in range:
  `git -C .. merge-base --is-ancestor <hash> <new-tag>` succeeds and
  `git -C .. merge-base --is-ancestor <hash> <old-tag>` fails.
- One commit per culprit. If a commit for that culprit already exists on the
  branch, add to it with `git cr commit --fixup=<sha>`, or `--fixup=amend:<sha>`
  when its message must change too (for an `amend!` message, keep the original
  subject and the "Chromium changes:" block, or the squash loses them). Fixups
  are squashed by the next `brockit rebase`.
- Platform-centric fixes get the platform tag: `--tagged=Android` for Android,
  `--tagged=iOS` for iOS (cr158: the iOS key pinning includes). Fixes that
  aren't ready get `--tagged=WIP`. Combine as `--tagged=Android,WIP`.
- A fix without an upstream culprit (a latent Brave bug that only now shows up)
  is committed without `--culprit`; say why in the body.
- Messages follow `../agents/shared/skills/cl-description/cl-description.md`: a
  `[Component]`-style subject under 72 columns, a body wrapped at 72 that
  explains why. Wrap with
  `vpython3 ../agents/shared/skills/cl-description/scripts/wrap_lines.py <file>`
  and pass the file with `-F`. Leave out the `Bug:` line. Never add
  `Co-Authored-By` or any other trailer: on a fixup, a body becomes the squashed
  message, and the hook appends after it.
- Don't claim in a message what wasn't verified (cr158: "at the version upstream
  last shipped", when only the latest tag had been checked).
- Codehealth tracker changes, including Tooling TODOs, are committed together
  with any `tools/cr/codehealth_review.md` change, with the message
  `Version CodeHealth tracker`.
- Changes to this file and the other `tools/cr/lift/` files are committed with
  `Version lift updates` (see [Committing it](#committing-it)).

### Editing upstream code

- Upstream files (anything in `src/` outside `src/brave`) change only through
  plaster or a patch. Never run a formatter over them.
- Change a plastered file by editing its `rewrite/**.yaml` and running
  `vpython3 tools/cr/plaster.py apply <yaml>` (or `plaster apply <yaml>`).
  Always pass the yaml; a bare `plaster apply` rewrites every patch. Don't run
  `plaster check` (it is a dry run of `apply`); check the regenerated `.patch`
  and `git diff` instead.
- Regenerate a raw patch (no yaml) with `pnpm run update_patches <path>`, scoped
  to that one file. Never run it unscoped during a lift: it rewrites hundreds of
  patches. When the upstream file ends up identical to upstream,
  `update_patches` leaves the stale patch alone; `git rm` it.
- Ignore `.patchinfo` files. They are gitignored metadata: don't inspect, edit
  or delete them.
- Plaster vs `chromium_src`: file-scope additions (includes, helpers, constants,
  types) belong in a `chromium_src` shadow, which declares them and then
  includes the upstream file. Plaster is for edits inside function bodies,
  switches, initializers and enums. Prefer a precise plaster over a
  `#define X X_ChromiumImpl` token rewrite, which silently rewrites every later
  occurrence. See `docs/plaster.md` and `docs/plaster_dos_and_donts.md`.
- When a fix touches a `chromium_src` file that uses a `#define` to rewrite the
  upstream file it includes, consider moving that `#define` to plaster:

  - If the move is simple (a token rename or a hook at one site), do it as part
    of the fix.
  - If keeping it separate makes the fix easier to review, make the migration a
    follow-up commit right after the fix.
  - Commit a migration that stands on its own with
    `git cr commit --tagged=plaster` (plus the culprit, if the migration is what
    fixes it).

  (cr158: the `TabContainerImpl` → `BraveTabContainer` `#define` in the
  `tab_strip.cc` shadow and the `ShowIfNotOffTheRecordProfile` `#define` in the
  `infobar_utils.cc` shadow both became plaster substitutions at their call
  sites.)

- Plaster `description`s read like a commit message: a subject saying what, a
  blank line, then why. Keep `count: 1` unless a different count is deliberate
  and commented.
- Keep `set_feature_flag_default_state` / `BASE_OVERRIDDEN_FEATURE` pins even
  when upstream's default now matches. Remove one only if upstream deleted the
  feature, together with its entry in `app/feature_defaults_unittest.cc`.

### Writing Brave code

- If upstream code builds in Chromium but not in Brave, find the Brave
  discrepancy instead of plastering upstream. The usual leak is
  `-iquote //brave/chromium_src`: every quoted include of an upstream header
  resolves to Brave's shadow, so a Brave header can reach upstream translation
  units several includes deep. For name-lookup errors, look for a Brave nested
  namespace that shadows a global one. The build environment is another source
  (cr158: `PYTHONPATH`, below).
- `auto` only where the Google style guide allows it: the type is obvious
  (`make_unique`, casts, iterators) or doesn't matter.
- Don't add null checks for values that can't be null at the call site, even if
  existing Brave or upstream code does. Use `CHECK()` for a real invariant.
- GN deps go under the same guard as the sources that need them. Don't add a
  platform guard around a dep the target uses unconditionally.
- Run `pnpm run format --base=HEAD` after editing Brave files. It formats only
  the changed lines of Brave files. Revert anything it reformats outside the
  change, such as unrelated tracker files.

### Machine state

- Never delete caches, out directories or anything outside the repo working tree
  (`~/.cache`, the siso cache, `out/`). If one looks corrupt, record it in the
  report and stop.
- Run one build per checkout at a time. Before starting one, check that no
  earlier build is still running there:
  `pgrep -af "siso|autoninja" | grep "$(realpath ..)"`. A build from another
  checkout is harmless.
- `brave/build/config/siso/brave_siso_config.star` is hardlinked into
  `src/build/config/siso/`. Editing tools that write atomically break the link,
  and the build then reads a stale copy. After editing it, run
  `python3 build/hardlink_file.py build/config/siso/brave_siso_config.star ../build/config/siso/brave_siso_config.star`
  and check that both paths have the same inode (`stat -c '%i %n'`).

## Tooling

Rely on tools over inference wherever one exists or can be written: 🚀Brockit,
🩹Plaster, `git cr` (`commit`, `follow-renames`), the `pnpm run` commands,
`tools/recipes`, and `lift_tools.py` below. A tool's result is reproducible and
reviewable; a judgement call has to be re-made on every run.

When a step needs work that a tool could do, or a tool gets something wrong or
says too little, file a request under `## Tooling TODOs` at the end of
`tools/cr/M<N>_codehealth.md` (create the section if it's missing). Write one
`###` item per tool problem: what the tool does now, the cr<N> example that
showed it, and checkboxes for the changes requested. Extend an existing item
rather than duplicating it. These requests are reviewed separately, so don't
implement missing features or improvements during a lift, and don't let one
block the run. Commit them with `git commit -m 'Version CodeHealth tracker'`,
like other tracker changes.

A hard bug is different: a tool that crashes, corrupts the tree or commits, or
produces a wrong result the lift can't work around. Fix it in the tool, with a
test in its `tools/cr/*_test.py` and the tool's tests passing. Commit the fix on
its own with `git cr commit --tagged=<tool>` (e.g. `--tagged=brockit` for
`brockit.py` or `rebase.py`), never mixed into a lift fix, and list it in the
report. If the fix isn't small and safe, work around the bug, file it as a
Tooling TODO, and carry on.

`tools/cr/lift/lift_tools.py` collects the checks done repeatedly by hand:

```sh
# Outcome, failed steps and distinct errors of a build log.
vpython3 tools/cr/lift/lift_tools.py build-summary /tmp/lift/build.log

# Upstream commits in the bump range touching paths, or adding/removing a string.
vpython3 tools/cr/lift/lift_tools.py culprits 158.0.8117.1..158.0.8127.2 net/http/foo.cc
vpython3 tools/cr/lift/lift_tools.py culprits 158.0.8117.1..158.0.8127.2 --symbol kFooFeature

# Check every culprit cited since the base: it resolves, and it's in range.
vpython3 tools/cr/lift/lift_tools.py verify-culprits origin/cr157 157.0.8103.15..158.0.8127.2
```

Logs go under a scratch directory such as `/tmp/lift/`, never in the repo.
`grep` may be aliased to `ugrep`, which skips binary files and handles
`--include` differently; use Python for binary inspection.

## Procedure

### 1. Reset and rebase

First pick the base. Once the previous milestone has landed, master's Chromium
version is on milestone `N-1` (or later), and the base is `origin/master`;
before that, master is still on `N-2` and the base is `origin/cr<N-1>`.

```sh
git fetch origin master cr<N>
brockit show --package-version origin/master  # prints master's Chromium tag
```

Don't decide from whether `origin/cr<N-1>` still exists or is an ancestor of
master: the branch can outlive its merge, and a squashed merge isn't an
ancestor. With the base chosen (`BASE` below is `origin/master` or
`origin/cr<N-1>`):

```sh
git fetch origin cr<N-1>  # only when BASE is origin/cr<N-1>
git checkout cr<N>
git reset --hard origin/cr<N>
git branch --set-upstream-to=$BASE
git rev-parse --abbrev-ref @{upstream}  # must print $BASE
brockit rebase --squash-minor-bumps --discard-regen-changes
```

Brockit takes the branch's upstream as the base to rebase onto, so set it before
rebasing. A fresh checkout of `cr<N>` tracks `origin/cr<N>`, and rebasing onto
that does nothing useful. The upstream also stays the base for `brockit lift`
(`--from-ref` defaults to `@upstream`). Use the same base wherever this document
says `origin/cr<N-1>`, such as `verify-culprits`.

The rebase also autosquashes `fixup!` and `amend!` commits. On each conflict,
resolve it, `git add` the files and run `git rebase --continue` (with
`GIT_EDITOR=true`), until it reports "Successfully rebased". Recipes:

- **Generated `patches/*.patch`:** `plaster apply patches/<name>.patch`
  regenerates it from its yaml. Don't hand-merge patch hunks. If the yaml
  conflicts too, resolve the yaml first: usually the newer design on the base
  branch supersedes the branch's version (cr158: the `.onion` CORS fix in cr157
  covered the preflight case the cr158 commit added). Check that it still
  applies to the new source and covers what the branch's version did, then
  regenerate the patch. The same patch may conflict again in a later "Update
  patches" commit; regenerate it again.
- **`tools/cr/M<N>_codehealth.md`:**
  `git checkout origin/cr<N> tools/cr/M<N>_codehealth.md`.
- **Lit mangler snapshot** (`tools/chromium_src/lit_mangler/__snapshots__/`):
  resolve and add. It is regenerated at the end.
- **Test filters and other files:** resolve on the merits. When both sides
  filter the same test, keep the broader filter.

A commit whose changes are already on the base branch is dropped automatically
("patch contents already upstream").

### 2. Lift

```sh
brockit lift --with-github --to=@latest-for-branch
```

It updates Chromium, reapplies patches with `--3way`, and stops with "action
needed" sections. Note the bump range it prints. Handle each section, then run
`brockit lift --with-github --to=@latest-for-branch --continue`.

For each item, find the culprit first
(`lift_tools.py culprits <bump range> <path>`), then fix:

- **Files that cannot be patched anymore: renamed or moved.** Run
  `git cr follow-renames <full culprit hash>`. It moves patches and yamls and
  updates C++ includes and GN labels, but not Java imports, nor namespaces or GN
  target labels that changed with the move. Grep for the old package, path and
  symbol names in Java, C++, GN, DEPS, `test/filters` and
  `app/feature_defaults_unittest.cc`, and fix what remains. Its formatter also
  reflows unrelated tracker files; revert those. Check that the patch applied to
  the moved upstream file.
- **Files that cannot be patched anymore: deleted.** Find where the code went.
  When a class is folded into its base (cr158: `TabContainerImpl` into
  `TabContainer`), the Brave subclass rebases onto the new class. Every method
  the subclass overrides that is no longer virtual needs `make_virtual`, plus
  `add_friend` for the subclass. A `#define Old Brave` that swapped in the
  subclass now matches nothing, and the build gives no error. Replace it with a
  plaster on the factory (`std::make_unique<TabContainer>(` →
  `std::make_unique<BraveTabContainer>(`). Delete the old patches and yamls.
- **Plaster failed to fix patches.** Read what the substitution targets and what
  the culprit did to it. In order of likelihood:
  - the target moved: move the yaml (cr158: `kTtc` to `components/ttc`);
  - upstream did the same thing: delete the yaml and its patch (cr158:
    `ENABLE_DESKTOP_ANDROID_EXTENSIONS` became `IS_ANDROID`);
  - the feature is gone: drop that substitution and its
    `feature_defaults_unittest.cc` entry;
  - the code changed shape: rewrite the substitution.
- **Manually resolve conflicts for.** Open the upstream file in `src/`.
  - _Minor_ (context drift, the Brave hook still has a home): resolve the
    markers and stop. `--continue` refreshes the patch, so don't regenerate or
    commit it.
  - _Non-trivial_ (the code the hook targeted is gone or moved): resolve it,
    rebuild the customisation on the new code, regenerate or delete the patch
    with `pnpm run update_patches <path>`, and commit with the culprit. cr158:
    static key pinning moved from `TransportSecurityState` into
    `KeyPinningStateChrome`. The constructor hook was replaced by a plaster on
    `kDefaultEnableStaticPins`, and the same culprit turned key pinning off on
    iOS, which Brave needs, so `chrome_key_pinning_supported = true` went into
    `build/args/ios_defaults.gni`. Read the whole culprit, not just the
    conflicting hunk.

After `--continue`:

- "❌ Plaster check. Please investigate it." Run `plaster apply --all` (the one
  sanctioned unscoped use) and inspect the diff. Nothing changed: carry on
  (stale `.patchinfo` metadata is a likely cause). Unremarkable, sensible
  changes: commit as `--fixup` to the "Apply-fixed 🩹 patches from ..." commit.
  Significant changes: their own commit with a culprit, `--tagged=WIP` if
  unsure.

### 3. Linux build

```sh
pnpm run build --ignore_compile_failure --target=brave:all \
    --gn=enable_updater:true --gn=use_prebuilt_omaha4:false \
    > /tmp/lift/build-linux.log 2>&1
```

Use exactly this command; never raw `autoninja`, `ninja` or `siso`. The out dir
is `../out/Component`. `--ignore_compile_failure` keeps going after errors, so
one round shows many failures. Loop:

1. `lift_tools.py build-summary /tmp/lift/build-linux.log`.
2. Group the errors by root cause, not by file. One header change can fail 60
   objects.
3. For each group, find the culprit and the fix (see
   [Fix patterns](#fix-patterns)). Independent groups can be researched in
   parallel, by subagents that only research. Edits and commits stay in the main
   agent so they don't race.
4. Commit each fix with its culprit, re-run `pnpm run format --base=HEAD` on
   Brave files, and rebuild.

"cannot make progress due to previous N errors" with many steps remaining is
normal: those steps depend on the failures. If a log ends with an error about
`out/Component/build.guard`, two builds overlapped. Make sure only one runs.

### 4. Linux tests

Once the build is green, run the tests that cover each fix, and add coverage
where a runtime fix has none.

```sh
python3 tools/recipes/runtest.py --build-dir ../out/Component --xvfb \
    <binary> --no-sandbox --gtest_filter='<Suite>.*'
```

The binary is a name relative to `--build-dir` (`brave_unit_tests`,
`brave_browser_tests`, `brave_cert_pinning_tests`, upstream `unit_tests`). Check
that the filter matched something: "Tests took 0 seconds" or `[0/0]` means it
didn't (fixture names are case-sensitive: `SpeedReaderBrowserTest`, not
`SpeedreaderBrowserTest`).

Finding coverage:

- Search Brave test files for the class or function a fix touches. Feature
  default pins are covered by `FeatureDefaultsTest.*` in `brave_unit_tests`.
- Brave tests of overridden upstream code live at the matching
  `chromium_src/.../*_unittest.cc` path and are listed in `test/BUILD.gn`'s
  `brave_unit_tests` sources. A `chromium_src` shadow of an upstream unittest
  can include it and reuse its fixtures and helpers.
- A new test must fail without the fix. Assert behaviour, not a value you just
  set. In cr158, the first test for the reader mode toolbar asserted the
  accelerator mode reported for the web contents. It failed, and that exposed
  that the fix itself was wrong (`SetAcceleratorHandling()` only applies when
  the `WebView` is the contents' delegate). The final test checks that a browser
  accelerator isn't held back for the renderer.
- Some code only builds in official builds (e.g. `enable_crash_dialog`). Its
  tests can't run in a Component build: write them, and list them as unverified
  in the report.
- Commit tests as a `--fixup` to their fix's commit.

Known environment-only failures, not lift regressions:

- `BraveFontWhitelistTest.*` fails on Linux distros that `GetLinuxDistro()`
  doesn't recognise (e.g. Ubuntu 26.04).
- Tests that pass on retry are flaky; report them, don't fix.
- An upstream test that checks behaviour Brave disables may already be in
  `test/filters/*.filter`. Check before investigating.

### 5. Lit mangler snapshot

```sh
pnpm run test-unit -t "mangled files should have up to date snapshots" -u
git add tools/chromium_src/lit_mangler/__snapshots__/mangle.test.ts.snap
git commit -m 'Update Lit mangler snapshot.'
```

Plain `git commit`, no culprit. The hook adds the prefix. Don't chase culprits
for individual stale snapshots. Run it again at the end if later fixes touched
manglers.

### 6. Android

```sh
pnpm run init --target_os=android
pnpm run build --target_os=android --target_arch=x64 \
    --target_android_output_format=apk --target=brave:all \
    > /tmp/lift/build-android.log 2>&1
```

Use `x64`, so that tests can run on an emulator. The out dir is
`../out/android_Static`. This command has no `--ignore_compile_failure`, so it
stops at the first error: expect one round per failure. Re-run
`pnpm run init --target_os=android` after changing `brave/DEPS`. It also checks
that the new deps sync. All Android fixes get `--tagged=Android`.

For tests, run only the Android tests for code touched by Android fixes. They
need an emulator: `tools/android/avd/avd.py install` and `start` with an x64
config such as `tools/android/avd/proto/android_34_google_apis_x64.textpb`. That
downloads several GB and needs `/dev/kvm`. If no device can be had, list the
Android tests as unverified in the report.

### 7. Update this document

Before finishing, fold what the run taught into this file, following
[Maintaining this document](#maintaining-this-document), and commit it.

### 8. Wrap up

- `lift_tools.py verify-culprits <base> <branch range>` reports no problems.
- `git status` is clean. Untracked files are scratch you created, or deps
  fetched into git-ignored paths.
- No build is left running.
- Print the report.

## Fix patterns

Each of these came up in the cr158 lift. Find the culprit first: the pattern
tells you where to look, the culprit tells you what upstream intended.

### Moves and renames

- **Java package moves** (`RecentlyClosedEntriesManager` to `recent_tabs`):
  `follow-renames` doesn't update Java imports. Fix them by hand in `android/`,
  including `junit` tests, then run `pnpm run format --base=HEAD`.
- **Namespace moves** (`kBrowserActuatorProtoStreamTransport` to
  `browser_actuator::transport`): update the qualified name, the include, the GN
  dep and the `DEPS` include rule.
- **Enum renames** (`WindowOpenDisposition::CURRENT_TAB` → `kCurrentTab`, no
  aliases): one mechanical rename across Brave in a single commit. Map every
  value, update comments that name the old values, then format. Check Java and
  TS separately (mojom Java bindings kept the old names).
- **Deleted third-party targets** (`//third_party/kotlin_stdlib`): use the label
  upstream's own build files now use
  (`//third_party/android_deps:kotlin_stdlib_java`).

### Signature and API changes

- **Brave overrides of a changed signature.** When a Brave `#define` declares an
  `_Unused` duplicate of an upstream method (`UpdateKeyPinsList`), update both
  declarations. If the new signature has a reply callback, run it, or callers
  waiting on the reply hang.
- **Removed setters.** Read how upstream migrated its own callers, and check
  that the replacement applies in Brave's situation. `SetAcceleratorHandling()`
  replaced `set_allow_accelerators(true)` only for a `WebView` that is its
  contents' delegate; Brave's reader mode toolbar isn't, so it needed
  `set_allow_accelerators_deprecated(true)`, as upstream UI hosts in the same
  position use. A test caught this.
- **Classes replaced by free functions** (`SessionCrashedBubble` →
  `ShowSessionCrashedBubbleIfNotOffTheRecordProfile()`): a Brave `#define` on
  the old method name compiles nothing and silently drops Brave's hook (here,
  the crash report permission dialog). Re-route with a plaster at the call site,
  and declare the Brave wrapper in the `chromium_src` header.
- **Class moved out of a namespace** (`ChromeSyncablePrefsDatabase` out of
  `browser_sync`): move Brave's definitions out too. Check that everything
  inside was fully qualified.
- **Brave appends to an upstream enum after `kMaxValue`** (`DownloadCommands`):
  when upstream adds a value, renumber Brave's values past it and update the
  `static_assert`. Check first that Brave's values aren't persisted or recorded
  in histograms.
- **Brave stubs out a whole upstream `.cc`** (`prerender_manager.cc`): new
  `extern` declarations in the header need definitions in the stub, or tests
  fail to link.

### Lost transitive includes

Upstream forward-declares something (`base::OneShotTimer`, `GURL`) and a Brave
file that relied on the old include chain breaks. Add the include to the Brave
file; don't plaster upstream. To name the culprit:

1. Preprocess the failing translation unit to list what it includes now. Take
   its compile command from the log (the `clang++ ... -c x.cc -o x.o` line),
   drop `-MMD -MF <file>`, and replace `-c <src> -o <obj>` with
   `-E -H <src> -o /dev/null`. Run it from the out dir: `-H` writes the include
   tree to stderr. This is a preprocessor run, not a build.
2. List the headers the range removed the include from:
   `git -C .. log <bump range> -G'#include "url/gurl.h"' -- '*.h'`. For each
   commit, see which headers lost the include.
3. The culprit is the commit whose header is in the include list.

### Build system and environment

- **New upstream deps in a test Brave compiles** (`gn check`: "Can't include
  this header from here" in an upstream test listed in Brave's
  `android/android_browser_tests.gni`): add the same deps upstream added in the
  culprit.
- **GN visibility**: a template now depends on a target with restricted
  visibility (crubit `cpp_api_from_rust` bindings on their crate). Add the
  dependent to the `visibility` list.
- **Brave-only environment leaks**: an upstream action that works in Chromium
  but fails in Brave can be Brave's `pnpm run build` environment. cr158:
  `build/commands/lib/config.ts` put `depot_tools` on `PYTHONPATH`, and its
  `utils.py` shadowed SPIRV-Tools' `utils` package. Confirm by running the
  action's import with and without the variable. Fix at the source: dropping the
  entry and making its last user add the path itself.
- **Upstream drops a library Brave uses** (Android-tools jars in the
  `android_deps` CIPD bundle): Brave's `DEPS` can declare per-library CIPD
  packages (`"dep_type": "cipd"`, `"condition": "checkout_android"`), fetched
  into a git-ignored directory. Look for them with
  `cipd ls chromium/third_party/android_deps/libs` and
  `cipd describe <package> -version latest`. Check the package layout with
  `cipd export` into a scratch directory. A new dependency source is a
  `--tagged=WIP` decision.
- **Stale Brave workarounds**: when an upstream change fixes what a Brave
  workaround worked around, the workaround can become the bug. cr158: a `dex.py`
  plaster kept the WireGuard prebuilt out of intermediate dexing because D8
  rejected its Java record there. Upstream then made intermediate dexing drop
  global synthetics and add them back in the final dex. The plaster then made
  WireGuard ship its own `RecordTag`, which duplicated upstream's. Look for a
  Brave plaster or patch on the failing tool before suspecting upstream.
  Reproduce by replaying the action's exact command (from the
  `rule ... command =` line in the out dir's `.ninja` files), with its outputs
  redirected to a scratch directory.
- **Stale outputs vs. real bugs**: before blaming a cache, compare the output
  with what the action produces when replayed. In cr158, the WireGuard dex was
  correct for the command actually run; the command was wrong.

### WebUI

- **Shared files moved** (`cr_elements/web_ui_listener_mixin.js` to
  `/shared/settings/`): use the import path upstream's own code now uses, and
  look for side-effect imports (`md_select.css.js`) that type-checking won't
  flag.
- **Hand-written browser proxies replaced by generated ones**
  (`CustomizeChromeApiProxy` → `browserProxyFactory` from `*.mojom-webui.js`):
  update overrides and the JS strings inside browser tests, which no build step
  checks.
- **New lint rules** (`@webui-eslint/lit-element-expressions`): fix every Brave
  template, including strings that lit_manglers inject into upstream templates.
  Boolean attributes set by a mangler use `''`, not `'true'`.
- **Brave replacements of upstream elements** (Brave's `cr-button`): declare
  properties upstream code now binds (`noink`) even if Brave's element ignores
  them.

### Android 32-bit

`std::ranges::count_if` returns `ptrdiff_t`, which is `int` on 32-bit ARM, so
comparisons with unsigned literals trip `-Wsign-compare`. Compare with signed
literals. This was a latent Brave bug with no upstream culprit.

## Report

Keep it as you go; print it at the end. Include:

- The bump range, and the branch range.
- Each commit made, as `<sha> <subject> (culprit <short hash> | no culprit)`,
  with fixups listed under their target.
- Each `--tagged=WIP` commit: the concern and the alternatives.
- Issues found but left alone because the codehealth tracker covers them, and
  any test failures attributed to the environment or to flakiness.
- Tooling TODOs filed or extended in `M<N>_codehealth.md`.
- Tool bugs fixed during the run, with their commits.
- What wasn't verified: platforms not built (iOS, Windows, macOS), tests that
  couldn't run (official-build-only code, Android without an emulator), and
  fixes verified only statically.

## Maintaining this document

This file should grow from evidence and shrink when advice stops paying off.
Update it at the end of every run, and as soon as something in it proves wrong
mid-run: a later step shouldn't repeat the mistake.

### Inputs

- **Corrections.** Places where following this file led to a wrong fix, a wasted
  round, or a commit that had to be amended (cr158: setting the upstream before
  `brockit rebase` was missing, so a fresh checkout would rebase onto itself).
- **New failure mechanisms.** Breakage whose cause and fix
  [Fix patterns](#fix-patterns) doesn't describe.
- **Repeated manual work.** A check run by hand in more than one round, or in
  two runs. If it belongs in `lift_tools.py`, add it there; if it belongs in
  Brockit, Plaster, `git cr` or another tool, file it under Tooling TODOs (see
  [Tooling](#tooling)).
- **Misses.** Problems in a range a run already lifted that surfaced later,
  through a later rebase, CI or a bug report. Look in `src/brave` history for
  fixes whose "Chromium changes:" link names an upstream commit from an
  already-lifted range, and add what would have caught each one.

### Changing it

- Add only what a run needed, or what would have caught a miss.
- Put a rule under the ground rule or procedure step it belongs to, and a fix
  under the [Fix patterns](#fix-patterns) heading for its mechanism. Write one
  bullet per mechanism: the signal, how to check it, and one concrete example in
  parentheses.
- Tag new examples with their milestone, e.g. `(cr159: …)`. Untagged examples
  date from cr158.
- Keep it range-independent. Lists of files, commits and counts go in the
  report, not here.
- If a bullet for the same mechanism exists, refresh its example or sharpen its
  check rather than adding a near-duplicate.
- Remove advice when its mechanism is gone, or when tooling now handles it (a
  Brockit step, a presubmit check, a `lift_tools.py` subcommand). Leave a
  one-line pointer if the tool's name helps.
- When a check is mechanical and has been done by hand in two runs, add it to
  `lift_tools.py`, with tests in `lift_tools_test.py`, and shorten the bullet to
  point at it.
- Prefer replacing text to appending it, so the file stays readable in one pass.

### Committing it

- Commit changes to this file, to the `tools/cr/lift/` helpers and to their
  `tools/cr/README.md` entry with `git commit -m 'Version lift updates'`,
  separately from the lift's fixes and from the tracker, so they can be audited.
  Plain `git commit`: no culprit, no tags, no trailers. The subject is pinned in
  `tools/cr/rebase.py`, so `brockit rebase` squashes repeated updates into one
  commit.
- Tooling TODOs are tracker content: commit them with
  `git commit -m 'Version CodeHealth tracker'`.
