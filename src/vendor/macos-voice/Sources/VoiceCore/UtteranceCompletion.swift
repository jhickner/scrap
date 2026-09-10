import Foundation

/// Whether what the child has said sounds finished.
///
/// A fixed silence timer has to be wrong in one direction or the other: short enough to feel
/// responsive after "what is a volcano?" is short enough to cut off a child who pauses in the
/// middle of "what is a…". The way out is to stop treating every silence the same. A pause
/// after a complete question means they are waiting; the same pause after a dangling
/// conjunction means they are thinking.
///
/// This reads the transcript rather than the audio, so it costs nothing and can be tested
/// without a microphone. The recognizer punctuates its own output, which is a strong signal
/// on its own, but a trailing "and" outranks it — a sentence that ends on a conjunction is not
/// finished no matter what mark the recognizer put after it.
enum UtteranceCompletion: Equatable, Sendable {
    /// Waiting for an answer. Send sooner than usual.
    case complete
    /// Mid-thought. Wait considerably longer.
    case incomplete
    /// No strong signal either way.
    case neutral

    /// How much of the base silence to wait for. A finished question waits about a second; a
    /// dangling "and then" gets closer to four.
    ///
    /// The floor is set by the recognizer, not the speaker: it delivers running hypotheses in
    /// bursts up to a second apart, and a wait shorter than that reads the gap between two
    /// bursts as silence and sends half a sentence. "Yes" and "what time is it?" are already
    /// whole, so they sit on that floor rather than scaling with the base.
    var silenceFactor: Double {
        switch self {
        case .complete: return 0.5
        case .neutral: return 1
        case .incomplete: return 1.9
        }
    }
}

enum UtteranceClassifier {
    /// Words a sentence does not end on. Ending here means the thought is still going: a
    /// conjunction, a preposition, an article, an auxiliary waiting for its verb, or a filler.
    static let continuations: Set<String> = [
        // conjunctions and subordinators
        "and", "but", "or", "nor", "so", "because", "cause", "since", "although", "though",
        "while", "whereas", "if", "unless", "until", "when", "whenever", "where", "whether",
        "that", "which", "who", "whom", "whose", "than", "then", "plus",
        // prepositions
        "to", "of", "in", "on", "at", "for", "from", "with", "without", "about", "into", "onto",
        "over", "under", "above", "below", "between", "among", "through", "during", "before",
        "after", "by", "like", "as", "near", "toward", "towards", "upon", "within",
        // articles and determiners
        "a", "an", "the", "my", "your", "his", "her", "its", "our", "their", "this", "these",
        "those", "some", "any", "every", "each", "another", "both", "several",
        // auxiliaries, modals, and verbs that take a complement
        "is", "are", "was", "were", "am", "be", "been", "being", "do", "does", "did", "have",
        "has", "had", "will", "would", "can", "could", "should", "shall", "may", "might",
        "must", "going", "gonna", "wanna", "want", "wants", "need", "needs", "try", "trying",
        // fillers
        "um", "uh", "er", "ah", "hmm", "hm", "mm", "eh",
    ]

    /// Utterances that are whole on their own. Without this a one-word answer looks like a
    /// fragment, and the child would be left waiting after saying "yes".
    static let standalone: Set<String> = [
        "yes", "yeah", "yep", "no", "nope", "okay", "ok", "sure", "maybe", "please", "thanks",
        "hi", "hello", "hey", "stop", "wait", "why", "what", "really", "cool", "wow", "nothing",
        "done", "nevermind",
    ]

    /// Beyond this many words, a fragment is unlikely enough that the absence of a terminator
    /// says more about the recognizer than about the child.
    static let fragmentWordCount = 2

    static func classify(_ text: String) -> UtteranceCompletion {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let words = trimmed
            .lowercased()
            .split { !$0.isLetter && !$0.isNumber && $0 != "'" }
            .map(String.init)
        guard let last = words.last else { return .neutral }

        // A trailing conjunction outranks punctuation: "and then." is not a finished thought
        // whatever the recognizer decided to put at the end of it.
        if continuations.contains(last) { return .incomplete }
        if words.allSatisfy(standalone.contains) { return .complete }

        let terminator = trimmed.last
        if terminator == "?" || terminator == "!" || terminator == "." { return .complete }
        if trimmed.hasSuffix(",") { return .incomplete }
        if words.count <= fragmentWordCount { return .incomplete }
        return .neutral
    }
}
