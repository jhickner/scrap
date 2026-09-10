import Foundation

/// Decides when a child has finished speaking.
///
/// With no button to release, the end of a turn has to be inferred from silence. The
/// recognizer finalizes a segment on every pause, so a turn is one or more finalized segments
/// followed by a long enough gap. Getting this wrong in the impatient direction is the worst
/// failure this feature has — it cuts a child off mid-thought — so the threshold is
/// deliberately longer than an adult would need, and any sign of speech resets it.
struct TurnEndpointer: Sendable {
    /// The baseline silence before the accumulated segments are treated as a turn. What is
    /// actually waited is this scaled by how finished the utterance sounds, so the number
    /// itself is only the middle case.
    static let defaultSilence: TimeInterval = 1.4

    /// Fillers and stray tokens that an open microphone produces constantly. On their own they
    /// are not a turn; inside a longer segment they are left alone.
    static let fillers: Set<String> = [
        "um", "uh", "hmm", "hm", "mm", "mhm", "er", "ah", "oh", "eh", "the", "a", "and", "so", "you",
    ]

    /// Things a child says to mean "be quiet" and nothing else. Matched against the whole
    /// utterance rather than searched for inside it, so "stop the volcano" is a question and
    /// only a bare "stop" silences the Primer.
    static let stopPhrases: Set<String> = [
        "stop", "wait", "quiet", "be quiet", "shush", "shh", "hush", "stop talking",
        "stop it", "enough", "thats enough", "shut up", "silence", "hold on", "one second",
    ]

    /// Things a child says to throw away what they were in the middle of saying.
    static let cancelPhrases = [
        "scratch that", "never mind", "nevermind", "forget it", "cancel that", "start over",
    ]

    enum Decision: Equatable, Sendable {
        case waiting
        /// The child said something that means "throw away what I was saying".
        case cancelled
        case send(String)
    }

    var silence: TimeInterval
    private(set) var segments: [String] = []
    private var lastActivity: TimeInterval?

    init(silence: TimeInterval = TurnEndpointer.defaultSilence) {
        self.silence = silence
    }

    /// What the child has said so far this turn, for the composer to show as it forms.
    var draft: String { segments.joined(separator: " ") }

    /// How the turn so far reads, and therefore how long its silence has to run.
    var completion: UtteranceCompletion { UtteranceClassifier.classify(draft) }
    var silenceThreshold: TimeInterval { silence * completion.silenceFactor }
    var hasSpeech: Bool { !segments.isEmpty }

    /// A running hypothesis for the segment being spoken. It never commits anything; it only
    /// proves the child is still talking, which is exactly what has to reset the silence clock.
    mutating func noteVolatile(_ text: String, at time: TimeInterval) {
        guard !Self.isNoise(text) else { return }
        lastActivity = time
    }

    /// One segment the recognizer has committed to.
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

    /// Polled on a timer. Returns the turn once the silence after the last speech is long enough.
    mutating func poll(at time: TimeInterval) -> Decision {
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

    /// True for the empties, the punctuation, and the lone fillers an open microphone yields
    /// between real utterances.
    static func isNoise(_ text: String) -> Bool {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return true }
        let words = trimmed
            .lowercased()
            .split { !$0.isLetter && !$0.isNumber }
            .map(String.init)
        guard !words.isEmpty else { return true }
        // Bracketed recognizer annotations such as "[BLANK_AUDIO]" are never speech.
        if trimmed.hasPrefix("[") || trimmed.hasPrefix("(") { return true }
        return words.allSatisfy { fillers.contains($0) }
    }

    /// True when the utterance is *only* an instruction to stop talking. Such a turn is not
    /// a question: the Primer stops, and nothing is sent, because the child has asked for
    /// silence rather than for an answer.
    static func isStopCommand(_ text: String) -> Bool {
        var words = normalized(text).split(separator: " ").map(String.init)
        // "Okay, stop" and "stop, please" are still just a stop.
        while let first = words.first, stopPadding.contains(first) { words.removeFirst() }
        while let last = words.last, stopPadding.contains(last) { words.removeLast() }
        let phrase = words.joined(separator: " ")
        if stopPhrases.contains(phrase) { return true }
        // "Stop. Stop." is one stop said twice.
        return stopPhrases.contains { stop in
            phrase == Array(repeating: stop, count: max(1, words.count / max(1, stop.split(separator: " ").count)))
                .joined(separator: " ")
        }
    }

    /// Words a child wraps a stop in without changing what it means.
    static let stopPadding: Set<String> = [
        "okay", "ok", "please", "now", "hey", "um", "uh", "just", "primer",
    ]

    static func normalized(_ text: String) -> String {
        // Apostrophes are removed rather than split on, so "that's enough" is two words and
        // matches the phrase list instead of becoming "that s enough".
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
