import AVFoundation
import Foundation

/// Short tones that tell the child what just happened without saying it.
///
/// Speech has no visible submit button, so the moments a typed interface gets for free — the
/// message left, the Primer is working on it — have to be marked some other way. A tone does it
/// in a tenth of a second and does not interrupt the reading of the reply the way spoken words
/// would. Borrowed from voiceclaude, which needs the same acknowledgements for the same reason.
enum VoiceChime {
    /// The turn has been sent and an answer is coming. Rising, because something is starting.
    case sent
    /// The turn was interrupted and the Primer is listening again.
    case interrupted
    /// The microphone is open. Some of the wait before this is Bluetooth renegotiating the
    /// link and cannot be removed, so the moment it ends is worth marking.
    case listening

    /// Frequency in hertz and duration in seconds, played in order.
    var notes: [(frequency: Double, duration: Double)] {
        switch self {
        case .sent: return [(784, 0.055), (1046.5, 0.075)]
        case .interrupted: return [(660, 0.05)]
        case .listening: return [(880, 0.06)]
        }
    }
}

enum VoiceChimes {
    /// Quiet enough to sit under a voice rather than over it.
    static let amplitude: Float = 0.11
    /// Ramps at each end of a note. Without them the abrupt start and stop of a sine is heard
    /// as a click, which is louder and more startling than the tone itself.
    static let rampSeconds = 0.006

    static func buffer(for chime: VoiceChime, format: AVAudioFormat) -> AVAudioPCMBuffer? {
        let rate = format.sampleRate
        let frames = chime.notes.reduce(0) { $0 + Int($1.duration * rate) }
        guard frames > 0,
              let buffer = AVAudioPCMBuffer(
                  pcmFormat: format,
                  frameCapacity: AVAudioFrameCount(frames)
              ),
              let channels = buffer.floatChannelData
        else { return nil }
        buffer.frameLength = AVAudioFrameCount(frames)

        var offset = 0
        for note in chime.notes {
            let noteFrames = Int(note.duration * rate)
            let ramp = max(1, Int(rampSeconds * rate))
            for frame in 0..<noteFrames {
                let envelope = Float(min(1, Double(frame) / Double(ramp)))
                    * Float(min(1, Double(noteFrames - frame) / Double(ramp)))
                let phase = 2 * Double.pi * note.frequency * Double(frame) / rate
                let sample = Float(sin(phase)) * amplitude * envelope
                for channel in 0..<Int(format.channelCount) {
                    channels[channel][offset + frame] = sample
                }
            }
            offset += noteFrames
        }
        return buffer
    }
}
