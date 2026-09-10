import Foundation
import os

public enum VoiceLog {
    private static let logger = Logger(subsystem: "local.c-libs.macos-voice", category: "voice")

    public static func note(_ message: String) {
        logger.info("\(message, privacy: .public)")
    }

    public static func problem(_ message: String) {
        logger.error("\(message, privacy: .public)")
    }
}

extension ContinuousClock.Instant {

    var elapsedMilliseconds: Int {
        Int((ContinuousClock.now - self) / .milliseconds(1))
    }
}
