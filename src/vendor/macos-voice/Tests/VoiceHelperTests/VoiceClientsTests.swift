import Testing
@testable import VoiceCore

struct VoiceClientsTests {
    @Test("playback rates belong to each client and are forgotten on disconnect")
    func playbackRates() {
        var clients = VoiceClients()
        clients.add(3)
        clients.add(4)
        clients.setRate(3, 0.6)
        clients.setRate(4, 0.5)
        #expect(clients.rate(3, fallback: 0.5) == 0.6)
        #expect(clients.rate(4, fallback: 0.5) == 0.5)
        clients.setRate(3, .nan)
        #expect(clients.rate(3, fallback: 0.5) == 0.6)
        clients.remove(3)
        clients.setRate(3, 0.9)
        clients.add(3)
        #expect(clients.rate(3, fallback: 0.5) == 0.5)
    }

    @Test("mic off for every client stops the mic and names the other clients")
    func micOffAll() {
        var clients = VoiceClients()
        clients.add(3)
        clients.add(4)
        clients.add(5)
        clients.setMic(4, false)
        #expect(clients.micWanted)
        #expect(Set(clients.micOffAll(from: 3)) == [4, 5])
        #expect(!clients.micWanted)
        clients.setMic(5, true)
        #expect(clients.micWanted)
    }

    @Test("a client that is gone cannot change the mic or leave it off for a reused fd")
    func unknownClient() {
        var clients = VoiceClients()
        clients.add(3)
        #expect(clients.micOffAll(from: 9).isEmpty)
        #expect(clients.micWanted)
        clients.setMic(9, false)
        clients.setMic(3, false)
        clients.add(9)
        #expect(clients.micWanted)
    }

    @Test("removing a client forgets its mic state")
    func remove() {
        var clients = VoiceClients()
        clients.add(3)
        clients.setMic(3, false)
        let removed = clients.remove(3)
        let again = clients.remove(3)
        #expect(removed)
        #expect(!again)
        #expect(clients.isEmpty)
        clients.add(3)
        #expect(clients.micWanted)
    }
}
