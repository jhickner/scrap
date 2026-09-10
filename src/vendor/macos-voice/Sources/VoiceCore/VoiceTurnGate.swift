import Foundation

/// When a client has marked itself busy (a model turn is running), extra speech
/// is sent immediately so the client can queue it, and a stop word aborts that
/// turn even before any reply audio has started.
enum VoiceTurnGate {
    static func canInterrupt(
        isBusy: Bool,
        mode: VoiceController.Mode,
        isFinal: Bool,
        echoCancelled: Bool
    ) -> Bool {
        if isBusy { return true }
        switch mode {
        case .answering: return true
        case .speaking: return echoCancelled || isFinal
        case .idle, .starting, .listening: return false
        }
    }

    /// Busy means the client is ready to queue; do not hold the utterance.
    static func sendImmediately(isBusy: Bool) -> Bool { isBusy }
}
