# Built-in StikJIT and on-device pairing

Husk needs an attached debugger to get executable memory on iOS
([02-jit-substrate.md](02-jit-substrate.md)). That debugger can be the
StikDebug app, or Husk's own built-in StikJIT helper. Both run the same
bundled `husk-jit.js` against the running Husk process.

## Choosing a method

**Settings › JIT & sideload › Method**:

- **Automatic** (default): StikDebug when it is installed, then TrollStore,
  otherwise Built-in StikJIT. Husk does not switch methods after a failure; the
  error says what to fix.
- **StikDebug**: the URL-scheme hand-off Husk has always used.
- **TrollStore**: asks TrollStore to enable JIT (`apple-magnifier://enable-jit`),
  for a Husk installed through it.
- **Built-in StikJIT**: no second app. Needs iOS 26, LocalDevVPN, and this
  iPhone's remote pairing file, which Husk can make itself on iOS 27.

Pairing on the device or importing a pairing file selects Built-in StikJIT.

## The walkthrough

First-run setup has a **Turn on JIT** page, and **Settings › JIT & sideload ›
Set up JIT** opens the same walkthrough. Tapping Start without JIT opens it too
when the chosen method is not set up. It offers three ways in:

1. **Pair on this iPhone** (iOS 27). Tap **Start pairing** and allow Local
   Network access, open **Settings › Privacy & Security › Developer Mode**,
   scroll down, tap **Pair with Husk**, and enter the code Husk shows. The code
   also appears in iOS's background-task banner and as a notification.
2. **Use a pairing file** made on a computer with the
   [StikDebug pairing-file guide](https://github.com/StikDebug/StikDebug-Guide/blob/main/pairing_file.md).
3. **Use StikDebug**, which ends the walkthrough with StikDebug selected.
4. **Use TrollStore**, likewise, for a Husk installed through TrollStore.

The first two continue with the same two steps:

- **Connect LocalDevVPN**. LocalDevVPN returns to Husk through the `husk://`
  URL scheme once connected.
- **Check setup**, which checks the VPN and downloads, mounts and verifies the
  matching Developer Disk Image, then **Enable JIT**.

If the device resets the connection it no longer accepts the pairing (each
on-device pairing replaces the last), so the walkthrough offers **Pair again**.
If the device cannot be reached it offers LocalDevVPN. After an iOS update,
**Reset Developer Disk Image** clears stale DDI data.

## How it works

### On-device pairing

iOS 27 can pair with a computer it finds on the local network, started from the
iPhone. Husk plays that computer for its own iPhone:

- `libhusk_rppairing.a` (`src/rppairing-ios`, built by
  `scripts/build_rppairing_ios.sh`) wraps [idevice](https://github.com/jkcoxson/idevice)'s
  `PairableHost`. It listens on a TCP port and runs the pairing.
- `JITPairing.swift` advertises that port as a
  `_remotepairing-pairable-host._tcp` Bonjour service through mDNSResponder,
  so only the Local Network permission is needed, not the multicast entitlement.
- Husk keeps running while the user is in Settings through an iOS 26
  continued-processing task (`<bundle id>.pairing.session`). If iOS refuses it,
  typically because a sideloader changed the bundle identifier, Husk only gets
  the usual ~30 seconds in the background and the walkthrough says so. A pairing
  that has not finished after five minutes is stopped.

Every pairing makes a new host key under the same identifier, so the iPhone
replaces its earlier record for Husk and only the newest pairing file works.

The pairing file is device-sensitive: Husk sends it only to its own helper
process. It is kept in the Keychain as a generic password (service
`<bundle id>.pairing`, account `pairingFile`), readable after the first unlock
and on this device only, so it is neither visible through Finder file sharing
nor carried to another device by a backup. Earlier builds stored it at
`Documents/StikJIT/pairingFile.plist`; the first time Husk needs the pairing it
copies that file into the Keychain and deletes it (and the then-empty
`StikJIT` folder) once the Keychain gives the same bytes back. If the Keychain
cannot be written, the file stays where it is and is used from there, and the
log says why.

The Keychain and the translation layer's path confinement
(docs/04-translation-layer.md) are defence in depth, not a sandbox. An Android
game's native code runs inside Husk's process: confinement keeps its file
calls out of Documents, and the Keychain keeps the pairing out of the files a
game could reach, but code in the process can still read Husk's memory, and
the pairing is in memory while Husk uses it.

### The helper

A process cannot synchronously debug itself, so StikJIT runs in
`HuskJITHelper`, an iOS 26 ExtensionKit extension embedded at
`Husk.app/Extensions/HuskJITHelper.appex`. Husk sends it its PID, the pairing
file and `husk-jit.js` over XPC (`HuskJITMessages.swift`). The helper calls
StikJIT with `forceScript` and stays attached while the script services trap
requests. Husk polls `CS_DEBUGGED`, then claims its JIT region exactly as after
a StikDebug attach, and detaches; that detach is what ends the helper's request.

### Building

```
./scripts/build_rppairing_ios.sh   # needs rustup + `rustup target add aarch64-apple-ios`
./scripts/package_ipa.sh
```

`ci_build.sh` runs both. `cargo test` in `src/rppairing-ios` runs the pairing
library's host tests.

## Signing and installation

The app and `HuskJITHelper` must be signed together, and the sideloader must
keep the embedded ExtensionKit extension. If it cannot, use StikDebug.

To build under another bundle identifier, set `HUSK_BUNDLE_IDENTIFIER` in
`src/app/project.yml`. The app, the helper (`<id>.JITHelper`) and the helper's
extension point (`<id>.HuskJITHelper`) all follow it; changing only
`PRODUCT_BUNDLE_IDENTIFIER` leaves the helper unfindable.

Licensing of the bundled pieces is in [01-licensing.md](01-licensing.md).
