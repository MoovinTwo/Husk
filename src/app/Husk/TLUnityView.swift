// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit
import QuartzCore
import AVFoundation

/// The engines the native runtime drives. Both draw into a CAMetalLayer through ANGLE, and both are one game per
/// process: an engine cannot be unloaded once it has started.
enum TLNativeEngine {
    case unity     // Subway Surfers and other Unity games: portrait, driven by UnityPlayer's own thread
    case cocos     // Geometry Dash and other cocos2d-x games: landscape, driven by a GL thread of our own
    case minecraft // Minecraft and other GameActivity games: landscape, multi-touch, the game runs its own threads
}

/// A Unity game's screen: one CAMetalLayer that the game's own GL (ANGLE over Metal) presents into.
///
/// Nothing is copied or composed here. The runtime hands the layer to EGL as the game's window; the
/// game draws and presents on its own thread. This view's jobs are the layer's size, the pause that
/// goes with leaving the screen, and turning touches into the pixel coordinates Android reports.
final class TLUnityUIView: UIView, UIKeyInput {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    /// The cocos2d-x game on screen, which the game's keyboard requests (they arrive on its GL thread) are routed to.
    nonisolated(unsafe) static weak var cocosView: TLUnityUIView?

    private let apk: String
    private let dataDir: String
    private let engine: TLNativeEngine
    private var launched = false
    /// Where the corner statistics go when this view does not draw them itself (a landscape game has its own bar).
    var onStats: ((String) -> Void)?
    /// Active touches by UITouch identity, each given a small stable id like Android's pointer ids.
    private var pointers: [ObjectIdentifier: Int32] = [:]

    /// "58 fps · 11.2 ms" in the corner, as the other screen has: frames the game finished per second, and the
    /// mean time one frame takes it. Refreshed once a second from the runtime's own counters.
    private let stats = UILabel()
    private var statsTimer: Timer?

