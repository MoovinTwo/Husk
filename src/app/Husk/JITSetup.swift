// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Security
import SwiftUI
import UIKit

/// How Husk gets a debugger attached. See docs/06-built-in-jit.md.
enum JITMethod: String, CaseIterable, Identifiable {
    /// StikDebug when it is installed, then TrollStore, otherwise Built-in StikJIT.
    case automatic
    case stikDebug
    case trollStore
    case builtIn

    var id: String { rawValue }
    var title: String {
        switch self {
        case .automatic: return "Automatic"
        case .stikDebug: return "StikDebug"
        case .trollStore: return "TrollStore"
        case .builtIn: return "Built-in StikJIT"
        }
    }

    /// Whether this method can work on this device right now. TrollStore only answers
    /// its `apple-magnifier` scheme when it is installed, and it does not install on
    /// the iOS versions Husk targets, so offering it there only leads to a dead end.
    @MainActor var isAvailable: Bool {
        switch self {
        case .trollStore: return JITBootstrap.isTrollStoreInstalled
        case .automatic, .stikDebug, .builtIn: return true
        }
    }

    /// The methods Settings offers: the unavailable ones are left out rather than shown
    /// and refused, except the current choice, which a Picker needs a row for.
    @MainActor static func offered(keeping current: JITMethod) -> [JITMethod] {
        allCases.filter { $0.isAvailable || $0 == current }
    }
}

/// Where Built-in StikJIT's pairing file came from.
enum JITPairingSource: String {
    /// Made by Husk on this device (iOS 27 and later, OnDevicePairing).
    case onDevice
    /// Imported from a file made on a computer.
    case imported
}

/// The RPPairing file Built-in StikJIT authenticates with. It is device-sensitive and
/// is only ever sent to Husk's own helper process, so it lives in the Keychain, on this
/// device only, rather than as a file: Documents is visible through Finder file sharing,
/// and Android games run their native code inside this process. Earlier builds kept it
/// at Documents/StikJIT/pairingFile.plist; the first access moves it.
enum JITPairingFileStore {
    private static let service = (Bundle.main.bundleIdentifier ?? "com.husk.app") + ".pairing"
    private static let account = "pairingFile"
    private static let sourceKey = "husk.jitPairingSource"

