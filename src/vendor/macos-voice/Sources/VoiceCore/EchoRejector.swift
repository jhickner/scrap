import Foundation

struct EchoRejector: Sendable {

    static let rememberedUtterances = 6

    static let minimumWords = 4

    static let matchRatio = 0.6

    static let matchRatioWhileSpeaking = 0.5

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

    func shouldReject(
        _ heard: String, minimumWords: Int? = nil, ratio: Double = EchoRejector.matchRatio
    ) -> Bool {
        let words = Self.words(heard)
        guard words.count >= (minimumWords ?? Self.minimumWords) else { return false }
        return spoken.contains { overlaps(words, with: $0, ratio: ratio) }
    }

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
