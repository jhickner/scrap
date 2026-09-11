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
}
