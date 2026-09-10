import Foundation

/// What the Primer speaks with. Behind a protocol because the voice is the part most likely to
/// be replaced — a hosted voice would slot in here without the engine, the chunker, or the
/// turn-taking knowing about it.
@MainActor
protocol VoiceOutput: AnyObject {
    /// Queues one utterance. Utterances are spoken in the order they arrive.
    func speak(_ text: String)
    /// Queues a silence, in order with the utterances around it.
    func pause(_ duration: TimeInterval)
    /// Abandons everything queued and stops mid-word. Used for barge-in.
    func stop()
    /// 0 is silence, 1 is full. Applies to utterances started after it changes.
    var volume: Float { get set }
    /// AVSpeechUtterance rate. Applies to utterances started after it changes.
    var rate: Float { get set }
    /// Called as each utterance finishes, with the text that was spoken, so the caller can
    /// recognise it coming back through the microphone.
    var onSpoken: ((String) -> Void)? { get set }
    /// Called when the output has nothing left to say. Only the direct route reports this;
    /// audio scheduled on the engine is drained by the engine.
    var onFinished: (() -> Void)? { get set }
    /// True while an utterance is still being rendered, queued, or spoken. Read by the
    /// controller to tell a reply that is still coming from one whose completion was lost.
    var hasPendingSpeech: Bool { get }
}
