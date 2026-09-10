import AVFoundation
import Foundation

/// Speaks with Apple's on-device voices, by whichever of two routes is right.
///
/// While the microphone is open, speech is synthesized to buffers and scheduled on the shared
/// `VoiceEngine`, because only audio rendered by that engine is cancelled from what the
/// microphone hears. Each utterance is rendered whole and scheduled in one go: a sentence
/// renders in a few milliseconds, and doing it that way keeps the buffers in order, which
/// hopping each one to the main actor as it arrived would not guarantee.
///
/// With the microphone closed there is nothing to cancel, so utterances go straight to the
/// synthesizer and out the ordinary system output. That is also the only route that works
/// then: the engine is not running, and reading replies aloud must not require it to be.
@MainActor
final class AppleVoiceOutput: VoiceOutput {
    /// Collects one utterance's buffers on whichever queue the synthesizer renders on. Only
    /// that queue touches it, and it is read on the main actor only after rendering has ended.
    private final class Render: @unchecked Sendable {
        var buffers: [AVAudioPCMBuffer] = []
    }

    var onSpoken: ((String) -> Void)?
    var onFinished: (() -> Void)?

    /// Kept apart because one synthesizer instance does not reliably serve both rendering to
    /// buffers and speaking to the output device.
    private let renderer = AVSpeechSynthesizer()
    private let speaker = AVSpeechSynthesizer()
    private unowned let engine: VoiceEngine
    private let voiceIdentifier: String?
    /// The resolved voice, held once found. Personal Voice is absent from `speechVoices()`
    /// until the user authorizes this bundle, which can land after this object is built, so
    /// a miss is retried on the next utterance instead of being cached as the fallback.
    private var resolvedVoice: AVSpeechSynthesisVoice?
    var rate: Float
    var volume: Float
    private let speakerDelegate = SpeakerDelegate()
    private enum Item {
        case text(String)
        case silence(TimeInterval)
    }
    private var queue: [Item] = []
    /// Silence owed before the next utterance on the direct route, which cannot play silence
    /// on its own and has to attach it to an utterance instead.
    private var owedDelay: TimeInterval = 0
    /// The breath between sentences on the engine route, where the synthesizer's own
    /// post-utterance delay does not apply because only the speech itself is rendered.
    static let sentenceGap: TimeInterval = 0.12
    private var isRendering = false
    private var generation = 0

    init(engine: VoiceEngine, voiceIdentifier: String?, rate: Float, volume: Float) {
        self.engine = engine
        self.voiceIdentifier = voiceIdentifier
        self.rate = rate
        self.volume = volume
        speaker.delegate = speakerDelegate
        speakerDelegate.onIdle = { [weak self] in self?.onFinished?() }
    }

    /// Reports when the direct route falls silent. The synthesizer's own `isSpeaking` is
    /// still true inside `didFinish`, so idleness is judged by whether another utterance is
    /// queued behind the one that ended.
    private final class SpeakerDelegate: NSObject, AVSpeechSynthesizerDelegate {
        var onIdle: (() -> Void)?
        private var pending = 0

        func note(_ utterance: AVSpeechUtterance) { pending += 1 }

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
        guard engine.isRunning, !isPersonalVoice else {
            // The synthesizer keeps its own queue and speaks in order, so with no engine to
            // schedule against there is nothing here to manage.
            let utterance = utterance(for: trimmed)
            utterance.preUtteranceDelay = owedDelay
            owedDelay = 0
            speakerDelegate.note(utterance)
            speaker.speak(utterance)
            onSpoken?(trimmed)
            return
        }
        queue.append(.text(trimmed))
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
        speaker.stopSpeaking(at: .immediate)
        engine.stopPlayback()
    }

