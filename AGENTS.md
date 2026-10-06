# AGENTS.md: checklist for agents working on Husk

Read this before changing anything. It holds the repo's rules, the traps earlier sessions fell into, and what is still open. Paths are relative to the repo root. Keep §6 up to date when you fix or find something.

## 1. Orient (10 minutes)

- [ ] **Read the docs:** [`README.md`](README.md), then [`docs/00-architecture.md`](docs/00-architecture.md), [`docs/02-jit-substrate.md`](docs/02-jit-substrate.md), [`docs/04-translation-layer.md`](docs/04-translation-layer.md), [`docs/06-built-in-jit.md`](docs/06-built-in-jit.md) and [`docs/01-licensing.md`](docs/01-licensing.md).
- [ ] **Know the target:** the fork targets an **iPhone 14 (iPhone14,7, 6 GB, A15) on iOS 26.2**, which is TXM-enforced. Executable memory comes only from an attached debugger (StikDebug or Built-in StikJIT). TrollStore and on-device pairing (iOS 27) don't apply.
- [ ] **Know the open PR stack**, merged in order. None of it has been built for iOS or run on a device yet:
  - `main` → #1 `claude/fix-critical-jit-snapshot` → #2 `claude/fix-tl-top-issues` → #3 `claude/fix-high-issues` → #4 `claude/fix-remaining-bugs`.
  - Base new work on the newest branch that isn't merged yet, not on `main`, unless the change is independent.
- [ ] **Run `git log --oneline -5`** and confirm you are on the base you think you are.

## 2. Workflow rules

- [ ] **Worktrees:**
  - Create them from the intended base explicitly: `git worktree add -b <branch> .claude/worktrees/<name> <base-branch>`.
  - Automatic worktree isolation bases on `main` and silently loses the PR stack.
  - Never reset or modify another agent's worktree or the main checkout.
- [ ] **Never push to `main`.** Use one branch per task and stack its PR on the right base.
- [ ] **One commit per logical change.** End commit messages with the session's attribution trailers when your harness provides them.
- [ ] **Stay out of other agents' files.** When several agents run in parallel, give each a disjoint file set. Two agents once fixed the same `run.sh` step independently and the merge produced duplicate symbols.
- [ ] **Don't post on GitHub** unless asked. PR bodies carry the device checklist.

## 3. What you can and cannot verify

