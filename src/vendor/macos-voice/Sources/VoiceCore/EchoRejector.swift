import Foundation

/// Drops the Primer's own voice when it leaks back into the microphone.
///
/// Voice processing subtracts the engine's own output from the microphone signal, but it is
/// subtraction rather than cancellation and some of the reply survives it — more of it the
/// louder the child sets the volume. This is the cheap backstop: whatever was just spoken is
/// remembered as text, and a recognized segment that matches it closely enough is not a turn.
struct EchoRejector: Sendable {
    /// Enough history to cover the AEC tail and a sentence or two either side of it.
    static let rememberedUtterances = 6

    /// Below this many words, a segment is left alone. "Build a volcano" is a perfectly good
    /// answer to a question that contained those words, and rejecting it would silently ignore
    /// the child. Short echo fragments are covered instead by the gate that closes the
    /// microphone while the Primer is speaking; this only has to catch what arrives late.
    static let minimumWords = 4

    /// Above this share of shared words, a heard segment is the reply coming back rather than
    /// the child. Set below 1 because the recognizer mishears leaked audio more than clean
    /// speech, so an echo is rarely word-perfect.
    static let matchRatio = 0.6
    /// The looser share used while the Primer is speaking, when a leak is the likelier
    /// explanation for anything heard and the recognizer mishears it word by word.
    static let matchRatioWhileSpeaking = 0.5

    /// Words that prove nothing. A reply of any length contains "where is he from" in order,
    /// so counting them would reject most short questions a child asks over the Primer. Only
    /// the words that carry the sentence are compared.
    static let functionWords: Set<String> = [
        "a", "an", "the", "and", "or", "but", "so", "of", "to", "in", "on", "at", "for",
        "from", "with", "by", "about", "as", "into", "over", "up", "down", "out", "off",
        "is", "are", "was", "were", "be", "been", "being", "am", "do", "does", "did", "have",
        "has", "had", "can", "could", "will", "would", "should", "may", "might", "must",
        "i", "you", "he", "she", "it", "we", "they", "me", "him", "her", "us", "them", "my",
        "your", "his", "its", "our", "their", "this", "that", "these", "those", "there",
        "here", "what", "which", "who", "whom", "whose", "where", "when", "why", "how",
        "not", "no", "yes", "if", "then", "than", "too", "very", "just", "also", "ok",
        "okay", "um", "uh", "oh", "well", "like", "really", "one", "some", "any", "all",
    ]

    private var spoken: [[String]] = []

    init() {}

    mutating func noteSpoken(_ text: String) {
        let words = Self.words(text)
        guard !words.isEmpty else { return }
        spoken.append(words)
        if spoken.count > Self.rememberedUtterances {
            spoken.removeFirst(spoken.count - Self.rememberedUtterances)
        }
    }

    mutating func reset() {
        spoken = []
    }

    /// `minimumWords` overrides the floor for callers that can afford to be aggressive. While
    /// the Primer is speaking, anything resembling what it is saying is its own voice — there
    /// is no short child answer to protect — so the floor drops and short leaks are caught.
    func shouldReject(
        _ heard: String, minimumWords: Int? = nil, ratio: Double = EchoRejector.matchRatio
    ) -> Bool {
        let words = Self.words(heard)
        guard words.count >= (minimumWords ?? Self.minimumWords) else { return false }
        return spoken.contains { overlaps(words, with: $0, ratio: ratio) }
    }

    /// A heard segment matches when most of its content words appear, in order, in something
    /// just spoken. Order matters: it separates an echo from a child who happens to reuse the
    /// Primer's vocabulary to answer it. A segment with no content words cannot be judged
    /// here and is left to the level and confidence gates.
    private func overlaps(_ heard: [String], with spoken: [String], ratio: Double) -> Bool {
        let heard = heard.filter { !Self.functionWords.contains($0) }
        guard !heard.isEmpty else { return false }
        var matched = 0
        var index = spoken.startIndex
        for word in heard {
            guard let found = spoken[index...].firstIndex(of: word) else { continue }
            matched += 1
            index = spoken.index(after: found)
        }
        return Double(matched) / Double(heard.count) >= ratio
    }

    static func words(_ text: String) -> [String] {
        text
            .lowercased()
            .split { !$0.isLetter && !$0.isNumber }
            .map(String.init)
    }
}
