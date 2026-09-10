import AVFoundation
import Foundation

/// The state machine behind the microphone button.
///
/// It owns the audio engine, the recognizer, the voice, and the three pure pieces that decide
/// what counts as a turn. Everything above it — `ChatModel`, the composer — only sees a mode
/// being switched on, text arriving, and replies being fed in as they stream.
@MainActor
public final class VoiceController {
    public enum Mode: Equatable, Sendable {
        /// Nothing is listening and nothing is spoken.
        case idle
        /// Bringing up the engine, or downloading the speech model on first run.
        case starting
        case listening
        /// The child's turn has been sent and the Primer is answering.
        case answering
        case speaking
    }

    /// How long after the last spoken audio drains before the microphone is trusted again.
    /// Voice processing is subtraction rather than cancellation and its tail outlives the
    /// audio; a shorter gate lets the end of a sentence come back as a turn.
    static let echoGate: TimeInterval = 0.7
    /// The same wait when the engine could not bring voice processing up at all.
    static let uncancelledEchoGate: TimeInterval = 1.5
    /// How often the silence clock is checked. Well below the silence threshold it feeds.
    static let pollInterval = Duration.milliseconds(150)
    /// Speech heard while the reply is playing needs more than one word before it is kept.
    /// A single leaked or misheard token must not become a queued turn.
    static let bargeInWordCount = 2
    /// The echo floor while the Primer is speaking. Low, because everything heard then is
    /// suspect, and a leak of two or three words is common.
    static let echoWordsWhileSpeaking = 2
    /// Below this confidence, a segment finalized while the Primer is speaking is taken for
    /// what the recognizer makes of cancellation residue rather than for the child.
    static let bargeInConfidence = 0.5
    /// How loud the microphone has to be, as a share of the child's last real turn, before
    /// something heard while the Primer is speaking is believed. What cancellation leaves of
    /// the reply is far quieter than a child in the room.
    static let bargeInLevelShare: Float = 0.25
    /// The window the level is judged over: long enough to cover the words just recognized.
    static let bargeInLevelWindow: TimeInterval = 1.5
    /// The breath at a paragraph break, on top of the gap between sentences.
    static let paragraphPause: TimeInterval = 0.5
    /// How long a reply may stand with no audio outstanding before it is taken for a lost
    /// completion. Long enough to cover the handoff between two utterances of a reply.
    static let silentSpeechTimeout: TimeInterval = 0.5

    public private(set) var mode: Mode = .idle {
        didSet { if mode != oldValue { onMode?(mode) } }
    }
    /// True while the child is in a conversation. The latch, not a preference: an open
    /// microphone is bounded by a mode they deliberately entered.
    public private(set) var isConversing = false
    /// What the child has said so far this turn, shown in the composer as it forms. It is the
    /// segments the recognizer has committed to plus its running guess at the one being spoken
    /// now, so words appear while they are being said rather than when the turn ends.
    public private(set) var heardDraft = "" {
        didSet { if heardDraft != oldValue { onHeard?(heardDraft) } }
    }
    /// True while a reply is being read aloud.
    public private(set) var isSpeaking = false {
        didSet { if isSpeaking != oldValue { onSpeaking?(isSpeaking) } }
    }
    public var errorMessage: String? {
        didSet { if let errorMessage { onError?(errorMessage) } }
    }

    /// Called with a finished turn. `ChatModel` sends it.
    public var onSend: ((String) -> Void)?
    /// Called when the user said the stop word. The client abandons the turn in flight.
    public var onInterrupt: (() -> Void)?
    public var onMode: ((Mode) -> Void)?
    public var onHeard: ((String) -> Void)?
    public var onSpeaking: ((Bool) -> Void)?
    public var onError: ((String) -> Void)?

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
    /// The recognizer's hypothesis for the segment in progress. Held separately from the
    /// endpointer's committed segments because it is replaced wholesale on every update and
    /// must never become part of the turn that is sent.
    private var volatileText = ""
    private var gateUntil: TimeInterval = 0
    private var isBusy = false
    private var heldUtterance: String?
    /// Set by the stop button: the rest of this reply stays silent as it streams in.
    private var isReplyMuted = false
    /// A hypothesis judged to be the Primer's own voice. The recognizer extends a segment word
    /// by word and gets the echo's first words right more often than its later ones, so once
    /// a prefix has been rejected everything that grows from it is rejected too — otherwise
    /// each extension is another roll of the dice, and one pass is all a leak needs.
    private var rejectedEchoPrefix: String?
    /// A focus handoff in the middle of an analyzer segment must not give its tail to the new
    /// client. Ignore that segment through its final result, then start cleanly.
    private var suppressCurrentTurn = false
    /// How loud the microphone peaked during the last turn that was sent.
    private var lastTurnPeak: Float = 0
    /// When `.speaking` was first seen with nothing left to play. Audio can be dropped before
    /// it is scheduled — the engine restarting under a route change, a conversion that fails,
    /// a synthesizer callback that never arrives — and every one of those loses the signal
    /// that ends the reply, leaving the microphone shut for the rest of the conversation.
    private var silentSince: TimeInterval?

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

