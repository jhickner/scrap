import AVFoundation
import Foundation

@MainActor
public final class VoiceController {
    public enum Mode: Equatable, Sendable {

        case idle

        case starting
        case listening

        case answering
        case speaking
    }

    static let echoGate: TimeInterval = 0.7

    static let uncancelledEchoGate: TimeInterval = 1.5

    static let pollInterval = Duration.milliseconds(150)

    static let bargeInWordCount = 2

    static let echoWordsWhileSpeaking = 2

    static let bargeInConfidence = 0.5

    static let bargeInLevelShare: Float = 0.25

    static let bargeInLevelWindow: TimeInterval = 1.5

    static let paragraphPause: TimeInterval = 0.5

    static let silentSpeechTimeout: TimeInterval = 0.5

    /* beyond the silence threshold, how long a volatile may sit with no final
       before the recognizer is treated as stalled */
    static let volatileStallTimeout: TimeInterval = 2.0

    public private(set) var mode: Mode = .idle {
        didSet { if mode != oldValue { onMode?(mode) } }
    }

    public private(set) var isConversing = false

    public private(set) var heardDraft = "" {
        didSet { if heardDraft != oldValue { onHeard?(heardDraft) } }
    }

    public private(set) var isSpeaking = false {
        didSet { if isSpeaking != oldValue { onSpeaking?(isSpeaking) } }
    }
    public var errorMessage: String? {
        didSet { if let errorMessage { onError?(errorMessage) } }
    }

    public var onSend: ((String) -> Void)?

    public var onInterrupt: (() -> Void)?
    public var onMode: ((Mode) -> Void)?
    public var onHeard: ((String) -> Void)?
    public var onSpeaking: ((Bool) -> Void)?
    public var onError: ((String) -> Void)?

    public var onNotice: ((String) -> Void)?

    private let settings: VoiceSettings
    private var volume: Float
    private var rate: Float
    private let engine = VoiceEngine()
    private var input: (any VoiceInput)?
    private var output: (any VoiceOutput)?
    private var chunker = SpokenTextChunker()
    private var endpointer: TurnEndpointer
    private var echo = EchoRejector()
    private var runTask: Task<Void, Never>?
    private var pollTask: Task<Void, Never>?

    private var volatileText = ""
    private var volatileAt: TimeInterval?
    private var promotedFinal: String?
    private var gateUntil: TimeInterval = 0
    private var isBusy = false
    private var heldUtterance: String?

    private var isReplyMuted = false

    private var rejectedEchoPrefix: String?

    private var suppressCurrentTurn = false

    private var lastTurnPeak: Float = 0

    private var silentSince: TimeInterval?

    private var pendingChime: VoiceChime?

    public init(settings: VoiceSettings = .default) {
        self.settings = settings
        self.volume = settings.volume
        self.rate = settings.rate
        self.endpointer = TurnEndpointer(silence: settings.silence)
        engine.playbackVolume = settings.volume
        engine.onConfigurationChange = { [weak self] in self?.handleRouteChange() }
        engine.onPlaybackDrained = { [weak self] in
            guard let self else { return }
            handlePlaybackDrained(echoCancelled: engine.isEchoCancelled)
        }
    }

    public func setVolume(_ volume: Float) {
        self.volume = min(max(volume, 0), 1)
        engine.playbackVolume = self.volume
        output?.volume = self.volume
    }

    public func setRate(_ rate: Float) {
        self.rate = min(max(rate, 0), 1)
        output?.rate = self.rate
    }

    public func setSilence(_ seconds: TimeInterval) {
        endpointer.silence = min(max(seconds, 0.4), 10)
    }

    public var isAvailable: Bool {
        if #available(macOS 26.0, *) { return true }
        return false
    }

    public func toggleConversation() {
        if isConversing {
            stopConversation()
        } else {
            startConversation()
        }
    }

    public func startConversation() {
        guard !isConversing else { return }
        guard isAvailable else {
            errorMessage = VoiceInputError.unsupported.localizedDescription
            return
        }
        isConversing = true
        mode = .starting
        runTask = Task { await run() }
    }

