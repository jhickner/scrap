import Foundation
@testable import VoiceCore
import Testing

@Suite("Echo rejector")
struct EchoRejectorTests {
    @Test("drops the reply coming back through the microphone")
    func rejectsExactEcho() {
        var rejector = EchoRejector()
        rejector.noteSpoken("A volcano is a mountain that opens downward.")
        #expect(rejector.shouldReject("a volcano is a mountain that opens downward"))
    }

    @Test("drops an echo the recognizer heard imperfectly")
    func rejectsPartialEcho() {
        var rejector = EchoRejector()
        rejector.noteSpoken("A volcano is a mountain that opens downward.")
        #expect(rejector.shouldReject("a volcano is a mountain that opens down word"))
    }

    @Test("lets the child speak")
    func keepsChildSpeech() {
        var rejector = EchoRejector()
        rejector.noteSpoken("A volcano is a mountain that opens downward.")
        #expect(!rejector.shouldReject("can we build one out of baking soda"))
    }

    @Test("lets a short answer through even when it reuses the Primer's own words")
    func keepsShortAnswersThatReuseVocabulary() {
        var rejector = EchoRejector()
        rejector.noteSpoken("Would you like to build a volcano, or read about one first?")
        #expect(!rejector.shouldReject("build a volcano"))
        #expect(!rejector.shouldReject("yes"))
    }

    @Test("still drops a long echo that reuses the same words in order")
    func rejectsLongEcho() {
        var rejector = EchoRejector()
        rejector.noteSpoken("Would you like to build a volcano, or read about one first?")
        #expect(rejector.shouldReject("would you like to build a volcano or read about one first"))
    }

    @Test("remembers across a few utterances, not forever")
    func forgetsOldUtterances() {
        var rejector = EchoRejector()
        rejector.noteSpoken("The first thing to know about volcanoes is pressure.")
        for index in 0..<EchoRejector.rememberedUtterances {
            rejector.noteSpoken("Filler sentence number \(index) about something else entirely.")
        }
        #expect(!rejector.shouldReject("the first thing to know about volcanoes is pressure"))
    }

    @Test("catches short leaks when the Primer is the one talking")
    func strictWhileSpeaking() {
        var rejector = EchoRejector()
        rejector.noteSpoken("Would you like to build a volcano, or read about one first?")
        // The same three words are let through while listening, because they are a plausible
        // answer, and caught while speaking, because nothing said then is the child's.
        #expect(!rejector.shouldReject("build a volcano"))
        #expect(rejector.shouldReject("build a volcano", minimumWords: 2))
    }

    @Test("forgets everything when a turn is abandoned")
    func resets() {
        var rejector = EchoRejector()
        rejector.noteSpoken("A volcano is a mountain.")
        rejector.reset()
        #expect(!rejector.shouldReject("a volcano is a mountain"))
    }

    @Test("says nothing about silence")
    func ignoresEmpty() {
        var rejector = EchoRejector()
        rejector.noteSpoken("A volcano is a mountain.")
        #expect(!rejector.shouldReject("   "))
    }

    @Test("a question made of function words is never mistaken for the reply")
    func functionWordsProveNothing() {
        var rejector = EchoRejector()
        rejector.noteSpoken("He is a poet from Ohio, and where he is from shapes what he writes.")
        #expect(!rejector.shouldReject("Where is", minimumWords: 2, ratio: 0.5))
        #expect(!rejector.shouldReject("Where is he from?", minimumWords: 2, ratio: 0.5))
        #expect(rejector.shouldReject("a poet from Ohio", minimumWords: 2, ratio: 0.5))
    }

    @Test("a half-misheard leak of the reply is still rejected by its content words")
    func misheardLeakIsRejected() {
        var rejector = EchoRejector()
        rejector.noteSpoken("The moon is about 384,000 kilometres away from the Earth.")
        #expect(rejector.shouldReject("The moon was a", minimumWords: 2))
        #expect(rejector.shouldReject(
            "The moon was a completely", minimumWords: 2,
            ratio: EchoRejector.matchRatioWhileSpeaking
        ))
    }
}
