import Foundation

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

    static func sendImmediately(isBusy: Bool) -> Bool { isBusy }
}