    /// Where earlier builds kept it.
    private static var legacyDirectory: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT", isDirectory: true)
    }
    private static var legacyURL: URL { legacyDirectory.appendingPathComponent("pairingFile.plist") }
    private static var legacyExists: Bool { FileManager.default.fileExists(atPath: legacyURL.path) }

    /// Runs once, on first use, before anything reads the Keychain.
    private static let migration: Void = migrateLegacyFile()

    static var exists: Bool {
        _ = migration
        if (try? keychainData()) != nil { return true }      // try? folds "none" and "unreadable" into nil
        return legacyExists
    }

    static var source: JITPairingSource? {
        guard exists else { return nil }
        return UserDefaults.standard.string(forKey: sourceKey).flatMap(JITPairingSource.init(rawValue:)) ?? .imported
    }

    static func data() throws -> Data {
        _ = migration
        do {
            if let data = try keychainData() { return data }
        } catch where legacyExists {
            // The Keychain cannot be read, but the file the move left behind still can.
        }
        // Only there when it could not be moved: then the file is the pairing.
        if legacyExists { return try Data(contentsOf: legacyURL) }
        throw NSError(domain: "HuskJIT", code: 11,
                      userInfo: [NSLocalizedDescriptionKey: "Pair this device or import its pairing file first."])
    }

    static func importFile(from source: URL) throws {
        let scoped = source.startAccessingSecurityScopedResource()
        defer { if scoped { source.stopAccessingSecurityScopedResource() } }
        try store(try Data(contentsOf: source), source: .imported)
    }

    /// Validates an RPPairing plist and makes it the pairing file.
    static func store(_ data: Data, source: JITPairingSource) throws {
        guard !data.isEmpty,
              let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil),
              let dictionary = plist as? [String: Any],
              let publicKey = dictionary["public_key"] as? Data, publicKey.count == 32,
              let privateKey = dictionary["private_key"] as? Data, privateKey.count == 32,
              let identifier = dictionary["identifier"] as? String, !identifier.isEmpty else {
            throw NSError(domain: "HuskJIT", code: 10,
                          userInfo: [NSLocalizedDescriptionKey:
                            "That is not a remote pairing file. Make one with the StikDebug pairing-file guide and try again."])
        }
        _ = migration
        try writeKeychain(data)
        UserDefaults.standard.set(source.rawValue, forKey: sourceKey)
        // A file an earlier build left (one the move could not take) is now out of date.
        if legacyExists { removeLegacyFile() }
    }

    // MARK: - Keychain

    private static var baseQuery: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: account]
    }

    /// The stored pairing, or nil when there is none.
    private static func keychainData() throws -> Data? {
        var query = baseQuery
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = result as? Data else { throw keychainError(status, doing: "read") }
        return data
    }

    /// Replaces the stored pairing in place, so a failed write leaves the old one rather than none.
    private static func writeKeychain(_ data: Data) throws {
        let attributes: [String: Any] = [kSecValueData as String: data,
                                         kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly]
        var status = SecItemUpdate(baseQuery as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            status = SecItemAdd(baseQuery.merging(attributes) { $1 } as CFDictionary, nil)
        }
        guard status == errSecSuccess else { throw keychainError(status, doing: "save") }
    }

    private static func keychainError(_ status: OSStatus, doing verb: String) -> NSError {
        let reason = SecCopyErrorMessageString(status, nil) as String? ?? "error \(status)"
        return NSError(domain: NSOSStatusErrorDomain, code: Int(status),
                       userInfo: [NSLocalizedDescriptionKey: "Husk could not \(verb) the pairing file in the Keychain: \(reason)."])
    }

    // MARK: - Moving an earlier build's file

    /// Copies Documents/StikJIT/pairingFile.plist into the Keychain and deletes it, but only once
    /// the Keychain gives the same bytes back: a pairing that cannot be moved stays where it is,
    /// and is used from there, rather than being lost.
    private static func migrateLegacyFile() {
        guard legacyExists else { return }
        do {
            // The current build never writes the file, so one that is there is the newest pairing.
            let data = try Data(contentsOf: legacyURL)
            try writeKeychain(data)
            guard try keychainData() == data else {
                throw NSError(domain: "HuskJIT", code: 12,
                              userInfo: [NSLocalizedDescriptionKey: "the Keychain did not give it back"])
            }
            removeLegacyFile()
            HuskLog.log("jit", "pairing file moved from Documents into the Keychain")
        } catch {
            HuskLog.log("jit", "pairing file left in Documents: \(error.localizedDescription)")
        }
    }

    private static func removeLegacyFile() {
        let fm = FileManager.default
        do {
            try fm.removeItem(at: legacyURL)
            if (try? fm.contentsOfDirectory(atPath: legacyDirectory.path))?.isEmpty == true {
                try fm.removeItem(at: legacyDirectory)
            }
        } catch {
            HuskLog.log("jit", "could not delete the old pairing file in Documents: \(error.localizedDescription)")
        }
    }
}

@MainActor
final class JITCoordinator: ObservableObject {
    static let shared = JITCoordinator()

    /// Why the built-in helper could not reach this device, read from its error.
    enum ConnectionProblem: Equatable {
        /// Nothing answered at LocalDevVPN's address: the VPN is off or not routing.
        case vpn
        /// The device closed the connection, usually because it no longer accepts
        /// this pairing (every new pairing replaces the last).
        case pairing

        init?(helperMessage: String) {
            let text = helperMessage.lowercased()
            let has = { (needles: [String]) in needles.contains(where: text.contains) }
            if has(["connectionreset", "connection reset", "device refused connection"]) {
                self = .pairing
            } else if has(["connectionrefused", "connection refused", "timedout", "timed out",
                           "networkunreachable", "network unreachable", "hostunreachable",
                           "host unreachable", "no route"]) {
                self = .vpn
            } else {
                return nil
            }
        }

        var message: String {
            switch self {
            case .vpn:
                return "Husk couldn't reach this device. Connect LocalDevVPN, then try again."
            case .pairing:
                return "This device closed the connection, which usually means it no longer accepts "
                     + "Husk's pairing. Pair again, and check that LocalDevVPN is connected."
            }
        }
    }

