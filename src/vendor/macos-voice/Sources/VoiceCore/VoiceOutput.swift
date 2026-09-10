import Foundation

@MainActor
protocol VoiceOutput: AnyObject {

    func speak(_ text: String)

    func pause(_ duration: TimeInterval)

    func stop()

    var volume: Float { get set }

    var rate: Float { get set }

    var onSpoken: ((String) -> Void)? { get set }

    var onFinished: (() -> Void)? { get set }

    var hasPendingSpeech: Bool { get }

    /* drop a render whose completion never arrived; true when one was dropped */
    func sweepStalledRender() -> Bool
}

extension VoiceOutput {
    func sweepStalledRender() -> Bool { false }
}