    init(apk: String, dataDir: String, engine: TLNativeEngine) {
        self.apk = apk
        self.dataDir = dataDir
        self.engine = engine
        super.init(frame: .zero)
        backgroundColor = .black
        isMultipleTouchEnabled = true
        // Two pixels per point: sharp enough, and a third of the pixels a 3x phone would ask the game
        // for -- a 3D game is limited by fill rate, and ANGLE's translation costs on top.
        contentScaleFactor = 2
        if let metal = layer as? CAMetalLayer {
            metal.pixelFormat = .bgra8Unorm
            metal.framebufferOnly = true
            metal.contentsScale = 2
            metal.isOpaque = true
        }

        stats.font = .monospacedSystemFont(ofSize: 10, weight: .medium)
        stats.textColor = .white
        stats.backgroundColor = UIColor.black.withAlphaComponent(0.55)
        stats.layer.cornerRadius = 4
        stats.layer.masksToBounds = true
        stats.textAlignment = .center
        stats.isUserInteractionEnabled = false
        stats.text = " "
        if engine == .unity { addSubview(stats) }
        if engine == .cocos {
            TLUnityUIView.cocosView = self
            TLUnityUIView.installKeyboardHandler()
        }
        // The GPU is not the app's while it is in the background: stop drawing, and carry on when it returns.
        NotificationCenter.default.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { _ in
            husk_unity_set_paused(true)
        }
        NotificationCenter.default.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            if self?.window != nil { husk_unity_set_paused(false) }
        }
    }

    deinit { statsTimer?.invalidate(); NotificationCenter.default.removeObserver(self) }

    private func updateStats() {
        var p = husk_unity_perf()
        husk_unity_perf_snapshot(&p)
        let text = p.fps > 0
            ? String(format: "%.0f fps · %.1f ms · max %.0f", p.fps, p.mean_ms, p.max_ms)
            : "starting"
        stats.text = text
        onStats?(text)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func layoutSubviews() {
        super.layoutSubviews()
        stats.frame = CGRect(x: bounds.width - 148, y: bounds.height - 22, width: 142, height: 16)
        guard bounds.width > 0, bounds.height > 0 else { return }
        let w = Int((bounds.width * contentScaleFactor).rounded())
        let h = Int((bounds.height * contentScaleFactor).rounded())
        (layer as? CAMetalLayer)?.drawableSize = CGSize(width: w, height: h)
        // A landscape game is told its size once, when it starts, so it must not start while the screen is still
        // turning: wait for a surface that is wider than it is tall.
        let ready = engine != .unity ? w > h : true
        if !launched, window != nil, ready { launch(width: w, height: h) }
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        statsTimer?.invalidate()
        statsTimer = nil
        if window != nil {
            husk_unity_set_paused(false)
            statsTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in self?.updateStats() }
        } else {
            husk_unity_set_paused(true)
            if isFirstResponder { resignFirstResponder() }
        }
        setNeedsLayout()
    }

    private func launch(width: Int, height: Int) {
        launched = true
        let angle = (Bundle.main.privateFrameworksPath ?? "") + "/libANGLE-shared.dylib"
        let ca = Bundle.main.path(forResource: "cacert", ofType: "pem") ?? ""
        try? FileManager.default.createDirectory(atPath: dataDir, withIntermediateDirectories: true)
        let layerPtr = Unmanaged.passUnretained(layer).toOpaque()
        if husk_unity_state() != Int32(HUSK_UNITY_IDLE) {
            // Already started this run: the engine cannot be loaded twice, so just show it again.
            HuskLog.log("tl", "unity: already started; resuming")
            return
        }
        // The other half of ContentView.start()'s check: QEMU and a native game
        // never share a launch, and QEMU cannot be stopped to make room.
        if QemuRunner.shared.isRunning {
            HuskLog.log("jit", "native: refusing to launch \(apk) while QEMU is running; "
                             + "restart Husk to play it")
            return
        }
        if engine != .unity {
            // The game plays through the silent switch, like the guest's own audio, and mixes with other audio.
            let session = AVAudioSession.sharedInstance()
            try? session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            try? session.setActive(true)
        }
        HuskLog.log("tl", "native: launching \(apk) at \(width)x\(height) (\(engine == .cocos ? "cocos2d-x" : engine == .minecraft ? "gameactivity" : "unity"))")
        let started: Bool
        switch engine {
        case .cocos: started = husk_cocos_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)
        case .minecraft: started = husk_gameactivity_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)
        case .unity: started = husk_unity_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)
        }
        if !started { HuskLog.log("tl", "native: launch refused") }
    }

    // MARK: keyboard (cocos2d-x games)

    /// A game asks for the keyboard when its text field is tapped. The keyboard belongs to this view; what it types goes
    /// to the game, and a strip above the keyboard shows the text, because in landscape the keyboard covers the game's field.
    override var canBecomeFirstResponder: Bool { engine == .cocos }
    var hasText: Bool { true }
    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var spellCheckingType: UITextSpellCheckingType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartDashesType: UITextSmartDashesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no
    var keyboardType: UIKeyboardType = .default
    var keyboardAppearance: UIKeyboardAppearance = .dark
    var returnKeyType: UIReturnKeyType = .done

    private var typed = ""
    private lazy var typedLabel: UILabel = {
        let l = UILabel()
        l.font = .systemFont(ofSize: 17, weight: .medium)
        l.textColor = .white
        l.lineBreakMode = .byTruncatingHead
        return l
    }()
    private lazy var keyboardBar: UIView = {
        let bar = UIView(frame: CGRect(x: 0, y: 0, width: 100, height: 44))
        bar.backgroundColor = UIColor(white: 0.12, alpha: 1)
        bar.autoresizingMask = [.flexibleWidth]
        typedLabel.frame = CGRect(x: 16, y: 0, width: 100, height: 44)
        typedLabel.autoresizingMask = [.flexibleWidth]
        bar.addSubview(typedLabel)
        let done = UIButton(type: .system)
        done.setTitle("Done", for: .normal)
        done.titleLabel?.font = .systemFont(ofSize: 17, weight: .semibold)
        done.frame = CGRect(x: 100, y: 0, width: 80, height: 44)
        done.autoresizingMask = [.flexibleLeftMargin]
        done.addAction(UIAction { [weak self] _ in self?.finishTyping() }, for: .touchUpInside)
        bar.addSubview(done)
        return bar
    }()
    override var inputAccessoryView: UIView? { engine == .cocos ? keyboardBar : nil }

    func insertText(_ text: String) {
        if text == "\n" { finishTyping(); return }
        typed += text
        typedLabel.text = typed
        husk_cocos_insert_text(text)
    }

    func deleteBackward() {
        if !typed.isEmpty { typed.removeLast() }
        typedLabel.text = typed
        husk_cocos_delete_backward()
    }

    private func setTyped(_ text: String) { typed = text; typedLabel.text = text }

    /// Return, or the Done button: what Android's "done" action does -- the game gets a newline, and the keyboard goes.
    private func finishTyping() {
        husk_cocos_insert_text("\n")
        resignFirstResponder()
    }

    /// The game's own requests, from its GL thread: 0 toggles, 1 shows, 2 hides.
    static func installKeyboardHandler() {
        // A link in the game (terms of use, the social buttons) opens in the browser.
        husk_cocos_set_open_url_handler { url in
            guard let url, let link = URL(string: String(cString: url)) else { return }
            DispatchQueue.main.async { UIApplication.shared.open(link) }
        }
        husk_cocos_set_keyboard_handler { action in
            DispatchQueue.main.async {
                guard let view = TLUnityUIView.cocosView else { return }
                let show = action == 1 || (action == 0 && !view.isFirstResponder)
                if show {
                    // Start the strip from what the game's field already holds, so editing a name shows the whole name.
                    husk_cocos_request_text { text in
                        let seed = text.map { String(cString: $0) } ?? ""
                        DispatchQueue.main.async { TLUnityUIView.cocosView?.setTyped(seed) }
                    }
                    view.becomeFirstResponder()
                } else {
                    view.resignFirstResponder()
                }
            }
        }
    }

    // MARK: touch

    private func id(for touch: UITouch) -> Int32 {
        let key = ObjectIdentifier(touch)
        if let existing = pointers[key] { return existing }
        var next: Int32 = 0
        while pointers.values.contains(next) { next += 1 }
        pointers[key] = next
        return next
    }

    private func send(_ touches: Set<UITouch>, phase: Int32) {
        for t in touches {
            let p = t.location(in: self)
            let pid = id(for: t)
            husk_unity_touch(phase, pid, Float(p.x * contentScaleFactor), Float(p.y * contentScaleFactor))
            if phase == 2 || phase == 3 { pointers[ObjectIdentifier(t)] = nil }
        }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?)     { send(touches, phase: 0) }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?)     { send(touches, phase: 1) }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?)     { send(touches, phase: 2) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        send(touches, phase: 2)
        pointers.removeAll()
    }
}

