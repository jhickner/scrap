import Foundation
import os

/// One place for the voice pipeline's diagnostics.
///
/// Voice is the part of Primer that cannot be watched while it runs — it fails on an audio
/// thread, inside a system recognizer, or in a permission dialog, and none of those leave a
/// trace in the transcript. Reading `log stream --predicate 'subsystem == "local.c-libs.macos-voice"'`
/// while talking to it is the only way to see what happened.
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
    /// Milliseconds since this instant, for timing the phases of bringing audio up.
    var elapsedMilliseconds: Int {
        Int((ContinuousClock.now - self) / .milliseconds(1))
    }
}
