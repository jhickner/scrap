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
}