struct TLUnityScreen: UIViewRepresentable {
    let apk: String
    let dataDir: String
    var engine: TLNativeEngine = .unity
    var onStats: ((String) -> Void)? = nil
    /// One view per game for the life of the process. The engine's GPU surface belongs to this view's layer and an
    /// engine cannot be started twice, so coming back to the game must show the same layer, not a new one.
    private static var shared: [String: TLUnityUIView] = [:]

    func makeUIView(context: Context) -> TLUnityUIView {
        if let view = Self.shared[apk] { view.onStats = onStats; return view }
        let view = TLUnityUIView(apk: apk, dataDir: dataDir, engine: engine)
        view.onStats = onStats
        Self.shared[apk] = view
        return view
    }
    func updateUIView(_ view: TLUnityUIView, context: Context) { view.onStats = onStats }
}

/// Polls the runtime for the status line and its log, ten times a second at most.
@MainActor
final class TLUnityModel: ObservableObject {
    @Published var state: Int32 = 0
    @Published var frames: UInt = 0
    @Published var logText = ""
    private var timer: Timer?
    private var ticks = 0

    func start() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    func stop() { timer?.invalidate(); timer = nil }

    private func poll() {
        ticks += 1
        state = husk_unity_state()
        frames = husk_unity_frames()
        if ticks % 4 == 0, let c = husk_tl_attempt_log() {
            let text = String(cString: c)
            free(c)
            if text != logText { logText = text }
        }
    }

    var statusText: String {
        switch state {
        case Int32(HUSK_UNITY_STARTING): return "Loading the engine…"
        case Int32(HUSK_UNITY_RUNNING):  return "Running"
        case Int32(HUSK_UNITY_FAILED):   return "Could not start — see the log"
        case Int32(HUSK_UNITY_ENDED):    return "The game exited"
        default:                         return "Starting"
        }
    }

