import AVFoundation
import Foundation

@MainActor
final class AppleVoiceOutput: VoiceOutput {

    private final class Render: @unchecked Sendable {
        var buffers: [AVAudioPCMBuffer] = []
    }

    var onSpoken: ((String) -> Void)?
    var onFinished: (() -> Void)?

    private let renderer = AVSpeechSynthesizer()
    private let speaker = AVSpeechSynthesizer()
    private unowned let engine: VoiceEngine
    private let voiceIdentifier: String?

    private var resolvedVoice: AVSpeechSynthesisVoice?
    var rate: Float
    var volume: Float
    private let speakerDelegate = SpeakerDelegate()
    private enum Item {
        case text(AVSpeechUtterance)
        case silence(TimeInterval)
    }
    private var queue: [Item] = []

    private var owedDelay: TimeInterval = 0

    static let sentenceGap: TimeInterval = 0.12

    static let directUtteranceLimit = 120
    private var isRendering = false
    private var renderDeadline: TimeInterval?
    private var didRender = false
    /* a render lands in tens of milliseconds once the voice is loaded; the
       first one also pays for loading it */
    static let renderTimeout: TimeInterval = 1
    static let firstRenderTimeout: TimeInterval = 5
    private var generation = 0

    var onMissingVoice: ((String) -> Void)?
    private var reportedMissingVoice = false

    init(engine: VoiceEngine, voiceIdentifier: String?, rate: Float, volume: Float) {
        self.engine = engine
        self.voiceIdentifier = voiceIdentifier
        self.rate = rate
        self.volume = volume
        speaker.delegate = speakerDelegate
        speakerDelegate.onIdle = { [weak self] in self?.onFinished?() }
    }

    private final class SpeakerDelegate: NSObject, AVSpeechSynthesizerDelegate {
        var onIdle: (() -> Void)?
        private(set) var pending = 0

        func note(_ utterance: AVSpeechUtterance) {
            pending += 1
            queuedAt[ObjectIdentifier(utterance)] = ContinuousClock.now
        }

        private var queuedAt: [ObjectIdentifier: ContinuousClock.Instant] = [:]

        func speechSynthesizer(
            _ synthesizer: AVSpeechSynthesizer, didStart utterance: AVSpeechUtterance
        ) {
            guard let queued = queuedAt.removeValue(forKey: ObjectIdentifier(utterance)) else { return }
            VoiceLog.note("speech started \(queued.elapsedMilliseconds)ms after queueing, \(utterance.speechString.count) chars")
        }

        func reset() { pending = 0 }

        private func finished() {
            pending = max(0, pending - 1)
            guard pending == 0 else { return }
            let onIdle = onIdle
            Task { @MainActor in onIdle?() }
        }

        func speechSynthesizer(
            _ synthesizer: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance
        ) { finished() }

        func speechSynthesizer(
            _ synthesizer: AVSpeechSynthesizer, didCancel utterance: AVSpeechUtterance
        ) { finished() }
    }

    func speak(_ text: String) {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        guard currentVoice() != nil else {
            guard !reportedMissingVoice else { return }
            reportedMissingVoice = true
            onMissingVoice?(voiceIdentifier ?? "")
            return
        }
        guard engine.isRunning, !isPersonalVoice else {

            let parts = SpokenTextChunker.clamped(trimmed, limit: Self.directUtteranceLimit)
            for (index, part) in parts.enumerated() {
                let utterance = utterance(for: part)

                if index < parts.count - 1 { utterance.postUtteranceDelay = 0 }
                utterance.preUtteranceDelay = owedDelay
                owedDelay = 0
                speakerDelegate.note(utterance)
                VoiceLog.note("queueing utterance \(index + 1)/\(parts.count), \(part.count) chars")
                speaker.speak(utterance)
                onSpoken?(part)
            }
            return
        }
        queue.append(.text(utterance(for: trimmed)))
        pump()
    }

    func pause(_ duration: TimeInterval) {
        guard duration > 0 else { return }
        guard engine.isRunning, !isPersonalVoice else {
            owedDelay += duration
            return
        }
        queue.append(.silence(duration))
        pump()
    }

    func stop() {
        generation &+= 1
        queue = []
        owedDelay = 0
        isRendering = false
        renderDeadline = nil
        speaker.stopSpeaking(at: .immediate)
        speakerDelegate.reset()
        engine.stopPlayback()
    }

    var hasPendingSpeech: Bool {
        isRendering || !queue.isEmpty || speakerDelegate.pending > 0 || speaker.isSpeaking
    }

    /* A render whose completion never arrives pins hasPendingSpeech, and the
       controller's silent-speech watchdog waits on that: the reply is never
       heard and listening never resumes. Drop a render that is past due so the
       watchdog can see an idle output again. */
    func sweepStalledRender() -> Bool {
        guard isRendering, let deadline = renderDeadline,
              Date.timeIntervalSinceReferenceDate >= deadline
        else { return false }
        VoiceLog.problem("speech render stalled; dropping it")
        generation &+= 1
        isRendering = false
        renderDeadline = nil
        queue = []
        owedDelay = 0
        return true
    }

