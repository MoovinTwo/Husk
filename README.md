# Husk

[![Husk Downloads](https://img.shields.io/github/downloads/leviidev/husk/total?style=for-the-badge&color=5865F2&labelColor=111111)](https://github.com/leviidev/husk/releases)

Android app launcher for iOS.

Drop in an APK, tap it, and the Android app opens full-screen.

## JIT

Husk needs JIT, which on iOS takes an attached debugger. Use StikDebug, or
Husk's built-in StikJIT helper (iOS 26+), which on iOS 27 can pair with your
iPhone from Settings with no computer. The app walks you through it; see
[docs/06-built-in-jit.md](docs/06-built-in-jit.md).

## Builds

The IPA is built locally on a Mac with Xcode: `./scripts/ci_build.sh
[output.ipa]` fetches the pinned, checksum-verified sources, builds QEMU and
its dependencies, ANGLE and the GPU stack, and packages an unsigned
`Husk.ipa` for AltStore, SideStore or TrollStore to sign and install. The
first run builds everything from scratch, which takes a couple of hours.

GitHub Actions ([tests.yml](.github/workflows/tests.yml)) runs the host-side
tests in `tests/translation-layer/run.sh` on every push and pull request; it
does not build the IPA.

## Licence

GPL-2.0-or-later. Husk links QEMU, which is GPLv2, so the shipped binary is a
combined GPLv2 work and the full source is public. It cannot go on the App
Store — both because of that and because it needs `get-task-allow` plus a
debugger attaching at runtime. See [docs/01-licensing.md](docs/01-licensing.md).