    public func stopConversation() {
        isConversing = false
        mode = .idle
        volatileText = ""
        volatileAt = nil
        promotedFinal = nil
        heardDraft = ""
        heldUtterance = nil
        suppressCurrentTurn = false
        isSpeaking = false
        pendingChime = nil
        endpointer.reset()
        echo.reset()
        pollTask?.cancel()
        pollTask = nil
        runTask?.cancel()
        runTask = nil
        input?.stop()
        input = nil
        output?.stop()
        output = nil
        engine.stop()
    }

    private func run() async {
        guard #available(macOS 26.0, *) else { return }
        VoiceLog.note("conversation: requesting permissions")
        guard await VoicePermissions.request() else {
            fail(VoiceInputError.notAuthorized.localizedDescription)
            return
        }
        VoiceLog.note("conversation: permissions granted; preparing recognizer")
        let recognizer = AnalyzerVoiceInput()
        input = recognizer

        let format: AVAudioFormat
        do {
            format = try await recognizer.prepare()
            VoiceLog.note("recognizer ready: \(format.sampleRate)Hz \(format.channelCount)ch")
        } catch {
            VoiceLog.problem("recognizer prepare failed: \(error)")
            fail(error.localizedDescription)
            return
        }
        guard !Task.isCancelled, isConversing else { return }

        let buffers: AsyncStream<VoiceAudioBuffer>
        do {
            buffers = try await engine.start(
                micFormat: format,
                preferredInputName: settings.preferredInput
            )
        } catch {
            VoiceLog.problem("engine start failed: \(error.localizedDescription)")
            fail(error.localizedDescription)
            return
        }
        guard !Task.isCancelled, isConversing else {
            engine.stop()
            return
        }

        output = makeOutput()
        mode = .listening
        if let pendingChime {
            engine.play(pendingChime)
            self.pendingChime = nil
        }
        startPolling()
        VoiceLog.note("listening (echo cancellation: \(engine.isEchoCancelled))")
        await recognizer.run(buffers: buffers) { [weak self] event in
            self?.handle(event)
        }
    }

    private func fail(_ message: String) {
        VoiceLog.problem("conversation stopped: \(message)")
        errorMessage = message
        stopConversation()
    }

    private func handleRouteChange() {
        guard isConversing else { return }
        stopConversation()
        isConversing = true
        mode = .starting
        runTask = Task { await run() }
    }

    private func handle(_ event: VoiceInputEvent) {
        switch event {
        case let .volatile(text):
            guard !suppressCurrentTurn else { return }
            let now = Date.timeIntervalSinceReferenceDate
            if considerInterrupting(text, isFinal: false) { return }
            guard hearable(text, isFinal: false, at: now) else {
                dropLeakedDraft()
                return
            }
            endpointer.noteVolatile(text, at: now)
            volatileText = text
            volatileAt = now
            updateHeardDraft()
        case let .final(text, confidence):
            volatileAt = nil
            if suppressCurrentTurn {
                suppressCurrentTurn = false
                promotedFinal = nil
                endpointer.reset()
                volatileText = ""
                heardDraft = ""
                return
            }
            if let promoted = promotedFinal {
                promotedFinal = nil
                if text.hasPrefix(promoted) || promoted.hasPrefix(text) {
                    VoiceLog.note("dropped final already promoted: \(text)")
                    volatileText = ""
                    return
                }
            }
            let now = Date.timeIntervalSinceReferenceDate
            if considerInterrupting(text, isFinal: true, confidence: confidence) { return }
            if TurnEndpointer.isStopCommand(text) {
                VoiceLog.note("stop command; nothing sent")
                endpointer.reset()
                heardDraft = ""
                return
            }
            guard hearable(text, isFinal: true, at: now, confidence: confidence) else {
                dropLeakedDraft()
                return
            }
            VoiceLog.note("final: \(text)")
            volatileText = ""
            switch endpointer.noteFinal(text, at: now) {
            case .cancelled:
                heardDraft = ""
            case .send, .waiting:
                updateHeardDraft()
            }
        case let .failed(message):
            VoiceLog.problem("recognizer failed: \(message)")
            fail(message)
        }
    }

    private func dropLeakedDraft() {
        guard replyInTheAir, !heardDraft.isEmpty else { return }
        endpointer.reset()
        volatileText = ""
        heardDraft = ""
    }

