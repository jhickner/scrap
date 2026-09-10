import AVFoundation
import Foundation

public enum VoiceChime: String {

    case sent

    case interrupted

    case listening

    var notes: [(frequency: Double, duration: Double)] {
        switch self {
        case .sent: return [(784, 0.055), (1046.5, 0.075)]
        case .interrupted: return [(660, 0.05)]
        case .listening: return [(880, 0.06)]
        }
    }
}

enum VoiceChimes {

    static let amplitude: Float = 0.11

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