    /// Takes effect on the next utterance; one already being spoken keeps the rate it started
    /// with, because AVSpeechUtterance reads its rate once.
    public func setRate(_ rate: Float) {
        self.rate = min(max(rate, 0), 1)
        output?.rate = self.rate
    }

    public var isAvailable: Bool {
        if #available(macOS 26.0, *) { return true }
        return false
    }

    // MARK: - conversation mode

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
        heardDraft = ""
        heldUtterance = nil
        suppressCurrentTurn = false
        isSpeaking = false
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
        startPolling()
        engine.play(.listening)
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

    /// A route change — AirPods sleeping, a case opening, another device taking them — moves
    /// the formats under the graph. Rebuilding is more reliable than patching it up.
    private func handleRouteChange() {
        guard isConversing else { return }
        stopConversation()
        isConversing = true
        mode = .starting
        runTask = Task { await run() }
    }

    // MARK: - hearing

    private func handle(_ event: VoiceInputEvent) {
        switch event {
        case let .volatile(text):
            guard !suppressCurrentTurn else { return }
            let now = Date.timeIntervalSinceReferenceDate
            if considerInterrupting(text, isFinal: false) { return }
            guard hearable(text, isFinal: false, at: now) else { return }
            endpointer.noteVolatile(text, at: now)
            volatileText = text
            updateHeardDraft()
        case let .final(text, confidence):
            if suppressCurrentTurn {
                suppressCurrentTurn = false
                endpointer.reset()
                volatileText = ""
                heardDraft = ""
                return
            }
            let now = Date.timeIntervalSinceReferenceDate
            if considerInterrupting(text, isFinal: true, confidence: confidence) { return }
            if TurnEndpointer.isStopCommand(text) {
                VoiceLog.note("stop command; nothing sent")
                endpointer.reset()
                heardDraft = ""
                return
            }
            guard hearable(text, isFinal: true, at: now, confidence: confidence) else { return }
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

    /// True when this hypothesis is the user, not the reply leaking back.
    private func hearable(
        _ text: String, isFinal: Bool, at now: TimeInterval, confidence: Double? = nil
    ) -> Bool {
        if isGated(at: now) {
            VoiceLog.note("\(isFinal ? "final" : "volatile") gated (mode \(mode)): \(text)")
            return false
        }
        if let prefix = rejectedEchoPrefix, Self.words(text).starts(with: Self.words(prefix)) {
            VoiceLog.note("ignored own voice (grown from rejected prefix): \(text)")
            return false
        }
        let speaking = replyInTheAir
        let floor = speaking ? Self.echoWordsWhileSpeaking : nil
        let ratio = speaking ? EchoRejector.matchRatioWhileSpeaking : EchoRejector.matchRatio
        if echo.shouldReject(text, minimumWords: floor, ratio: ratio) {
            VoiceLog.note("\(isFinal ? "final" : "volatile") rejected as echo: \(text)")
            if speaking { rejectedEchoPrefix = text }
            return false
        }
        if speaking, !isPlausiblyTheChild(text, confidence: confidence) {
            return false
        }
        // One leaked token must not start a queued turn while the reply is in the air.
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
        // Read before polling: a send resets the endpointer, and what it thought of the turn
        // goes with it.
        let completion = endpointer.completion
        let threshold = endpointer.silenceThreshold
        guard case let .send(utterance) = endpointer.poll(at: Date.timeIntervalSinceReferenceDate)
        else { return }
        VoiceLog.note("turn (\(completion) after \(threshold)s): \(utterance)")
        // The loudest the child was over this turn and its trailing silence, as the yardstick
        // for whether what is heard during the reply is them.
        lastTurnPeak = engine.levels.peak(within: 5)
        VoiceLog.note(String(format: "turn peak level %.3f", lastTurnPeak))
        volatileText = ""
        heardDraft = ""
        deliver(utterance)
    }

    /// Recovers from a reply whose end was never reported.
    private func pollSilentSpeech() {
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

    /// Holds a turn back while a reply is still in flight, rather than interrupting it.
    /// A busy client has already said it will queue, so the utterance is sent now.
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
        engine.play(.sent)
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

    /// Whether a stop word right now should take the floor.
    ///
    /// A busy client is mid-turn even while this side is still listening, so stop aborts it.
    /// While it is answering but not yet speaking, nothing is playing, so anything heard is
    /// certainly the child. While it is speaking, a running hypothesis is only trusted with
    /// echo cancellation — otherwise a leaked "stop" would cancel the reply on its own first
    /// word. Without it, only finalized segments count.
    private func canInterrupt(isFinal: Bool) -> Bool {
        VoiceTurnGate.canInterrupt(
            isBusy: isBusy,
            mode: mode,
            isFinal: isFinal,
            echoCancelled: engine.isEchoCancelled
        )
    }

    /// Stops the reply only on a stop word. Other speech is queued behind it.
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

    /// While the Primer is speaking, what cancellation leaves of its own voice is still audio,
    /// and the recognizer will make sentences out of it — sentences that match nothing it
    /// said, so text cannot catch them. Two things can: the recognizer's own confidence in
    /// them, which is low, and the microphone level, which is far below a child in the room.
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

    /// The stop word. Everything about the turn in flight is abandoned — the audio, the
    /// sentences still queued, and the model round still generating.
    private func interrupt() {
        VoiceLog.note("interrupted while \(mode)")
        output?.stop()
        chunker = SpokenTextChunker()
        echo.reset()
        heldUtterance = nil
        volatileText = ""
        heardDraft = ""
        rejectedEchoPrefix = nil
        // With cancellation the same words that stopped the reply can begin the next turn.
        // Without it the tail of the reply is still being finalized and must not become one.
        gateUntil = engine.isEchoCancelled
            ? 0 : Date.timeIntervalSinceReferenceDate + Self.echoGate
        isSpeaking = false
        mode = .listening
        engine.play(.interrupted)
        onInterrupt?()
    }

    /// The stop button. Silences the rest of this reply without abandoning it: the text keeps
    /// streaming in, and in a conversation the microphone opens again for the next turn.
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

    // MARK: - speaking

    private func makeOutput() -> any VoiceOutput {
        let output = AppleVoiceOutput(
            engine: engine,
            voiceIdentifier: settings.voice,
            rate: rate,
            volume: volume
        )
        output.onSpoken = { [weak self] text in self?.echo.noteSpoken(text) }
        // The direct route has no engine playback to drain, so the synthesizer going idle is
        // the only signal that the reply is over. Without it the mode never leaves .speaking
        // and every later turn is held instead of sent. Nothing cancels this audio, so it
        // takes the longer gate.
        output.onFinished = { [weak self] in
            guard let self, !engine.isSpeaking else { return }
            handlePlaybackDrained(echoCancelled: false)
        }
        return output
    }

    /// The reply as it streams. Sentences are spoken the moment they are complete, so the
    /// Primer starts talking about a sentence behind the model rather than a whole answer
    /// behind it.
    public func appendReply(_ delta: String) {
        guard isConversing, !isReplyMuted else { return }
        let utterances = chunker.append(delta)
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

    /// Called when a turn is abandoned — cancelled, or failed. Nothing half-spoken should
    /// carry into the next one.
    public func cancelReply() {
        chunker = SpokenTextChunker()
        output?.stop()
        heldUtterance = nil
        isSpeaking = false
        if isConversing { resumeListening() }
    }

    /// A short cue from a client that is not focused: it does not take the microphone.
    public func announce(_ text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard isConversing, !trimmed.isEmpty else { return }
        ensureOutput()
        isSpeaking = true
        output?.speak(trimmed)
    }

    /// A different client took ownership of a shared helper. Drop both sides of the old
    /// exchange so a partial utterance or an unfinished reply cannot cross that boundary.
    /// Returns in-progress speech so the departing client can send it rather than lose it.
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
        // With no cancellation the tail of the reply is still in the room, and the recognizer
        // is still finalizing segments from it. Wait longer before trusting the microphone.
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

    /// Mirrors a streaming turn: stop aborts it, and other speech is sent so the client
    /// can queue it rather than interrupting.
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
    /// Name substring of the input device to prefer, so a headset is used even when it is
    /// not the system default input.
    public var preferredInput: String?
    /// 0 is silence, 1 is full. Applies to spoken replies, not the chime.
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