    @Published var method: JITMethod {
        didSet { UserDefaults.standard.set(method.rawValue, forKey: "husk.jitMethod") }
    }
    @Published var showSetup = false
    @Published private(set) var connectionProblem: ConnectionProblem?
    @Published private(set) var busy = false
    @Published private(set) var status: String?
    @Published private(set) var error: String?
    @Published private(set) var txmPresent: Bool?
    /// The setup check passed for the current pairing file.
    @Published private(set) var prepared = false
    @Published private(set) var pairingSource = JITPairingFileStore.source
    /// Bumped each time the built-in helper attaches. Husk stays in the
    /// foreground for that, so nothing else tells ContentView to claim the region.
    @Published private(set) var attachGeneration = 0

    var hasPairing: Bool { pairingSource != nil }

    private init() {
        method = UserDefaults.standard.string(forKey: "husk.jitMethod")
            .flatMap(JITMethod.init(rawValue:)) ?? .automatic
        validateMethod()
    }

    /// A choice saved when its method still worked (TrollStore since removed, or a
    /// setting carried over from another device) falls back to Automatic rather than
    /// failing at every launch.
    func validateMethod() {
        guard !method.isAvailable else { return }
        log("stored JIT method \(method.title) is not available on this device; using Automatic")
        method = .automatic
    }

    /// The concrete method to use. Automatic picks the first one that can work here,
    /// and a choice that has stopped working is treated as Automatic too.
    var resolvedMethod: JITMethod {
        guard method == .automatic || !method.isAvailable else { return method }
        if JITBootstrap.isStikDebugInstalled { return .stikDebug }
        if JITMethod.trollStore.isAvailable { return .trollStore }
        return .builtIn
    }

    var automaticDescription: String {
        switch resolvedMethod {
        case .stikDebug: return "StikDebug is installed, so Husk will open it."
        case .trollStore: return "TrollStore is installed, so Husk will ask it to enable JIT."
        default: return "StikDebug and TrollStore were not found, so Husk will use its built-in helper."
        }
    }

    func refreshPairingStatus() {
        pairingSource = JITPairingFileStore.source
    }

    private func log(_ line: String) { HuskLog.log("jit", line) }

    /// Get a debugger attached with whichever method applies, or open the setup
    /// walkthrough when that method is not set up yet.
    func enable() {
        guard !JITBootstrap.isDebuggerAttached else { return }
        error = nil
        connectionProblem = nil
        switch resolvedMethod {
        case .automatic:
            assertionFailure("Automatic must resolve to a concrete JIT method")
        case .stikDebug:
            if !JITBootstrap.requestAttach(), !JITBootstrap.requestTrollStoreAttach() {
                error = "StikDebug is not installed. Install it, or set up Built-in StikJIT."
                showSetup = true
            }
        case .trollStore:
            if !JITBootstrap.requestTrollStoreAttach() {
                error = "TrollStore could not be opened. Install Husk through TrollStore, or choose another method."
                showSetup = true
            }
        case .builtIn:
            guard HuskBuiltInJIT.isAvailable, hasPairing else {
                log("built-in JIT is not set up; opening the walkthrough")
                showSetup = true
                return
            }
            enableBuiltIn()
        }
    }

    func importPairingFile(_ source: URL) {
        do {
            try JITPairingFileStore.importFile(from: source)
            OnDevicePairing.shared.cancel()
            refreshPairingStatus()
            method = .builtIn
            prepared = false
            status = "Pairing file imported."
            error = nil
            log("pairing file imported")
        } catch {
            self.error = error.localizedDescription
        }
    }

    /// OnDevicePairing finished: its file becomes the pairing file and Built-in StikJIT the method.
    func storeOnDevicePairing(_ data: Data) throws {
        try JITPairingFileStore.store(data, source: .onDevice)
        refreshPairingStatus()
        method = .builtIn
        prepared = false
        status = "Paired on this device."
        error = nil
    }

    /// Check LocalDevVPN and the Developer Disk Image, mounting it when needed.
    func prepareBuiltIn() {
        guard let pairing = builtInPairing() else { return }
        busy = true
        error = nil
        connectionProblem = nil
        status = "Checking LocalDevVPN and the Developer Disk Image…"
        HuskBuiltInJIT.send(.prepare(pairingData: pairing)) { [weak self] result in
            guard let self else { return }
            busy = false
            switch result {
            case .success(let response):
                txmPresent = response.txmPresent
                prepared = response.success
                status = response.success ? response.message : nil
                error = response.success ? nil : helperFailure(response.message)
                log("setup check \(response.success ? "passed" : "failed"): \(response.message)")
            case .failure(let failure):
                prepared = false
                status = nil
                error = failure.localizedDescription
            }
        }
    }