    private func updateHeardDraft() {
        heardDraft = [endpointer.draft, volatileText]
            .filter { !$0.isEmpty }
            .joined(separator: " ")
    }

    private func isGated(at now: TimeInterval) -> Bool {
        now < gateUntil
    }

    private var replyInTheAir: Bool {
        mode == .speaking || mode == .answering || engine.isSpeaking
    }

    private func hearable(
        _ text: String, isFinal: Bool, at now: TimeInterval, confidence: Double? = nil
    ) -> Bool {
        if isGated(at: now) {
            VoiceLog.note("\(isFinal ? "final" : "volatile") gated (mode \(mode)): \(text)")
            return false
        }

        var judged = text
        if let prefix = rejectedEchoPrefix {
            let heardWords = Self.words(text)
            let prefixWords = Self.words(prefix)
            if heardWords.starts(with: prefixWords) {
                let tail = Array(heardWords.dropFirst(prefixWords.count)).joined(separator: " ")
                guard Self.isBargeIn(tail) else {
                    VoiceLog.note("ignored own voice (grown from rejected prefix): \(text)")
                    return false
                }
                judged = tail
            }
        }
        let speaking = replyInTheAir
        let floor = speaking ? Self.echoWordsWhileSpeaking : nil
        let ratio = speaking ? EchoRejector.matchRatioWhileSpeaking : EchoRejector.matchRatio
        if echo.shouldReject(judged, minimumWords: floor, ratio: ratio) {
            VoiceLog.note("\(isFinal ? "final" : "volatile") rejected as echo: \(text)")
            if speaking { rejectedEchoPrefix = text }
            return false
        }
        if speaking, !isPlausiblyTheChild(text, confidence: confidence) {
            return false
        }

        if speaking, !endpointer.hasSpeech, !Self.isBargeIn(text) {
            return false
        }
        return true
    }