    private func currentVoice() -> AVSpeechSynthesisVoice? {
        if let resolvedVoice { return resolvedVoice }
        guard let named = Self.namedVoice(matching: voiceIdentifier) else {

            guard let voiceIdentifier, !voiceIdentifier.isEmpty else {
                return Self.bestEnglishVoice()
            }
            VoiceLog.note("voice \(voiceIdentifier) not installed")
            return nil
        }
        resolvedVoice = named
        VoiceLog.note("voice: \(named.name) (\(named.identifier), personal: \(named.voiceTraits.contains(.isPersonalVoice)))")
        return named
    }

    private var isPersonalVoice: Bool {
        currentVoice()?.voiceTraits.contains(.isPersonalVoice) ?? false
    }

    private func utterance(for text: String) -> AVSpeechUtterance {
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = currentVoice()
        utterance.rate = rate
        utterance.volume = volume

        utterance.postUtteranceDelay = 0.12
        return utterance
    }

    private func pump() {
        guard !isRendering, !queue.isEmpty else { return }
        let utterance: AVSpeechUtterance
        switch queue.removeFirst() {
        case let .silence(duration):
            engine.schedule([Self.silence(duration)].compactMap { $0 })
            pump()
            return
        case let .text(queued):
            utterance = queued
        }
        isRendering = true
        renderDeadline = Date.timeIntervalSinceReferenceDate +
            (didRender ? Self.renderTimeout : Self.firstRenderTimeout)
        let expected = generation
        let clock = ContinuousClock.now

        let render = Render()

        let text = utterance.speechString
        renderer.write(utterance) { @Sendable [weak self] buffer in
            guard let pcm = buffer as? AVAudioPCMBuffer else { return }
            guard pcm.frameLength > 0 else {
                Task { @MainActor [weak self] in
                    VoiceLog.note("rendered \(text.count) chars in \(clock.elapsedMilliseconds)ms")
                    self?.finish(text: text, render: render, expected: expected)
                }
                return
            }
            render.buffers.append(pcm)
        }
    }

    private func finish(text: String, render: Render, expected: Int) {
        isRendering = false
        renderDeadline = nil
        didRender = true
        let buffers = render.buffers
        render.buffers = []

        guard expected == generation else { return }
        engine.schedule(buffers + [Self.silence(Self.sentenceGap, like: buffers.last)].compactMap { $0 })
        onSpoken?(text)
        pump()
    }

    private static func silence(
        _ duration: TimeInterval, like reference: AVAudioPCMBuffer? = nil
    ) -> AVAudioPCMBuffer? {
        let format = reference?.format
            ?? AVAudioFormat(standardFormatWithSampleRate: 24000, channels: 1)
        guard let format,
              let buffer = AVAudioPCMBuffer(
                  pcmFormat: format,
                  frameCapacity: AVAudioFrameCount(duration * format.sampleRate)
              )
        else { return nil }
        buffer.frameLength = buffer.frameCapacity
        return buffer
    }

    static func voice(matching identifier: String?) -> AVSpeechSynthesisVoice? {
        namedVoice(matching: identifier) ?? bestEnglishVoice()
    }

    static func namedVoice(matching identifier: String?) -> AVSpeechSynthesisVoice? {
        guard let identifier, !identifier.isEmpty else { return nil }
        if let exact = AVSpeechSynthesisVoice(identifier: identifier) { return exact }

        return AVSpeechSynthesisVoice.speechVoices()
            .filter { $0.name.localizedCaseInsensitiveContains(identifier) }
            .max { $0.quality.rawValue < $1.quality.rawValue }
    }

    static func bestEnglishVoice() -> AVSpeechSynthesisVoice? {
        let english = AVSpeechSynthesisVoice.speechVoices()
            .filter { $0.language.hasPrefix("en") }
        guard !english.isEmpty else { return nil }

        let preferredLanguage = Locale.current.identifier.replacingOccurrences(of: "_", with: "-")
        func rank(_ voice: AVSpeechSynthesisVoice) -> Int {
            let quality: Int
            switch voice.quality {
            case .premium: quality = 3
            case .enhanced: quality = 2
            default: quality = 1
            }
            return quality * 2 + (preferredLanguage.hasPrefix(voice.language) ? 1 : 0)
        }
        return english.max { rank($0) < rank($1) }
    }

    static func availableEnglishVoices() -> [AVSpeechSynthesisVoice] {
        AVSpeechSynthesisVoice.speechVoices()
            .filter { $0.language.hasPrefix("en") }
            .sorted { $0.name < $1.name }
    }
}
