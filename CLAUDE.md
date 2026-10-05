# Husk: notes for Claude Code

@AGENTS.md

The five rules that matter most (details in AGENTS.md):

1. **Base your work correctly.** Branch from the newest unmerged PR branch (or `main` for independent work). Create worktrees from that base explicitly; never push to `main`.
2. **Say what you could not verify.** Swift and Darwin-only C do not compile on Linux. Run `sh tests/translation-layer/run.sh`, syntax-check what you can, and put a device checklist in the PR.
3. **Executable memory comes only from `husk_ios_jit_carve()`.** Guest calls go through `TL_GUEST_CALL` / `TL_JNI_NATIVE_CALL`, and guest paths through `tl_path_confine()`.
4. **Respect the licences.** Husk is GPL-2.0-or-later and links QEMU (GPLv2). Never copy Apache-2.0 or GPL-3.0 code in. `src/app/Husk/Resources/husk-jit.js` is StikDebug's AGPL-3.0 script: don't edit its body or copy from it.
5. **Never fake licence or DRM checks for games.**
