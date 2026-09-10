import AVFoundation
import Foundation
import Speech

enum VoiceInputEvent: Sendable {
    /// The running hypothesis for the segment being spoken. Never committed — it only proves
    /// the child is still talking.
    case volatile(String)
    /// One segment the recognizer has committed to, at a pause. `confidence` is the
    /// recognizer's own, averaged over the words, or nil when it did not report one.
    case final(String, confidence: Double?)
    case failed(String)
}

@MainActor
protocol VoiceInput: AnyObject {
    /// The audio format this recognizer wants, decided before the engine starts because the
    /// microphone tap has to convert into it.
    func prepare() async throws -> AVAudioFormat
    /// Consumes microphone audio until the stream ends or `stop()` is called.
    func run(
        buffers: AsyncStream<VoiceAudioBuffer>,
        onEvent: @escaping @MainActor (VoiceInputEvent) -> Void
    ) async
    func stop()
}

enum VoiceInputError: LocalizedError {
    case unsupported
    case notAuthorized
    case noModel(String)

    var errorDescription: String? {
        switch self {
        case .unsupported:
            return "Talking out loud needs a newer version of macOS on this Mac."
        case .notAuthorized:
            return "Primer needs permission to use the microphone and speech recognition."
        case let .noModel(language):
            return "No speech model is available for \(language)."
        }
    }
}

/// Asks for the microphone and speech-recognition grants. Primer is a signed, bundled app with
/// its own identity, so it can hold these itself — voiceclaude needed a separate helper app
/// only because processes started inside tmux are denied them.
enum VoicePermissions {
    static func request() async -> Bool {
        let speech = await withCheckedContinuation { continuation in
            SFSpeechRecognizer.requestAuthorization { status in
                VoiceLog.note("speech authorization status: \(status.rawValue) (3 = authorized)")
                continuation.resume(returning: status == .authorized)
            }
        }
        guard speech else { return false }
        let microphone = await AVCaptureDevice.requestAccess(for: .audio)
        VoiceLog.note("microphone granted: \(microphone)")
        return microphone
    }
}

/// On-device continuous transcription through `SpeechAnalyzer`.
///
/// Chosen over `SFSpeechRecognizer` for accuracy, for having no session length cap, and for
/// endpointing on pauses by itself — all three matter when the microphone stays open for a
/// whole conversation rather than for one held button.
@available(macOS 26.0, *)
@MainActor
final class AnalyzerVoiceInput: VoiceInput {
    private let locale: Locale
    private var transcriber: SpeechTranscriber?
    private var analyzer: SpeechAnalyzer?
    private var inputBuilder: AsyncStream<AnalyzerInput>.Continuation?
    private var resultsTask: Task<Void, Never>?

    init(locale: Locale = Locale(identifier: "en-US")) {
        self.locale = locale
    }

    func prepare() async throws -> AVAudioFormat {
        let transcriber = SpeechTranscriber(
            locale: locale,
            transcriptionOptions: [],
            reportingOptions: [.volatileResults, .fastResults],
            attributeOptions: [.transcriptionConfidence]
        )
        self.transcriber = transcriber

        // The model is a one-time, system-managed download on first use.
        let installed = await Self.isInstalled(locale)
        VoiceLog.note("transcriber built; model installed for \(locale.identifier): \(installed)")
        if !installed {
            VoiceLog.note("downloading speech model (first run)…")
            guard let request = try await AssetInventory.assetInstallationRequest(
                supporting: [transcriber]
            ) else {
                throw VoiceInputError.noModel(locale.identifier)
            }
            try await request.downloadAndInstall()
            VoiceLog.note("speech model installed")
        }

        guard let format = await SpeechAnalyzer.bestAvailableAudioFormat(
            compatibleWith: [transcriber]
        ) else {
            throw VoiceInputError.noModel(locale.identifier)
        }
        return format
    }

    func run(
        buffers: AsyncStream<VoiceAudioBuffer>,
        onEvent: @escaping @MainActor (VoiceInputEvent) -> Void
    ) async {
        guard let transcriber else {
            onEvent(.failed(VoiceInputError.unsupported.localizedDescription))
            return
        }
        let (inputSequence, inputBuilder) = AsyncStream<AnalyzerInput>.makeStream()
        self.inputBuilder = inputBuilder

        // Results are consumed before audio is fed in, so nothing said at the very start of a
        // session is missed.
        resultsTask = Task { @MainActor in
            do {
                for try await result in transcriber.results {
                    let text = String(result.text.characters)
                        .trimmingCharacters(in: .whitespaces)
                    guard !text.isEmpty else { continue }
                    guard result.isFinal else {
                        VoiceLog.note("result volatile: \(text)")
                        onEvent(.volatile(text))
                        continue
                    }
                    let confidence = Self.confidence(of: result.text)
                    VoiceLog.note(
                        "result final (confidence \(confidence.map { String(format: "%.2f", $0) } ?? "n/a")): \(text)"
                    )
                    onEvent(.final(text, confidence: confidence))
                }
            } catch {
                onEvent(.failed(error.localizedDescription))
            }
        }

        let analyzer = SpeechAnalyzer(modules: [transcriber])
        self.analyzer = analyzer
        do {
            try await analyzer.start(inputSequence: inputSequence)
            VoiceLog.note("analyzer started; feeding audio")
        } catch {
            VoiceLog.problem("analyzer start failed: \(error)")
            onEvent(.failed(error.localizedDescription))
            return
        }

        var fed = 0
        for await chunk in buffers {
            inputBuilder.yield(AnalyzerInput(buffer: chunk.buffer))
            fed += 1
            if fed % 200 == 0 { VoiceLog.note("fed \(fed) buffers to the analyzer") }
        }
        VoiceLog.note("audio stream ended after \(fed) buffers")
        inputBuilder.finish()
    }

    func stop() {
        inputBuilder?.finish()
        inputBuilder = nil
        resultsTask?.cancel()
        resultsTask = nil
        let analyzer = self.analyzer
        self.analyzer = nil
        transcriber = nil
        Task { try? await analyzer?.finalizeAndFinishThroughEndOfInput() }
    }

    /// The mean of the per-run confidences the transcriber attached, weighted by length.
    private static func confidence(of text: AttributedString) -> Double? {
        var total = 0.0
        var weight = 0
        for run in text.runs {
            guard let value = run.transcriptionConfidence else { continue }
            let length = text[run.range].characters.count
            total += value * Double(length)
            weight += length
        }
        return weight > 0 ? total / Double(weight) : nil
    }

    private static func isInstalled(_ locale: Locale) async -> Bool {
        let wanted = locale.language.languageCode?.identifier
        for installed in await SpeechTranscriber.installedLocales {
            if installed.language.languageCode?.identifier == wanted { return true }
        }
        return false
    }
}
