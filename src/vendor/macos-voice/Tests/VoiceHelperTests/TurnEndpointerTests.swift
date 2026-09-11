import Foundation
@testable import VoiceCore
import Testing

@Suite("Turn endpointer")
struct TurnEndpointerTests {
    @Test("sends once the silence after the last segment is long enough")
    func sendsAfterSilence() {
        var endpointer = TurnEndpointer(silence: 1.4)
        #expect(endpointer.noteFinal("what is a volcano", at: 10) == .waiting)
        #expect(endpointer.poll(at: 11.0) == .waiting)
        #expect(endpointer.poll(at: 11.5) == .send("what is a volcano"))
    }

    @Test("joins segments spoken across a pause into one turn")
    func joinsSegments() {
        var endpointer = TurnEndpointer(silence: 1.4)
        _ = endpointer.noteFinal("what is a volcano", at: 10)
        _ = endpointer.noteFinal("and where are they", at: 11)
        #expect(endpointer.draft == "what is a volcano and where are they")
        #expect(endpointer.poll(at: 12.5) == .send("what is a volcano and where are they"))
    }

    @Test("a child still talking resets the clock, so they are never cut off mid-thought")
    func volatileResetsTheClock() {

        var endpointer = TurnEndpointer(silence: 1.4)
        _ = endpointer.noteFinal("what is a volcano?", at: 10)
        #expect(endpointer.poll(at: 10.5) == .waiting)
        endpointer.noteVolatile("actually wait", at: 10.6)
        #expect(endpointer.poll(at: 11.2, awaitingFinal: true) == .waiting)
        #expect(endpointer.poll(at: 11.4, awaitingFinal: true) == .waiting)
        #expect(endpointer.poll(at: 20, awaitingFinal: true) == .waiting)
        _ = endpointer.noteFinal("actually wait for the illustration", at: 20)
        #expect(endpointer.poll(at: 21.5) == .send(
            "what is a volcano? actually wait for the illustration"
        ))
        #expect(endpointer.poll(at: 30) == .waiting)
    }

    @Test("a stalled partial can be promoted before the whole turn is sent")
    func stalledTailIsPreserved() {
        var endpointer = TurnEndpointer(silence: 1.4)
        _ = endpointer.noteFinal("please explain the", at: 10)
        endpointer.noteVolatile("illustration", at: 11)
        #expect(endpointer.poll(at: 16, awaitingFinal: true) == .waiting)
        _ = endpointer.noteFinal("illustration", at: 11)
        #expect(endpointer.poll(at: 16) == .send("please explain the illustration"))
    }

    @Test("says nothing when nothing has been said")
    func silenceAloneIsNotATurn() {
        var endpointer = TurnEndpointer(silence: 1.4)
        #expect(endpointer.poll(at: 100) == .waiting)
    }

    @Test("ignores the fillers and empties an open microphone produces")
    func ignoresNoise() {
        var endpointer = TurnEndpointer(silence: 1.4)
        #expect(endpointer.noteFinal("um", at: 10) == .waiting)
        #expect(endpointer.noteFinal("  ", at: 10.2) == .waiting)
        #expect(endpointer.noteFinal("[BLANK_AUDIO]", at: 10.4) == .waiting)
        #expect(endpointer.hasSpeech == false)
        #expect(endpointer.poll(at: 20) == .waiting)
    }

    @Test("keeps a filler that is part of something real")
    func keepsFillersInsideSpeech() {
        var endpointer = TurnEndpointer(silence: 1.4)
        _ = endpointer.noteFinal("um what is a volcano", at: 10)
        #expect(endpointer.poll(at: 12) == .send("um what is a volcano"))
    }

    @Test("throws the turn away when the child takes it back")
    func cancels() {
        var endpointer = TurnEndpointer(silence: 1.4)
        _ = endpointer.noteFinal("what is a vol", at: 10)
        #expect(endpointer.noteFinal("scratch that", at: 11) == .cancelled)
        #expect(endpointer.hasSpeech == false)
        #expect(endpointer.poll(at: 20) == .waiting)
    }

    @Test("waits longer when the child is plainly mid-thought")
    func waitsLongerOnIncompleteSpeech() {
        var endpointer = TurnEndpointer(silence: 1.4)
        _ = endpointer.noteFinal("what is a volcano and", at: 10)

        #expect(endpointer.poll(at: 11.5) == .waiting)
        #expect(endpointer.poll(at: 12.7) == .send("what is a volcano and"))
    }

    @Test("sends sooner when the question is plainly finished")
    func sendsSoonerOnCompleteSpeech() {
        var endpointer = TurnEndpointer(silence: 1.4)
        _ = endpointer.noteFinal("what is a volcano?", at: 10)
        #expect(endpointer.poll(at: 10.6) == .waiting)
        #expect(endpointer.poll(at: 10.8) == .send("what is a volcano?"))
    }

    @Test("a bare stop is an instruction to be quiet, not a question")
    func recognizesStopCommands() {
        #expect(TurnEndpointer.isStopCommand("stop"))
        #expect(TurnEndpointer.isStopCommand("Stop!"))
        #expect(TurnEndpointer.isStopCommand("be quiet"))
        #expect(TurnEndpointer.isStopCommand("that's enough"))
        #expect(TurnEndpointer.isStopCommand("hold on"))
        #expect(TurnEndpointer.isStopCommand("Okay, stop."))
        #expect(TurnEndpointer.isStopCommand("stop please"))
        #expect(TurnEndpointer.isStopCommand("hey, be quiet now"))
        #expect(TurnEndpointer.isStopCommand("Stop. Stop."))
        #expect(TurnEndpointer.isStopCommand("hold on, hold on"))
    }

    @Test("pause and resume are commands only on their own")
    func recognizesPauseCommands() {
        #expect(TurnEndpointer.isPauseCommand("Pause."))
        #expect(TurnEndpointer.isPauseCommand("okay, resume listening"))
        #expect(TurnEndpointer.isPauseCommand("Pause, pause."))
        #expect(!TurnEndpointer.isPauseCommand("pause the build"))
        #expect(!TurnEndpointer.isStopCommand("pause"))
    }

    @Test("a stop inside a real sentence is part of the question")
    func stopInsideASentenceIsNotACommand() {

        #expect(!TurnEndpointer.isStopCommand("stop the volcano"))
        #expect(!TurnEndpointer.isStopCommand("why did it stop"))
        #expect(!TurnEndpointer.isStopCommand("what makes lava stop moving"))
    }

    @Test("does not send the same turn twice")
    func sendsOnce() {
        var endpointer = TurnEndpointer(silence: 1.4)
        _ = endpointer.noteFinal("hello", at: 10)
        #expect(endpointer.poll(at: 12) == .send("hello"))
        #expect(endpointer.poll(at: 13) == .waiting)
    }
}