    private func startPolling() {
        pollTask?.cancel()
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: Self.pollInterval)
                guard let self else { return }
                await MainActor.run { self.poll() }
            }
        }
    }

    private func poll() {
        guard isConversing, mode != .idle, mode != .starting else { return }
        pollSilentSpeech()
        pollStalledVolatile()

        let completion = endpointer.completion
        let threshold = endpointer.silenceThreshold
        // Silence must not discard a trailing segment still being recognized.
        guard case let .send(utterance) = endpointer.poll(
            at: Date.timeIntervalSinceReferenceDate, awaitingFinal: volatileAt != nil
        )
        else { return }
        VoiceLog.note("turn (\(completion) after \(threshold)s): \(utterance)")

        lastTurnPeak = engine.levels.peak(within: 5)
        VoiceLog.note(String(format: "turn peak level %.3f", lastTurnPeak))
        volatileText = ""
        heardDraft = ""
        deliver(utterance)
    }

    /* The recognizer sometimes stops after volatile results without ever
       delivering the final, leaving the last partial standing forever. Once a
       volatile has sat past the silence threshold plus a margin, promote it to
       the final it never got; its real final, should it still come, is dropped
       as a duplicate. */
    private func pollStalledVolatile() {
        guard !volatileText.isEmpty, let since = volatileAt else { return }
        let now = Date.timeIntervalSinceReferenceDate
        guard now - since >= endpointer.silenceThreshold + Self.volatileStallTimeout
        else { return }
        let text = volatileText
        VoiceLog.problem("recognizer stalled; promoting volatile to final: \(text)")
        promotedFinal = text
        volatileAt = nil
        volatileText = ""
        switch endpointer.noteFinal(text, at: since) {
        case .cancelled:
            heardDraft = ""
        case .send, .waiting:
            updateHeardDraft()
        }
    }

    private func pollSilentSpeech() {
        if output?.sweepStalledRender() == true {
            silentSince = nil
            handlePlaybackDrained(echoCancelled: engine.isEchoCancelled)
            return
        }
        guard mode == .speaking || isSpeaking, !engine.isSpeaking,
              !(output?.hasPendingSpeech ?? false)
        else {
            silentSince = nil
            return
        }
        let now = Date.timeIntervalSinceReferenceDate
        guard let since = silentSince else {
            silentSince = now
            return
        }
        guard now - since >= Self.silentSpeechTimeout else { return }
        VoiceLog.problem("speaking with nothing to play (mode \(mode)); listening again")
        silentSince = nil
        handlePlaybackDrained(echoCancelled: engine.isEchoCancelled)
    }

    private var canDeliver: Bool {
        !isBusy && mode != .speaking && !engine.isSpeaking
    }

    private func deliver(_ utterance: String) {
        if VoiceTurnGate.sendImmediately(isBusy: isBusy) {
            VoiceLog.note("queued: \(utterance)")
            onSend?(utterance)
            return
        }
        guard canDeliver else {
            heldUtterance = [heldUtterance, utterance].compactMap { $0 }.joined(separator: " ")
            VoiceLog.note("held: \(utterance)")
            return
        }
        mode = .answering
        onSend?(utterance)
    }

    private func flushHeld() {
        guard canDeliver, let held = heldUtterance else { return }
        heldUtterance = nil
        deliver(held)
    }

    private static func words(_ text: String) -> [String] { EchoRejector.words(text) }

    static func isBargeIn(_ text: String) -> Bool {
        guard !TurnEndpointer.isNoise(text) else { return false }
        return EchoRejector.words(text).count >= bargeInWordCount
    }

    private func canInterrupt(isFinal: Bool) -> Bool {
        VoiceTurnGate.canInterrupt(
            isBusy: isBusy,
            mode: mode,
            isFinal: isFinal,
            echoCancelled: engine.isEchoCancelled
        )
    }

    @discardableResult
    private func considerInterrupting(
        _ text: String, isFinal: Bool, confidence: Double? = nil
    ) -> Bool {
        guard canInterrupt(isFinal: isFinal) else { return false }
        guard TurnEndpointer.isStopCommand(text) else { return false }
        guard isPlausiblyTheChild(text, confidence: confidence) else { return false }
        interrupt()
        return true
    }

    private func isPlausiblyTheChild(_ text: String, confidence: Double?) -> Bool {
        guard mode == .speaking else { return true }
        let level = engine.levels.peak(within: Self.bargeInLevelWindow)
        let floor = lastTurnPeak * Self.bargeInLevelShare
        let confidenceText = confidence.map { String(format: "%.2f", $0) } ?? "n/a"
        if lastTurnPeak > 0, level < floor {
            VoiceLog.note(String(
                format: "ignored while speaking: level %.3f below %.3f (confidence %@): %@",
                level, floor, confidenceText, text
            ))
            return false
        }
        if let confidence, confidence < Self.bargeInConfidence {
            VoiceLog.note(String(
                format: "ignored while speaking: confidence %@ (level %.3f): %@",
                confidenceText, level, text
            ))
            return false
        }
        VoiceLog.note(String(
            format: "heard while speaking: level %.3f (floor %.3f), confidence %@: %@",
            level, floor, confidenceText, text
        ))
        return true
    }

    private func interrupt() {
        VoiceLog.note("interrupted while \(mode)")
        output?.stop()
        chunker = SpokenTextChunker()
        echo.reset()
        heldUtterance = nil
        volatileText = ""
        heardDraft = ""
        rejectedEchoPrefix = nil

        gateUntil = engine.isEchoCancelled
            ? 0 : Date.timeIntervalSinceReferenceDate + Self.echoGate
        isSpeaking = false
        mode = .listening
        onInterrupt?()
    }

    public func stopSpeaking() {
        VoiceLog.note("speech stopped by button while \(mode)")
        output?.stop()
        chunker = SpokenTextChunker()
        isReplyMuted = true
        isSpeaking = false
        guard isConversing else { return }
        gateUntil = engine.isEchoCancelled
            ? 0 : Date.timeIntervalSinceReferenceDate + Self.echoGate
        if mode == .speaking { resumeListening() }
    }

    private func makeOutput() -> any VoiceOutput {
        let output = AppleVoiceOutput(
            engine: engine,
            voiceIdentifier: settings.voice,
            rate: rate,
            volume: volume
        )
        output.onSpoken = { [weak self] text in self?.echo.noteSpoken(text) }
        output.onMissingVoice = { [weak self] name in
            self?.onNotice?("voice \(name) is not installed; the reply was not spoken")
        }

        output.onFinished = { [weak self] in
            guard let self, !engine.isSpeaking else { return }
            handlePlaybackDrained(echoCancelled: false)
        }
        return output
    }

    public func appendReply(_ delta: String) {
        guard isConversing, !isReplyMuted else { return }
        let utterances = chunker.append(delta)
        VoiceLog.note("reply delta \(delta.count) chars -> \(utterances.count) utterances")
        guard !utterances.isEmpty else { return }
        speak(utterances)
    }

    public func finishReply() {
        guard isConversing, !isReplyMuted else {
            chunker = SpokenTextChunker()
            return
        }
        let utterances = chunker.finish()
        if !utterances.isEmpty { speak(utterances) }
        if isConversing, !engine.isSpeaking, mode != .speaking { resumeListening() }
    }

    private func speak(_ utterances: [String]) {
        ensureOutput()
        isSpeaking = true
        mode = .speaking
        for utterance in utterances {
            if utterance == SpokenTextChunker.paragraphBreak {
                output?.pause(Self.paragraphPause)
            } else {
                output?.speak(utterance)
            }
        }
    }

    public func cancelReply() {
        chunker = SpokenTextChunker()
        output?.stop()
        heldUtterance = nil
        isSpeaking = false
        if isConversing { resumeListening() }
    }

    public func announce(_ text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard isConversing, !trimmed.isEmpty else { return }
        ensureOutput()
        isSpeaking = true
        output?.speak(trimmed)
    }

    @discardableResult
    public func handoff() -> String? {
        let leftover = heardDraft.trimmingCharacters(in: .whitespacesAndNewlines)
        let held = heldUtterance?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        suppressCurrentTurn = !leftover.isEmpty
        chunker = SpokenTextChunker()
        output?.stop()
        heldUtterance = nil
        isBusy = false
        isReplyMuted = false
        isSpeaking = false
        endpointer.reset()
        volatileText = ""
        heardDraft = ""
        rejectedEchoPrefix = nil
        if isConversing { mode = .listening }
        let outgoing: String
        if leftover.isEmpty {
            outgoing = held
        } else if held.isEmpty {
            outgoing = leftover
        } else {
            outgoing = held + " " + leftover
        }
        if outgoing.isEmpty || TurnEndpointer.isStopCommand(outgoing) { return nil }
        return outgoing
    }

    private func ensureOutput() {
        guard output == nil else { return }
        output = makeOutput()
    }

    private func handlePlaybackDrained(echoCancelled: Bool) {

        let gate = echoCancelled ? Self.echoGate : Self.uncancelledEchoGate
        gateUntil = Date.timeIntervalSinceReferenceDate + gate
        isSpeaking = false
        guard isConversing, mode == .speaking else { return }
        resumeListening()
    }

    private func resumeListening() {
        rejectedEchoPrefix = nil
        if !endpointer.hasSpeech {
            volatileText = ""
            heardDraft = ""
        }
        mode = .listening
        flushHeld()
    }

    public func play(_ chime: VoiceChime) {
        guard isConversing else { pendingChime = chime; return }
        engine.play(chime)
    }

    public func setMic(_ on: Bool) {
        if on { startConversation() } else { stopConversation() }
    }

    public func setBusy(_ busy: Bool) {
        isBusy = busy
        if busy { isReplyMuted = false }
        flushHeld()
    }
}

public struct VoiceSettings: Sendable {
    public var voice: String?
    public var rate: Float
    public var silence: TimeInterval

    public var preferredInput: String?

    public var volume: Float

    public static let defaultVoice = "Jamie"

    public static let `default` = VoiceSettings(
        voice: VoiceSettings.defaultVoice,
        rate: AVSpeechUtteranceDefaultSpeechRate,
        silence: TurnEndpointer.defaultSilence,
        preferredInput: "AirPods",
        volume: 1
    )

    public init(voice: String?, rate: Float, silence: TimeInterval, preferredInput: String?,
                volume: Float = 1) {
        self.voice = voice
        self.rate = rate
        self.silence = silence
        self.preferredInput = preferredInput
        self.volume = min(max(volume, 0), 1)
    }
}