    func enableBuiltIn() {
        guard let pairing = builtInPairing() else { return }
        guard let script = JITBootstrap.scriptBase64 else {
            error = "Husk's JIT script is missing from this installation. Reinstall Husk."
            return
        }

        busy = true
        error = nil
        connectionProblem = nil
        status = "Starting Husk's JIT helper…"
        log("enabling JIT with the built-in helper")
        var readiness: Timer?
        var finished = false
        let finish: (String?) -> Void = { [weak self] failure in
            guard let self, !finished else { return }
            finished = true
            readiness?.invalidate()
            busy = false
            if let failure {
                status = nil
                error = failure
                log("built-in JIT failed: \(failure)")
                showSetup = true
            } else {
                status = "JIT is on."
                error = nil
                attachGeneration += 1
                log("built-in helper attached")
            }
        }

        HuskBuiltInJIT.send(
            .enable(targetPID: getpid(), pairingData: pairing, scriptBase64: script),
            started: { [weak self] in
                self?.status = "Waiting for the helper to attach…"
                readiness = Self.waitForDebugger { attached in
                    finish(attached ? nil : "The helper did not attach within 90 seconds. "
                                          + "Check that LocalDevVPN is connected, then try again.")
                }
            },
            completion: { [weak self] result in
                // The helper replies once Husk detaches, or when attaching failed.
                switch result {
                case .success(let response):
                    self?.txmPresent = response.txmPresent
                    HuskLog.log("jit", "helper: \(response.message)")
                    if !response.success {
                        finish(self?.helperFailure(response.message) ?? response.message)
                    }
                case .failure(let failure):
                    finish(failure.localizedDescription)
                }
            })
    }

    func resetDDI() {
        busy = true
        error = nil
        status = "Resetting the Developer Disk Image cache…"
        HuskBuiltInJIT.send(.resetDDI) { [weak self] result in
            guard let self else { return }
            busy = false
            prepared = false
            switch result {
            case .success(let response):
                status = response.success ? response.message : nil
                error = response.success ? nil : response.message
            case .failure(let failure):
                status = nil
                error = failure.localizedDescription
            }
        }
    }

    private func builtInPairing() -> Data? {
        if let reason = HuskBuiltInJIT.unavailableReason {
            error = reason
            return nil
        }
        guard let pairing = try? JITPairingFileStore.data() else {
            error = "Pair this device or import its pairing file first."
            return nil
        }
        return pairing
    }

    /// The helper's failure as shown: a connection problem gets its plain
    /// explanation (the helper's own message is already in the log).
    private func helperFailure(_ message: String) -> String {
        connectionProblem = ConnectionProblem(helperMessage: message)
        return connectionProblem?.message ?? message
    }

    /// Polls CS_DEBUGGED until it is set or `timeout` passes.
    private static func waitForDebugger(timeout: TimeInterval = 90,
                                        completion: @escaping (Bool) -> Void) -> Timer {
        let deadline = Date().addingTimeInterval(timeout)
        let timer = Timer(timeInterval: 0.5, repeats: true) { timer in
            if JITBootstrap.debuggedFlag {
                timer.invalidate()
                completion(true)
            } else if Date() >= deadline {
                timer.invalidate()
                completion(false)
            }
        }
        RunLoop.main.add(timer, forMode: .common)
        return timer
    }
}

/// LocalDevVPN, which Built-in StikJIT and StikDebug reach the device through:
/// open it to connect when it is installed, otherwise its App Store page.
enum LocalDevVPN {
    static let appStore = URL(string: "https://apps.apple.com/us/app/localdevvpn/id6755608044")!
    /// `enable` connects the VPN; `scheme` has LocalDevVPN return to Husk afterwards.
    static let connect = URL(string: "localdevvpn://enable?scheme=husk")!

    static var isInstalled: Bool { UIApplication.shared.canOpenURL(URL(string: "localdevvpn://")!) }
    static var actionTitle: String { isInstalled ? "Connect LocalDevVPN" : "Get LocalDevVPN" }

    @MainActor static func open() {
        let installed = isInstalled
        HuskLog.log("jit", "LocalDevVPN \(installed ? "connect" : "app-store")")
        UIApplication.shared.open(installed ? connect : appStore)
    }
}
