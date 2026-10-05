#!/bin/sh
# Husk dependency pins.
# shellcheck disable=SC2034  # sourced by fetch_sources.sh
#
# Versions deliberately match UTM's patches/sources where they overlap, because those
# exact versions are known to cross-compile for arm64-apple-ios. Husk needs only the
# subset required for a headless system-mode aarch64 QEMU: no spice, no gstreamer,
# no virglrenderer, no MoltenVK, no usb, no tpm.
#
# Every tarball is pinned by SHA-256 as well as URL; scripts/fetch_sources.sh refuses
# a download (or an already-downloaded file) whose hash does not match. When bumping
# a version, update its *_SHA256 too (sha256sum FILE, or shasum -a 256 FILE on macOS).
# An empty pin is warned about, not enforced.

# QEMU: UTM's fork release. Chosen over upstream v11.1.1 because this tarball already
# carries --enable-shared-lib (QEMU built as a dylib rather than an executable), which
# upstream does not have and which Husk requires -- iOS apps cannot spawn processes,
# so QEMU must live in-process. Its TCG is stock upstream: UTM's separate
# qemu-10.0.12-utm.patch touches 28 files and not one of them is under tcg/.
QEMU_SRC="https://github.com/utmapp/qemu/releases/download/v10.0.12-utm/qemu-10.0.12-utm.tar.xz"
QEMU_SHA256="7c9605290b34152debb842e965a55d2e4fbba4793e536ab677b4027e5dc0ba1a"

# Hard requirements for system-mode QEMU.
FFI_SRC="https://github.com/libffi/libffi/releases/download/v3.5.0/libffi-3.5.0.tar.gz"
FFI_SHA256="8c72678628a5dd8782f08ad421d5a441e42c1c5c1b33e0bc211cbfcf1f3b3978"
ICONV_SRC="https://ftp.gnu.org/gnu/libiconv/libiconv-1.16.tar.gz"
ICONV_SHA256="e6a1b1b589654277ee790cce3734f07876ac4ccfaecbee8afa0b649cf529cc04"
GETTEXT_SRC="https://ftp.gnu.org/gnu/gettext/gettext-0.22.5.tar.gz"
GETTEXT_SHA256="ec1705b1e969b83a9f073144ec806151db88127f5e40fe5a94cb6c8fa48996a0"
GLIB_SRC="https://download.gnome.org/sources/glib/2.83/glib-2.83.0.tar.xz"
GLIB_SHA256="a07d9e1a57a4279c5ece71c26dc44eea12bd518ea9ff695d53e722997032b614"
PIXMAN_SRC="https://www.cairographics.org/releases/pixman-0.38.0.tar.gz"
PIXMAN_SHA256="a7592bef0156d7c27545487a52245669b00cf7e70054505381cff2136d890ca8"

# Coroutines. The iOS SDK deprecates/withholds makecontext/swapcontext, so QEMU's
# ucontext coroutine backend needs this reimplementation. UTM's fork is pinned because
# it carries the Darwin/arm64 assembly fixes.
LIBUCONTEXT_REPO="https://github.com/utmapp/libucontext.git"
LIBUCONTEXT_COMMIT="9b1d8f01a6e99166f9808c79966abe10786de8b6"

# User-mode networking. Not needed for Phase 0 (the Debian guest boots without a NIC),
# but Android will not come up cleanly without a network, so build it now.
SLIRP_SRC="https://github.com/utmapp/libslirp/releases/download/v4.9.1-release-mirror/libslirp-v4.9.1.tar.gz"
SLIRP_SHA256="3970542143b7c11e6a09a4d2b50f30a133473c41f15ed0bdcc3b7a1c450d9a5c"
