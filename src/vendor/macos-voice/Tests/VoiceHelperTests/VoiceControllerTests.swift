import Foundation
import Testing
@testable import VoiceCore

@MainActor
struct VoiceControllerTests {
    @Test("reply completion preserves an unfinished dictation and its stall recovery")
    func resumeWithVolatile() {
        let voice = VoiceController()
        var previews: [String] = []
        var sent: [String] = []
        voice.onHeard = { previews.append($0) }
        voice.onSend = { sent.append($0) }
        voice.handle(.volatile("Please investigate the missing text"), at: 100)

        // A reply can finish before the recognizer finalizes any segment.
        voice.resumeListening()
        #expect(voice.heardDraft == "Please investigate the missing text")
        #expect(!previews.contains(""))
        #expect(sent.isEmpty)

        // No real final arrives: the existing timeout must still recover it.
        voice.pollStalledVolatile(at: 160)
        #expect(voice.heardDraft == "Please investigate the missing text")
        voice.handle(.final("Please investigate the missing text", confidence: nil), at: 161)
        voice.handle(.volatile("and the delayed submission"), at: 162)
        #expect(voice.heardDraft == "Please investigate the missing text and the delayed submission")
        #expect(sent.isEmpty)
    }

    @Test("a delayed final revises the preserved draft without a blank preview")
    func resumeBeforeDelayedFinal() {
        let voice = VoiceController()
        var previews: [String] = []
        voice.onHeard = { previews.append($0) }
        voice.handle(.volatile("Investigate the missing"), at: 100)
        voice.resumeListening()
        voice.handle(.final("Investigate the missing text.", confidence: nil), at: 170)
        #expect(previews == ["Investigate the missing", "Investigate the missing text."])
        #expect(voice.handoff() == "Investigate the missing text.")
    }

    @Test("reply completion preserves a partial after finalized segments")
    func resumeWithFinalAndVolatile() {
        let voice = VoiceController()
        voice.handle(.final("First sentence.", confidence: nil), at: 100)
        voice.handle(.volatile("Second sentence"), at: 101)
        voice.resumeListening()
        #expect(voice.heardDraft == "First sentence. Second sentence")
        voice.handle(.final("Second sentence revised.", confidence: nil), at: 102)
        #expect(voice.heardDraft == "First sentence. Second sentence revised.")
    }
    @Test("a delayed barge-in final keeps its preview and is delivered once", arguments: [false, true])
    func delayedBargeInFinal(busy: Bool) {
        var level: Float = 0.662
        let voice = VoiceController(output: SilentOutput(), levelPeak: { window in window == 5 ? 0.662 : level })
        var sent: [String] = []
        var previews: [String] = []
        var drops = 0
        voice.onSend = { sent.append($0) }
        voice.onHeard = { previews.append($0) }
        voice.onDrop = { drops += 1 }

        // Establish the previous turn's peak, as in the 12:24 incident.
        voice.handle(.final("Check the voice build.", confidence: 0.97), at: 100)
        voice.pollTurn(at: 110)
        sent.removeAll()
        previews.removeAll()
        voice.setBusy(busy)
        voice.speak(["The voice build is current."])
        level = 0.501
        voice.handle(.volatile("Okay, dispatch Gro to do something small so you get its quota"), at: 120)
        level = 0.010
        let final = "Okay, and dispatch Brock to do something small so you get its quota."
        voice.handle(.final(final, confidence: 0.82), at: 122)
        #expect(voice.heardDraft == final)
        #expect(!previews.contains(""))
        #expect(drops == 0)

        voice.pollTurn(at: 130)
        #expect(sent == (busy ? [final] : []))
        voice.resumeListening()
        voice.pollTurn(at: 140)
        #expect(sent == [final])

        // The preceding segment's loudness must not authorize another final.
        voice.speak(["Another reply."])
        voice.handle(.final("Unrelated quiet speech.", confidence: 0.9), at: 150)
        #expect(voice.heardDraft.isEmpty)
        #expect(drops == 1)
    }

    @Test("retained speech level does not bypass final confidence rejection")
    func lowConfidenceFinal() {
        var level: Float = 0.662
        let voice = VoiceController(output: SilentOutput(), levelPeak: { window in window == 5 ? 0.662 : level })
        voice.handle(.final("Check the voice build.", confidence: 0.97), at: 100)
        voice.pollTurn(at: 110)
        voice.speak(["The voice build is current."])
        level = 0.501
        voice.handle(.volatile("Some uncertain words"), at: 120)
        level = 0.010
        voice.handle(.final("Some uncertain words.", confidence: 0.2), at: 122)
        #expect(voice.heardDraft.isEmpty)
        #expect(voice.handoff() == nil)
    }

}


@MainActor
private final class SilentOutput: VoiceOutput {
    var volume: Float = 1
    var rate: Float = 0.5
    var onSpoken: ((String) -> Void)?
    var onFinished: (() -> Void)?
    var hasPendingSpeech = false
    func speak(_ text: String) {}
    func pause(_ duration: TimeInterval) {}
    func stop() {}
}
