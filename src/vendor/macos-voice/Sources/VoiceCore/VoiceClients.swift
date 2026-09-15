/// Connected clients and which of them have turned the microphone off. The
/// microphone runs while any client wants it.
public struct VoiceClients: Sendable {
    public private(set) var all = Set<Int32>()
    private var micOff = Set<Int32>()
    private var rates: [Int32: Float] = [:]

    public init() {}

    public var isEmpty: Bool { all.isEmpty }
    public var micWanted: Bool { all.contains { !micOff.contains($0) } }

    public func contains(_ fd: Int32) -> Bool { all.contains(fd) }

    public mutating func add(_ fd: Int32) {
        all.insert(fd)
        micOff.remove(fd)
    }

    @discardableResult
    public mutating func remove(_ fd: Int32) -> Bool {
        micOff.remove(fd)
        rates.removeValue(forKey: fd)
        return all.remove(fd) != nil
    }

    public mutating func setRate(_ fd: Int32, _ rate: Float) {
        guard all.contains(fd), rate.isFinite else { return }
        rates[fd] = min(max(rate, 0), 1)
    }

    public func rate(_ fd: Int32, fallback: Float) -> Float {
        rates[fd] ?? fallback
    }

    public mutating func setMic(_ fd: Int32, _ on: Bool) {
        guard all.contains(fd) else { return }
        if on { micOff.remove(fd) } else { micOff.insert(fd) }
    }

    /// Turns the microphone off for every client. Returns the clients other
    /// than `fd`, which are told to turn theirs off.
    public mutating func micOffAll(from fd: Int32) -> [Int32] {
        guard all.contains(fd) else { return [] }
        micOff = all
        return all.filter { $0 != fd }
    }
}
