import Foundation

enum UtteranceCompletion: Equatable, Sendable {

    case complete

    case incomplete

    case neutral

    var silenceFactor: Double {
        switch self {
        case .complete: return 0.5
        case .neutral: return 1
        case .incomplete: return 1.9
        }
    }
}

enum UtteranceClassifier {

    static let continuations: Set<String> = [

        "and", "but", "or", "nor", "so", "because", "cause", "since", "although", "though",
        "while", "whereas", "if", "unless", "until", "when", "whenever", "where", "whether",
        "that", "which", "who", "whom", "whose", "than", "then", "plus",

        "to", "of", "in", "on", "at", "for", "from", "with", "without", "about", "into", "onto",
        "over", "under", "above", "below", "between", "among", "through", "during", "before",
        "after", "by", "like", "as", "near", "toward", "towards", "upon", "within",

        "a", "an", "the", "my", "your", "his", "her", "its", "our", "their", "this", "these",
        "those", "some", "any", "every", "each", "another", "both", "several",

        "is", "are", "was", "were", "am", "be", "been", "being", "do", "does", "did", "have",
        "has", "had", "will", "would", "can", "could", "should", "shall", "may", "might",
        "must", "going", "gonna", "wanna", "want", "wants", "need", "needs", "try", "trying",

        "um", "uh", "er", "ah", "hmm", "hm", "mm", "eh",
    ]

    static let standalone: Set<String> = [
        "yes", "yeah", "yep", "no", "nope", "okay", "ok", "sure", "maybe", "please", "thanks",
        "hi", "hello", "hey", "stop", "wait", "why", "what", "really", "cool", "wow", "nothing",
        "done", "nevermind",
    ]

    static let fragmentWordCount = 2

    static func classify(_ text: String) -> UtteranceCompletion {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let words = trimmed
            .lowercased()
            .split { !$0.isLetter && !$0.isNumber && $0 != "'" }
            .map(String.init)
        guard let last = words.last else { return .neutral }

        if continuations.contains(last) { return .incomplete }
        if words.allSatisfy(standalone.contains) { return .complete }

        let terminator = trimmed.last
        if terminator == "?" || terminator == "!" || terminator == "." { return .complete }
        if trimmed.hasSuffix(",") { return .incomplete }
        if words.count <= fragmentWordCount { return .incomplete }
        return .neutral
    }
}