    var subStatusText: String {
        switch state {
        case Int32(HUSK_UNITY_RUNNING): return "\(frames) frame(s) drawn · native runtime"
        case Int32(HUSK_UNITY_STARTING): return "Loading libraries and starting the engine"
        default: return "Native runtime"
        }
    }

    var statusColor: Color {
        switch state {
        case Int32(HUSK_UNITY_RUNNING):  return Theme.good
        case Int32(HUSK_UNITY_FAILED):   return .red
        case Int32(HUSK_UNITY_ENDED):    return .orange
        default:                         return Theme.accent
        }
    }
}

/// The Unity game the way the other runner shows a game: a status header, the screen taking whatever the log
/// leaves, and a log underneath that opens and closes. Presented full screen, not as a sheet, so a swipe in the
/// game is the game's.
struct TLUnityAttemptView: View {
    let app: TLApp
    @Environment(\.dismiss) private var dismiss
    @StateObject private var model = TLUnityModel()
    @AppStorage("husk.tl.unity.showLog") private var showLogSetting = false
    @AppStorage(TranslationLayer.devInfoKey) private var devInfo = false
    /// The log is detail: without developer info it stays shut and its bar is not shown.
    private var showLog: Bool { get { showLogSetting && devInfo } nonmutating set { showLogSetting = newValue } }

    private var dataDir: String {
        TranslationLayer.root.appendingPathComponent(app.id, isDirectory: true)
            .appendingPathComponent("unity-data", isDirectory: true).path
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                HStack {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(model.statusText)
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(model.statusColor)
                        Text(model.subStatusText)
                            .font(.system(size: 12))
                            .foregroundStyle(Theme.textDim)
                    }
                    Spacer()
                }
                .padding()
                .background(Theme.surface)

                Divider()

                if let apk = app.apks.first {
                    TLUnityScreen(apk: apk, dataDir: dataDir)
                        .frame(maxWidth: .infinity, maxHeight: showLog ? 380 : .infinity)
                        .background(Color.black)
                    Divider()
                }

                if devInfo {
                HStack(spacing: 10) {
                    Button {
                        withAnimation(.snappy(duration: 0.25)) { showLog.toggle() }
                    } label: {
                        HStack(spacing: 6) {
                            Image(systemName: showLog ? "chevron.down" : "chevron.right")
                                .font(.system(size: 11, weight: .bold))
                                .frame(width: 12)
                            Text("ATTEMPT LOG")
                                .font(.technical(11, weight: .bold))
                        }
                        .foregroundStyle(Theme.textDim)
                    }
                    .buttonStyle(.plain)
                    Spacer()
                    if showLog {
                        Button { UIPasteboard.general.string = model.logText } label: {
                            Label("Copy", systemImage: "doc.on.doc").font(.system(size: 12))
                        }
                    } else {
                        Text("tap to show")
                            .font(.system(size: 11))
                            .foregroundStyle(Theme.textDim.opacity(0.7))
                    }
                }
                .padding(.horizontal)
                .padding(.vertical, 8)
                .contentShape(Rectangle())
                .onTapGesture {
                    if !showLog { withAnimation(.snappy(duration: 0.25)) { showLog = true } }
                }

                }

                if showLog {
                    ScrollViewReader { proxy in
                        ScrollView {
                            Text(model.logText.isEmpty ? "Starting…" : model.logText)
                                .font(.technical(11))
                                .foregroundStyle(Theme.text)
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(12)
                                .textSelection(.enabled)
                                .id("bottom")
                        }
                        .background(Theme.bg)
                        .onChange(of: model.logText) { _ in proxy.scrollTo("bottom", anchor: .bottom) }
                    }
                    .transition(.opacity)
                }
            }
            .background(Theme.bg.ignoresSafeArea())
            .navigationTitle(app.label)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } }
            }
        }
        // Swipes near the edges are the game's: keep the system from taking them for itself.
        .defersSystemGestures(on: .all)
        .onAppear { model.start() }
        .onDisappear { model.stop() }
    }
}


