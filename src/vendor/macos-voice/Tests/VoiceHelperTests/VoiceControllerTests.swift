import Foundation
import Testing
@testable import VoiceCore

@MainActor
struct VoiceControllerTests {
    @Test("a stalled phrase's longer final keeps the new words from the recorded incident",
          arguments: [false, true])
    func growingFinalAfterPromotion(deliverFirst: Bool) {
        let voice = VoiceController()
        var sent: [String] = []
        voice.onSend = { sent.append($0) }
        voice.handle(.volatile("kind of comparing and contrast"), at: 100)
        voice.pollStalledVolatile(at: 110)
        if deliverFirst { voice.pollTurn(at: 110) }
        let kept = deliverFirst ? "" : "kind of comparing and contrast "
        voice.handle(.volatile("kind of comparing and contrasting Some markdown file"), at: 111)
        #expect(voice.heardDraft == kept + "Some markdown file")
        voice.handle(.final("kind of comparing and contrasting, Some markdown file store, like, LLM Wiki.",
                            confidence: 0.81), at: 112)
        #expect(voice.heardDraft == kept + "Some markdown file store, like, LLM Wiki.")
        voice.pollTurn(at: 120)
        #expect(sent.joined(separator: " ") ==
                "kind of comparing and contrast Some markdown file store, like, LLM Wiki.")
    }

    @Test("repeated stalls remember the full recognized prefix")
    func repeatedPromotion() {
        let voice = VoiceController()
        voice.handle(.volatile("First sent"), at: 100)
        voice.pollStalledVolatile(at: 110)
        voice.handle(.volatile("First sentence. Second sentence"), at: 111)
        voice.pollStalledVolatile(at: 120)
        voice.handle(.volatile("First sentence. Second sentence. Third sentence"), at: 121)
        voice.handle(.final("First sentence. Second sentence. Third sentence.", confidence: nil), at: 122)
        #expect(voice.heardDraft == "First sent Second sentence Third sentence.")
    }

    @Test("a punctuation revision of promoted words is not duplicated")
    func promotedPunctuationRevision() {
        let voice = VoiceController()
        voice.handle(.volatile("hello world"), at: 100)
        voice.pollStalledVolatile(at: 110)
        voice.handle(.volatile("Hello, world."), at: 111)
        #expect(voice.heardDraft == "hello world")
        voice.handle(.final("Hello, world.", confidence: nil), at: 112)
        #expect(voice.heardDraft == "hello world")
        voice.handle(.volatile("Next sentence"), at: 113)
        #expect(voice.heardDraft == "hello world Next sentence")
    }

    @Test("a rewritten prefix cannot discard new speech")
    func promotedPrefixRewritten() {
        let voice = VoiceController()
        voice.handle(.volatile("Compare the file"), at: 100)
        voice.pollStalledVolatile(at: 110)
        voice.handle(.final("Comparing those files reveals another issue.", confidence: nil), at: 111)
        #expect(voice.heardDraft.contains("reveals another issue."))
    }

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