    /// Resolves the configured voice, retrying while the named one is still missing.
    private func currentVoice() -> AVSpeechSynthesisVoice? {
        if let resolvedVoice { return resolvedVoice }
        guard let named = Self.namedVoice(matching: voiceIdentifier) else {
            let fallback = Self.bestEnglishVoice()
            VoiceLog.note("voice \(voiceIdentifier ?? "unset") not installed, using \(fallback?.name ?? "none")")
            return fallback
        }
        resolvedVoice = named
        VoiceLog.note("voice: \(named.name) (\(named.identifier), personal: \(named.voiceTraits.contains(.isPersonalVoice)))")
        return named
    }

    /// Personal Voice renders only to the output device: the synthesizer will not hand that
    /// audio to an app, so `write(_:toBufferCallback:)` yields nothing and the engine route
    /// is silent. Speak it directly and give up echo cancellation for it.
    private var isPersonalVoice: Bool {
        currentVoice()?.voiceTraits.contains(.isPersonalVoice) ?? false
    }

    private func utterance(for text: String) -> AVSpeechUtterance {
        let utterance = AVSpeechUtterance(string: text)
        utterance.voice = currentVoice()
        utterance.rate = rate
        utterance.volume = volume
        // A beat between sentences; without it a streamed reply runs together into one breath.
        utterance.postUtteranceDelay = 0.12
        return utterance
    }

    private func pump() {
        guard !isRendering, !queue.isEmpty else { return }
        let text: String
        switch queue.removeFirst() {
        case let .silence(duration):
            engine.schedule([Self.silence(duration)].compactMap { $0 })
            pump()
            return
        case let .text(queued):
            text = queued
        }
        isRendering = true
        let expected = generation

        let render = Render()
        // `@Sendable` for the same reason as the audio tap: the synthesizer renders on its own
        // queue, and an inherited main-actor isolation check would trap there. The box is what
        // crosses back, so no buffer is passed across on its own.
        renderer.write(utterance(for: text)) { @Sendable [weak self] buffer in
            guard let pcm = buffer as? AVAudioPCMBuffer else { return }
            guard pcm.frameLength > 0 else {
                Task { @MainActor [weak self] in
                    self?.finish(text: text, render: render, expected: expected)
                }
                return
            }
            render.buffers.append(pcm)
        }
    }

    private func finish(text: String, render: Render, expected: Int) {
        isRendering = false
        let buffers = render.buffers
        render.buffers = []
        // A stop landed while this was rendering; its audio belongs to a turn that is over.
        guard expected == generation else { return }
        engine.schedule(buffers + [Self.silence(Self.sentenceGap, like: buffers.last)].compactMap { $0 })
        onSpoken?(text)
        pump()
    }

    /// A buffer of silence, in the format of the audio it follows so the two join into one.
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

    // MARK: - voices

    /// The best English voice installed. Premium and enhanced voices are separate downloads,
    /// so this degrades to the compact system voice rather than failing when they are absent.
    static func voice(matching identifier: String?) -> AVSpeechSynthesisVoice? {
        namedVoice(matching: identifier) ?? bestEnglishVoice()
    }

    /// The configured voice, or nil when nothing installed matches it.
    static func namedVoice(matching identifier: String?) -> AVSpeechSynthesisVoice? {
        guard let identifier, !identifier.isEmpty else { return nil }
        if let exact = AVSpeechSynthesisVoice(identifier: identifier) { return exact }
        // Voices are named "Jamie (Premium)" and "Jamie (Enhanced)", and the same name can
        // be installed at several qualities. Take the best one that matches.
        return AVSpeechSynthesisVoice.speechVoices()
            .filter { $0.name.localizedCaseInsensitiveContains(identifier) }
            .max { $0.quality.rawValue < $1.quality.rawValue }
    }

    static func bestEnglishVoice() -> AVSpeechSynthesisVoice? {
        let english = AVSpeechSynthesisVoice.speechVoices()
            .filter { $0.language.hasPrefix("en") }
        guard !english.isEmpty else { return nil }
        // Prefer the child's own locale within English, so a British voice is not chosen for a
        // American child purely because it happens to be the higher quality download.
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