/// A cocos2d-x game's screen (Geometry Dash). These are landscape games: the app turns to landscape while this is up and
/// back afterwards, the game takes the whole screen, and a thin bar above it carries what the other runners show -- the
/// status, the frames per second and the time a frame takes -- and, with developer info on, the log beside the game.
struct TLCocosAttemptView: View {
    let app: TLApp
    @Environment(\.dismiss) private var dismiss
    @StateObject private var model = TLUnityModel()
    @AppStorage("husk.tl.unity.showLog") private var showLogSetting = false
    @AppStorage(TranslationLayer.devInfoKey) private var devInfo = false
    @State private var stats = "starting"
    private var showLog: Bool { get { showLogSetting && devInfo } nonmutating set { showLogSetting = newValue } }

    /// Geometry Dash and the like are cocos2d-x; Minecraft is built on GameActivity. Both are landscape.
    private var engine: TLNativeEngine { app.report?.nativeEngine == .minecraft ? .minecraft : .cocos }

    private var dataDir: String {
        TranslationLayer.root.appendingPathComponent(app.id, isDirectory: true)
            .appendingPathComponent(engine == .minecraft ? "minecraft-data" : "cocos-data", isDirectory: true).path
    }

    /// Another game is already loaded in this session, and an engine cannot be loaded twice.
    private var blockedBy: String? {
        guard let loaded = husk_native_loaded_apk().map({ String(cString: $0) }), loaded != app.apks.first else { return nil }
        return (loaded as NSString).lastPathComponent
    }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: 0) {
                bar
                if let other = blockedBy {
                    VStack(spacing: 8) {
                        Text("Another game is already loaded")
                            .font(.system(size: 16, weight: .semibold)).foregroundStyle(.white)
                        Text("\(other) was started in this session, and a game cannot be unloaded once it has started. Close Husk completely and open it again to run \(app.label).")
                            .font(.system(size: 13)).foregroundStyle(.white.opacity(0.7))
                            .multilineTextAlignment(.center).frame(maxWidth: 460)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if let apk = app.apks.first {
                    HStack(spacing: 0) {
                        TLUnityScreen(apk: apk, dataDir: dataDir, engine: engine, onStats: { stats = $0 })
                            .background(Color.black)
                        if showLog { logPanel.frame(width: 320) }
                    }
                    .ignoresSafeArea(.container, edges: [.horizontal, .bottom])
                }
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        // Swipes near the edges are the game's.
        .defersSystemGestures(on: .all)
        .onAppear { HuskOrientation.set(.landscape); model.start() }
        .onDisappear { model.stop(); HuskOrientation.set(HuskOrientation.standard) }
    }

    private var bar: some View {
        HStack(spacing: 12) {
            Button { dismiss() } label: {
                Label("Close", systemImage: "xmark").font(.system(size: 13, weight: .semibold))
            }
            .tint(.white)
            Circle().fill(model.statusColor).frame(width: 7, height: 7)
            Text(model.state == Int32(HUSK_UNITY_RUNNING) ? app.label : model.statusText)
                .font(.system(size: 12, weight: .medium)).foregroundStyle(.white.opacity(0.85)).lineLimit(1)
            Spacer()
            if model.state == Int32(HUSK_UNITY_RUNNING) {
                Text(stats).font(.technical(11)).foregroundStyle(.white.opacity(0.7)).lineLimit(1)
            }
            if devInfo {
                Button { withAnimation(.snappy(duration: 0.25)) { showLog.toggle() } } label: {
                    Text(showLog ? "Hide log" : "Log").font(.system(size: 12, weight: .semibold))
                }
                .tint(.white)
            }
        }
        .padding(.horizontal, 14)
        .frame(height: 30)
        .background(Color(white: 0.08))
    }

    private var logPanel: some View {
        VStack(spacing: 0) {
            HStack {
                Text("ATTEMPT LOG").font(.technical(10, weight: .bold)).foregroundStyle(Theme.textDim)
                Spacer()
                Button { UIPasteboard.general.string = model.logText } label: {
                    Label("Copy", systemImage: "doc.on.doc").font(.system(size: 11))
                }
            }
            .padding(.horizontal, 10).padding(.vertical, 6)
            ScrollViewReader { proxy in
                ScrollView {
                    Text(model.logText.isEmpty ? "Starting…" : model.logText)
                        .font(.technical(10))
                        .foregroundStyle(Theme.text)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                        .textSelection(.enabled)
                        .id("bottom")
                }
                .onChange(of: model.logText) { _ in proxy.scrollTo("bottom", anchor: .bottom) }
            }
        }
        .background(Theme.bg)
    }
}
