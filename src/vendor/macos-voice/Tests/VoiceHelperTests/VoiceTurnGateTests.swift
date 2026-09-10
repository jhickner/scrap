import Testing
@testable import VoiceCore

struct VoiceTurnGateTests {
    @Test("stop aborts a model turn that has not started speaking")
    func stopWhileBusyListening() {
        #expect(VoiceTurnGate.canInterrupt(
            isBusy: true, mode: .listening, isFinal: true, echoCancelled: false
        ))
        #expect(VoiceTurnGate.canInterrupt(
            isBusy: true, mode: .listening, isFinal: false, echoCancelled: false
        ))
    }

    @Test("stop does not abort while idle listening")
    func stopWhileIdleListening() {
        #expect(!VoiceTurnGate.canInterrupt(
            isBusy: false, mode: .listening, isFinal: true, echoCancelled: false
        ))
    }

    @Test("stop aborts while answering")
    func stopWhileAnswering() {
        #expect(VoiceTurnGate.canInterrupt(
            isBusy: false, mode: .answering, isFinal: false, echoCancelled: false
        ))
    }

    @Test("while speaking, a partial stop needs echo cancellation")
    func stopWhileSpeaking() {
        #expect(!VoiceTurnGate.canInterrupt(
            isBusy: false, mode: .speaking, isFinal: false, echoCancelled: false
        ))
        #expect(VoiceTurnGate.canInterrupt(
            isBusy: false, mode: .speaking, isFinal: true, echoCancelled: false
        ))
        #expect(VoiceTurnGate.canInterrupt(
            isBusy: false, mode: .speaking, isFinal: false, echoCancelled: true
        ))
    }

    @Test("busy speech is sent immediately so the client can queue it")
    func sendImmediatelyWhenBusy() {
        #expect(VoiceTurnGate.sendImmediately(isBusy: true))
        #expect(!VoiceTurnGate.sendImmediately(isBusy: false))
    }
}
