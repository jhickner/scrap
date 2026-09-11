import Foundation

struct TurnEndpointer: Sendable {

    static let defaultSilence: TimeInterval = 1.4

    static let fillers: Set<String> = [
        "um", "uh", "hmm", "hm", "mm", "mhm", "er", "ah", "oh", "eh", "the", "a", "and", "so", "you",
    ]

    static let stopPhrases: Set<String> = [
        "stop", "wait", "quiet", "be quiet", "shush", "shh", "hush", "stop talking",
        "stop it", "enough", "thats enough", "shut up", "silence", "hold on", "one second",
    ]

    static let cancelPhrases = [
        "scratch that", "never mind", "nevermind", "forget it", "cancel that", "start over",
    ]

    enum Decision: Equatable, Sendable {
        case waiting

        case cancelled
        case send(String)
    }

    var silence: TimeInterval
    private(set) var segments: [String] = []
    private var lastActivity: TimeInterval?

    init(silence: TimeInterval = TurnEndpointer.defaultSilence) {
        self.silence = silence
    }

    var draft: String { segments.joined(separator: " ") }

    var completion: UtteranceCompletion { UtteranceClassifier.classify(draft) }
    var silenceThreshold: TimeInterval { silence * completion.silenceFactor }
    var hasSpeech: Bool { !segments.isEmpty }

    mutating func noteVolatile(_ text: String, at time: TimeInterval) {
        guard !Self.isNoise(text) else { return }
        lastActivity = time
    }

    mutating func noteFinal(_ text: String, at time: TimeInterval) -> Decision {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !Self.isNoise(trimmed) else { return .waiting }
        lastActivity = time
        if Self.isCancel(trimmed) {
            reset()
            return .cancelled
        }
        segments.append(trimmed)
        return .waiting
    }

    mutating func poll(at time: TimeInterval, awaitingFinal: Bool = false) -> Decision {
        guard !awaitingFinal else { return .waiting }
        guard !segments.isEmpty, let lastActivity else { return .waiting }
        guard time - lastActivity >= silenceThreshold else { return .waiting }
        let utterance = draft
        reset()
        return .send(utterance)
    }

    mutating func reset() {
        segments = []
        lastActivity = nil
    }

    static func isNoise(_ text: String) -> Bool {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return true }
        let words = trimmed
            .lowercased()
            .split { !$0.isLetter && !$0.isNumber }
            .map(String.init)
        guard !words.isEmpty else { return true }

        if trimmed.hasPrefix("[") || trimmed.hasPrefix("(") { return true }
        return words.allSatisfy { fillers.contains($0) }
    }

    static func isStopCommand(_ text: String) -> Bool {
        isCommand(text, in: stopPhrases)
    }

    static let pausePhrases: Set<String> = [
        "pause", "pause listening", "resume", "resume listening",
    ]

    /* the client's pause and resume words, which it acts on from the turn text */
    static func isPauseCommand(_ text: String) -> Bool {
        isCommand(text, in: pausePhrases)
    }

    private static func isCommand(_ text: String, in stopPhrases: Set<String>) -> Bool {
        var words = normalized(text).split(separator: " ").map(String.init)

        while let first = words.first, stopPadding.contains(first) { words.removeFirst() }
        while let last = words.last, stopPadding.contains(last) { words.removeLast() }
        let phrase = words.joined(separator: " ")
        if stopPhrases.contains(phrase) { return true }

        return stopPhrases.contains { stop in
            phrase == Array(repeating: stop, count: max(1, words.count / max(1, stop.split(separator: " ").count)))
                .joined(separator: " ")
        }
    }

    static let stopPadding: Set<String> = [
        "okay", "ok", "please", "now", "hey", "um", "uh", "just", "primer",
    ]

    static func normalized(_ text: String) -> String {

        text
            .lowercased()
            .replacingOccurrences(of: "'", with: "")
            .replacingOccurrences(of: "\u{2019}", with: "")
            .split { !$0.isLetter && !$0.isNumber }
            .joined(separator: " ")
    }

    static func isCancel(_ text: String) -> Bool {
        let normalized = normalized(text)
        if normalized == "cancel" { return true }
        return cancelPhrases.contains { normalized.contains($0) }
    }
}