| Can verify on Linux | Cannot verify on Linux |
|---|---|
| `sh tests/translation-layer/run.sh` (6 groups, 114 checks on #4; needs clang, lld, llvm, python3, zlib headers, `gcc-aarch64-linux-gnu`, `qemu-user`) | Any Swift: no `swiftc` |
| `cc -std=gnu11 -fsyntax-only -Wall -Wextra -Werror -Isrc/translation-layer -Isrc/translation-layer-next src/translation-layer-next/husk-tl-ld.c` | Darwin-only C (Mach, CoreFoundation, Security, `pthread_*_np`); syntax-check against stand-in headers and diff the diagnostics against the base |
| Generated arm64 instruction bytes under `qemu-aarch64(-static)`; encodings with `llvm-mc -show-encoding` | Anything involving the debugger, TXM, Keychain, Metal/ANGLE or the real device |
| Pure logic copied into throwaway ASan/UBSan tests | Whether a game actually runs |

- [ ] **Say plainly in your report and PR what was not compiled or run.** Never claim device behaviour you didn't observe.
- [ ] **Put a device checklist in every PR** that touches Swift or Darwin C.

## 4. Licensing rules (hard constraints)

- [ ] **Licence headers:** Husk's own files are **GPL-2.0-or-later** with an SPDX header (`/* SPDX-License-Identifier: GPL-2.0-or-later */` in C, `// …` in Swift).
- [ ] **Never copy Apache-2.0 or GPL-3.0 code into a file that links with QEMU (GPLv2).** That covers UTM app code, CocoaSpice, AOSP/ART and ATL.
- [ ] **`src/app/Husk/Resources/husk-jit.js` is StikDebug's AGPL-3.0 script.**
  - Don't edit its body, and don't copy from it into GPL code.
  - It is documented as a separate work in `docs/01-licensing.md`.
  - A clean-room rewrite can only be done by someone who has never read it, `universal.js`, or the protocol details quoted in `docs/02` and the comments in `src/ios-jit/husk-brk.S`. An earlier attempt was stopped by a safety check, so don't retry it on your own initiative.
- [ ] **Don't bundle Google Play Services.** Don't add third-party binaries with no licence (cf. BreakpointJIT).
- [ ] **Don't fake licence or DRM checks for commercial games** (see `husk-tl-jni-minecraft.c`; PR #2 made them honest).

## 5. Code rules: check every change against these

- [ ] **Executable memory** comes only from `husk_ios_jit_carve()` (`src/ios-jit/husk-ios-jit.c`). The native runtime uses `tl_jit_carve()` / `tl_xmem_alloc()`. Never write into the region directly, and never assume offset 0. The region is one-shot and nothing is ever given back.
- [ ] **Host→guest calls** are wrapped:
  - `TL_GUEST_CALL(...)` (`husk-tl-guest.h`), or `TL_JNI_NATIVE_CALL(...)` (`husk-tl-jni.h`) when JNI locals are involved.
  - A game's `exit`/`abort` lands on the outermost landing pad; nothing ever `pthread_exit`s the main thread.
  - Once the game is ENDED, don't call into the guest again.
- [ ] **Guest exits** go through `tl_guest_fatal()` / `tl_guest_abort_at()` (`husk-tl-bionic.c`).
- [ ] **Guest file paths** go through `tl_path_confine()` / `tl_path_at()` (`husk-tl-bionic-path.c`), including raw syscalls in `tl_linux_syscall` (`husk-tl-bionic-io.c`).
- [ ] **JNI ownership:**
  - every `jobject` returned to guest code is a fresh local reference recorded in the current frame;
  - fields and array elements own their values;
  - `DeleteLocalRef` releases only recorded locals.
- [ ] **New threads that run guest code** call `tl_ld_thread_attach()` (the first landing pad does it automatically).
- [ ] **Signal handlers** use only async-signal-safe calls: no malloc, locks, `snprintf`, `dladdr` or `tl_log_line` (see `on_fatal`).
- [ ] **Shared data needs locks.** The shim, linker and JNI tables are shared across guest threads.
- [ ] **Guest shell commands** (`HuskBridgeFS.swift`) quote every guest- or user-controlled value with `AndroidHost.quote()`.
- [ ] **Bionic shims:** never bind a bionic function directly to Darwin unless signature, layouts, flags and errno semantics all match (`husk-tl-bionic.h`). The prototype's `husk-tl-shim.c` still falls back to host `dlsym`; don't copy that pattern.
- [ ] **Instruction rewriting:** verify every new encoding with an assembler, and keep stubs within branch range (±128 MiB for `b`, ±1 MiB for `ldr` literal).
- [ ] **Project file:** a new source file must also be added to `src/app/Husk.xcodeproj/project.pbxproj` with unique 24-hex IDs (`project.yml` globs folders, but the checked-in pbxproj is what ships). A new exported dylib symbol must be added to `wanted` in `scripts/integrate_husk.sh`.

## 6. Open work (as of 2026-10-05)

**Re-sync the stack first:**
- [ ] PR #1's branch has four follow-up commits (`ebcc347`…`2877253`) that PRs #2–#4 lack. Merge `origin/claude/fix-critical-jit-snapshot` into #2, then #2 into #3, then #3 into #4. Use merges, not rebases, since others may have the branches checked out.
- [ ] The only expected conflict is `src/app/Husk/JITBootstrap.swift`. Keep PR #4's 256/512 setting, then make the 512 MiB constants added by the follow-ups (`TL_JIT_REGION_BYTES`, `HUSK_JIT_REGION_BYTES`, `QemuRunner.jitRegionMiB`) follow the setting.
- [ ] Re-run `tests/translation-layer/run.sh` after each merge.

**Verify the stack:**
- [ ] Build PRs #1–#4 on a Mac (`scripts/ci_build.sh`) and work through each PR's device checklist (in each PR's description on GitHub).
- [ ] Watch for these Swift compile risks: `@MainActor` on `JITMethod.isAvailable`, `LenientInt` decoding, the Keychain code in `JITSetup.swift`.
- [ ] Confirm on Darwin arm64:
  - the TSD-slot discovery and `tpidrro_el0` masking (thread blocks and virtual x18);
  - `siglongjmp` across guest frames;
  - `F_GETPATH` in `tl_path_at`.

**High:**
- [ ] **Smaller Android snapshot.** Build and publish a 2–2.5 GiB snapshot (`smp` 2) from the fork's releases (`HUSK_RELEASES_REPO`, `scripts/publish_snapshot.sh` pins `guestMiB`), because the shipped 4 GiB one doesn't fit 6 GB. Mac task.
- [ ] **The guest shell on 127.0.0.1:5599 is unauthenticated.** Fixing it needs the LineageOS guest image's build source (`husk_agent`), which isn't in the repo. Get it, then add a per-boot token or move to virtio-serial.
- [ ] **GameMaker and other non-engine games** fall to the prototype loader. They need a native-runtime driver (GameMaker: replay `RunnerJNILib`). Start from the APK's import list in the translation layer report.

**Medium:**
- [ ] **JIT script licensing** is a legal judgement: grant from StikDebug, clean-room rewrite, or accept aggregation.
- [ ] **Guest JIT engines** (Mono, LuaJIT, V8 without jitless) can't get executable memory.
- [ ] **A guest exiting while holding locks** leaves them held, including the linker's `g_big` during constructors.
- [ ] **CA bundle:** decide macOS store vs Mozilla (`scripts/update_cacert.sh`) after a device test with real games.
- [ ] **Paths:** check-then-use race; `AF_UNIX` socket paths not confined; games that need `TMPDIR` are refused.
- [ ] **The prototype loader never reuses** its JIT slices; each attempt consumes region space until restart.
- [ ] **Source checksums** for six of the seven tarballs are trust-on-first-download; confirm them against upstream.

**Low:**
- [ ] Code spanning more than ~256 MiB still has unreachable `svc` sites (the -ENOSYS fallback).
- [ ] `checkLicense` returns -1 (assumed "unlicensed"); confirm from a device log.
- [ ] A guest passing a local to `DeleteGlobalRef` double-releases it (Android's CheckJNI aborts there).
- [ ] The TrollStore walkthrough page has unreachable "not found" text.
- [ ] Media-scan `file://` URIs aren't percent-encoded.

## 7. Before you hand back

- [ ] `sh tests/translation-layer/run.sh` passes. Paste the tail.
- [ ] The syntax check above passes for `husk-tl-ld.c`, and no new diagnostics appear in other changed C files.
- [ ] Re-read your diff adversarially: double frees, lock ordering, `longjmp` across cleanup, branch ranges, quoting.
- [ ] The report lists commits, files, what was verified, what was **not**, and open risks.
