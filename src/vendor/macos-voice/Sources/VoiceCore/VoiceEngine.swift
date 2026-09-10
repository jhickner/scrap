import AVFoundation
import Foundation

struct VoiceAudioBuffer: @unchecked Sendable {
    let buffer: AVAudioPCMBuffer
}

final class LevelMeter: @unchecked Sendable {
    private let lock = NSLock()
    private var samples: [(time: TimeInterval, peak: Float)] = []
    private static let history: TimeInterval = 5

    func note(peak: Float) {
        let now = Date.timeIntervalSinceReferenceDate
        lock.lock()
        samples.append((now, peak))
        while let first = samples.first, now - first.time > Self.history {
            samples.removeFirst()
        }
        lock.unlock()
    }

    func peak(within seconds: TimeInterval) -> Float {
        let since = Date.timeIntervalSinceReferenceDate - seconds
        lock.lock()
        defer { lock.unlock() }
        return samples.lazy.filter { $0.time >= since }.map(\.peak).max() ?? 0
    }

    func reset() {
        lock.lock()
        samples = []
        lock.unlock()
    }
}

private final class ConversionSource: @unchecked Sendable {
    let buffer: AVAudioPCMBuffer
    var consumed = false
    init(_ buffer: AVAudioPCMBuffer) { self.buffer = buffer }
}

private final class TapState: @unchecked Sendable {
    var converter: AVAudioConverter?
    var count = 0

    var channelPeaks: [Float] = []
    var monoPeak: Float = 0

    var bufferPeak: Float = 0
    let target: AVAudioFormat
    let levels: LevelMeter
    init(target: AVAudioFormat, levels: LevelMeter) {
        self.target = target
        self.levels = levels
    }
}

@MainActor
final class VoiceEngine {
    enum StartError: LocalizedError {
        case noInput
        case engineUnavailable(String)

        case allAttemptsFailed(String)

        var errorDescription: String? {
            switch self {
            case .noInput:
                return "No microphone is available."
            case let .engineUnavailable(reason):
                return "The microphone could not be started: \(reason)"
            case let .allAttemptsFailed(detail):
                return "The microphone could not be started. \(detail)"
            }
        }
    }

    private static let startAttempts = 6
    private static let startRetryDelays: [Duration] = [
        .milliseconds(60), .milliseconds(120), .milliseconds(240),
        .milliseconds(400), .milliseconds(600),
    ]

    private static let deviceSettleDelay = Duration.milliseconds(600)

    private static let voiceProcessingSettleDelay = Duration.milliseconds(150)

    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private var micContinuation: AsyncStream<VoiceAudioBuffer>.Continuation?

    let levels = LevelMeter()
    private var configurationObserver: (any NSObjectProtocol)?
    private var scheduledBuffers = 0
    private var playbackGeneration = 0

    private(set) var isRunning = false

    private(set) var isEchoCancelled = false

    var onConfigurationChange: (() -> Void)?

    var onPlaybackDrained: (() -> Void)?

    var playbackVolume: Float = 1 {
        didSet { player.volume = max(0, min(1, playbackVolume)) }
    }

    var playbackFormat: AVAudioFormat {
        engine.mainMixerNode.outputFormat(forBus: 0)
    }

    var isSpeaking: Bool { scheduledBuffers > 0 }

    private struct Attempt {
        let voiceProcessing: Bool
        let preferredDevice: Bool

        var label: String {
            "\(voiceProcessing ? "echo-cancelled" : "plain")/\(preferredDevice ? "named" : "default")"
        }

        private static let wiringVersion = 2

        private static var defaultsKey: String {
            let device = VoiceEngine.defaultInputDevice().flatMap(VoiceEngine.uid) ?? "unknown"
            return "PrimerVoiceWiring.\(wiringVersion).\(device)"
        }

        func remember() {
            UserDefaults.standard.set(
                [voiceProcessing, preferredDevice],
                forKey: Attempt.defaultsKey
            )
        }

        static func forget() {
            UserDefaults.standard.removeObject(forKey: defaultsKey)
            UserDefaults.standard.removeObject(forKey: probedKey)
        }

        private static var probedKey: String { defaultsKey + ".probed" }

        static var lastProbed: Date? {
            get { UserDefaults.standard.object(forKey: probedKey) as? Date }
            set { UserDefaults.standard.set(newValue, forKey: probedKey) }
        }

        static func remembered() -> Attempt? {
            guard let flags = UserDefaults.standard.array(forKey: defaultsKey) as? [Bool],
                  flags.count == 2
            else { return nil }
            return Attempt(voiceProcessing: flags[0], preferredDevice: flags[1])
        }
    }

    private static let attempts = [
        Attempt(voiceProcessing: true, preferredDevice: false),
        Attempt(voiceProcessing: true, preferredDevice: true),
        Attempt(voiceProcessing: false, preferredDevice: false),
        Attempt(voiceProcessing: false, preferredDevice: true),
    ]

    func start(
        micFormat: AVAudioFormat,
        preferredInputName: String?
    ) async throws -> AsyncStream<VoiceAudioBuffer> {
        if isRunning { stop() }
        let clock = ContinuousClock.now
        var failures: [String] = []

        for attempt in Self.orderedAttempts() {
            if attempt.preferredDevice, preferredInputName?.isEmpty != false { continue }
            do {
                let stream = try await start(
                    micFormat: micFormat,
                    deviceName: attempt.preferredDevice ? preferredInputName : nil,
                    voiceProcessing: attempt.voiceProcessing
                )
                isEchoCancelled = attempt.voiceProcessing
                attempt.remember()
                VoiceLog.note("engine started: \(attempt.label) in \(clock.elapsedMilliseconds)ms")
                return stream
            } catch {
                failures.append("\(attempt.label) \(Self.describe(error))")
                VoiceLog.problem("engine attempt \(attempt.label) failed: \(Self.describe(error))")
                teardown()
            }
        }
        Attempt.forget()
        throw StartError.allAttemptsFailed(failures.joined(separator: "; "))
    }

    static let reprobeInterval: TimeInterval = 24 * 60 * 60

    private static func orderedAttempts() -> [Attempt] {
        guard let remembered = Attempt.remembered() else {
            Attempt.lastProbed = Date()
            return attempts
        }
        if !remembered.voiceProcessing,
           Attempt.lastProbed.map({ Date().timeIntervalSince($0) > reprobeInterval }) ?? true {
            Attempt.lastProbed = Date()
            VoiceLog.note("re-probing voice processing (remembered: \(remembered.label))")
            return attempts
        }
        return [remembered] + attempts.filter {
            $0.voiceProcessing != remembered.voiceProcessing
                || $0.preferredDevice != remembered.preferredDevice
        }
    }

    private static func isConfigurationRefusal(_ error: any Error) -> Bool {
        (error as NSError).code == -10875
    }

    private static func describe(_ error: any Error) -> String {
        let nsError = error as NSError
        guard nsError.domain == NSOSStatusErrorDomain
            || nsError.domain == "com.apple.coreaudio.avfaudio" else {
            return nsError.localizedDescription
        }
        return "OSStatus \(nsError.code)"
    }

    private func start(
        micFormat: AVAudioFormat,
        deviceName: String?,
        voiceProcessing: Bool
    ) async throws -> AsyncStream<VoiceAudioBuffer> {

        let input = engine.inputNode

        if let deviceName, !deviceName.isEmpty, let device = Self.inputDevice(matching: deviceName),
           device != Self.defaultInputDevice() {
            select(device, on: input)
            try? await Task.sleep(for: Self.deviceSettleDelay)
        }

        if input.isVoiceProcessingEnabled != voiceProcessing {
            let clock = ContinuousClock.now
            if voiceProcessing {
                try input.setVoiceProcessingEnabled(true)
            } else {
                try? input.setVoiceProcessingEnabled(false)
            }
            VoiceLog.note("voice processing \(voiceProcessing) took \(clock.elapsedMilliseconds)ms")

            try? await Task.sleep(for: Self.voiceProcessingSettleDelay)
        }

        if voiceProcessing {
            input.voiceProcessingOtherAudioDuckingConfiguration =
                AVAudioVoiceProcessingOtherAudioDuckingConfiguration(
                    enableAdvancedDucking: false,
                    duckingLevel: .min
                )
        }

        let formatClock = ContinuousClock.now
        var inputFormat = input.outputFormat(forBus: 0)
        var polls = 0
        for _ in 0..<20 where inputFormat.sampleRate == 0 || inputFormat.channelCount == 0 {
            try? await Task.sleep(for: .milliseconds(50))
            inputFormat = input.outputFormat(forBus: 0)
            polls += 1
        }
        if polls > 0 {
            VoiceLog.note("input format settled after \(polls) polls, \(formatClock.elapsedMilliseconds)ms")
        }
        guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else {
            throw StartError.noInput
        }

        let mixer = engine.mainMixerNode
        let outputRate = voiceProcessing
            ? inputFormat.sampleRate : engine.outputNode.outputFormat(forBus: 0).sampleRate
        if let stereo = AVAudioFormat(standardFormatWithSampleRate: outputRate, channels: 2) {
            engine.connect(mixer, to: engine.outputNode, format: stereo)
        }

        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: playbackFormat)
        player.volume = max(0, min(1, playbackVolume))

        let (stream, continuation) = AsyncStream<VoiceAudioBuffer>.makeStream(
            bufferingPolicy: .bufferingNewest(64)
        )
        micContinuation = continuation

        let state = TapState(target: micFormat, levels: levels)

        input.installTap(onBus: 0, bufferSize: 4096, format: nil) { @Sendable buffer, _ in
            state.count += 1
            if state.count == 1 {
                VoiceLog.note(String(
                    format: "audio flowing: %.0fHz %dch",
                    buffer.format.sampleRate, buffer.format.channelCount
                ))
            }
            guard let converted = Self.convert(buffer, using: state) else {
                if state.count % 200 == 0 { VoiceLog.problem("conversion failed") }
                return
            }
            state.levels.note(peak: state.bufferPeak)

            if state.count % 60 == 0 {
                let peaks = state.channelPeaks.map { String(format: "%.3f", $0) }.joined(separator: "/")
                VoiceLog.note(String(
                    format: "levels after %d buffers: channels %@ -> mono %.3f",
                    state.count, peaks.isEmpty ? "n/a" : peaks, state.monoPeak
                ))
                state.channelPeaks = state.channelPeaks.map { _ in 0 }
                state.monoPeak = 0
            }
            continuation.yield(VoiceAudioBuffer(buffer: converted))
        }

        engine.prepare()
        var lastError: (any Error)?
        for attempt in 0..<Self.startAttempts {
            do {
                try engine.start()
                player.play()
                isRunning = true
                observeConfigurationChanges()
                return stream
            } catch {
                lastError = error

                if Self.isConfigurationRefusal(error) { break }
                guard attempt < Self.startRetryDelays.count else { break }
                try? await Task.sleep(for: Self.startRetryDelays[attempt])
            }
        }
        throw StartError.engineUnavailable(
            lastError?.localizedDescription ?? "unknown error"
        )
    }

    private func teardown(disablingVoiceProcessing: Bool = false) {
        engine.stop()
        engine.inputNode.removeTap(onBus: 0)
        micContinuation?.finish()
        micContinuation = nil
        if engine.attachedNodes.contains(player) {
            engine.disconnectNodeOutput(player)
            engine.detach(player)
        }
        if disablingVoiceProcessing {
            try? engine.inputNode.setVoiceProcessingEnabled(false)
        }
        isRunning = false
        scheduledBuffers = 0
    }

    func stop() {
        guard isRunning || configurationObserver != nil else { return }
        if let configurationObserver {
            NotificationCenter.default.removeObserver(configurationObserver)
            self.configurationObserver = nil
        }
        player.stop()

        teardown(disablingVoiceProcessing: true)
        isEchoCancelled = false
    }

    func schedule(_ buffers: [AVAudioPCMBuffer]) {
        guard isRunning, !buffers.isEmpty else { return }
        guard let joined = Self.join(buffers) else { return }
        schedule(joined)
    }

    func play(_ chime: VoiceChime) {
        guard isRunning else { return }
        guard let buffer = VoiceChimes.buffer(for: chime, format: playbackFormat) else { return }
        schedule(buffer, tracked: false)
    }

    private func schedule(_ buffer: AVAudioPCMBuffer, tracked: Bool = true) {
        guard isRunning, buffer.frameLength > 0 else { return }
        guard let ready = Self.convert(buffer, to: playbackFormat) else { return }
        if tracked { scheduledBuffers += 1 }

        let generation = playbackGeneration

        player.scheduleBuffer(ready, completionCallbackType: .dataPlayedBack) { @Sendable [weak self] _ in
            Task { @MainActor [weak self] in
                guard let self, tracked, generation == playbackGeneration else { return }
                scheduledBuffers = max(0, scheduledBuffers - 1)
                if scheduledBuffers == 0 { onPlaybackDrained?() }
            }
        }
    }

    func stopPlayback() {
        playbackGeneration &+= 1
        scheduledBuffers = 0
        player.stop()
        if isRunning { player.play() }
    }

    private nonisolated static func convert(
        _ buffer: AVAudioPCMBuffer,
        using state: TapState
    ) -> AVAudioPCMBuffer? {

        state.bufferPeak = 0
        let source = downmix(buffer, state: state) ?? measure(buffer, state: state)
        if state.converter == nil || state.converter?.inputFormat != source.format {
            state.converter = AVAudioConverter(from: source.format, to: state.target)
            state.converter?.primeMethod = .none
        }
        guard let converter = state.converter else { return nil }
        return run(converter, on: source, to: state.target)
    }

    private nonisolated static func join(_ buffers: [AVAudioPCMBuffer]) -> AVAudioPCMBuffer? {
        guard let first = buffers.first else { return nil }
        guard buffers.count > 1 else { return first }
        let format = first.format
        guard buffers.allSatisfy({ $0.format == format }) else { return first }

        let frames = buffers.reduce(AVAudioFrameCount(0)) { $0 + $1.frameLength }
        guard frames > 0,
              let joined = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: frames)
        else { return first }
        joined.frameLength = frames

        let channels = Int(format.channelCount)
        var offset = 0
        if let destination = joined.floatChannelData {
            for buffer in buffers {
                guard let source = buffer.floatChannelData else { continue }
                let count = Int(buffer.frameLength)
                for channel in 0..<channels {
                    memcpy(
                        destination[channel] + offset,
                        source[channel],
                        count * MemoryLayout<Float>.size
                    )
                }
                offset += count
            }
            return joined
        }
        if let destination = joined.int16ChannelData {
            for buffer in buffers {
                guard let source = buffer.int16ChannelData else { continue }
                let count = Int(buffer.frameLength)
                for channel in 0..<channels {
                    memcpy(
                        destination[channel] + offset,
                        source[channel],
                        count * MemoryLayout<Int16>.size
                    )
                }
                offset += count
            }
            return joined
        }
        return first
    }

    private nonisolated static func measure(
        _ buffer: AVAudioPCMBuffer,
        state: TapState
    ) -> AVAudioPCMBuffer {
        guard let input = buffer.floatChannelData else { return buffer }
        var peak: Float = 0
        for frame in 0..<Int(buffer.frameLength) {
            peak = max(peak, abs(input[0][frame]))
        }
        state.bufferPeak = peak
        state.monoPeak = max(state.monoPeak, peak)
        return buffer
    }

    private nonisolated static func downmix(
        _ buffer: AVAudioPCMBuffer,
        state: TapState
    ) -> AVAudioPCMBuffer? {
        let channels = Int(buffer.format.channelCount)
        guard channels > 1, let input = buffer.floatChannelData else { return nil }
        guard let format = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: buffer.format.sampleRate,
            channels: 1,
            interleaved: false
        ), let mono = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: buffer.frameLength) else {
            return nil
        }
        mono.frameLength = buffer.frameLength
        guard let output = mono.floatChannelData else { return nil }

        if state.channelPeaks.count != channels {
            state.channelPeaks = [Float](repeating: 0, count: channels)
        }

        let gain = 1 / Float(channels)
        for frame in 0..<Int(buffer.frameLength) {
            var sum: Float = 0
            for channel in 0..<channels {
                let sample = input[channel][frame]
                sum += sample
                state.channelPeaks[channel] = max(state.channelPeaks[channel], abs(sample))
            }
            let mixed = max(-1, min(1, sum * gain))
            output[0][frame] = mixed
            state.bufferPeak = max(state.bufferPeak, abs(mixed))
        }
        state.monoPeak = max(state.monoPeak, state.bufferPeak)
        return mono
    }

    private nonisolated static func convert(
        _ buffer: AVAudioPCMBuffer,
        to format: AVAudioFormat
    ) -> AVAudioPCMBuffer? {
        if buffer.format == format { return buffer }
        guard let converter = AVAudioConverter(from: buffer.format, to: format) else { return nil }
        converter.primeMethod = .none
        return run(converter, on: buffer, to: format)
    }

    private nonisolated static func run(
        _ converter: AVAudioConverter,
        on buffer: AVAudioPCMBuffer,
        to format: AVAudioFormat
    ) -> AVAudioPCMBuffer? {
        let ratio = format.sampleRate / buffer.format.sampleRate
        let capacity = AVAudioFrameCount(Double(buffer.frameLength) * ratio) + 1024
        guard let output = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: capacity) else {
            return nil
        }
        var error: NSError?
        let source = ConversionSource(buffer)
        converter.convert(to: output, error: &error) { _, status in
            if source.consumed {
                status.pointee = .noDataNow
                return nil
            }
            source.consumed = true
            status.pointee = .haveData
            return source.buffer
        }
        guard error == nil, output.frameLength > 0 else { return nil }
        return output
    }

    private func observeConfigurationChanges() {
        configurationObserver = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange,
            object: engine,
            queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated {
                self?.onConfigurationChange?()
            }
        }
    }

    nonisolated static func uid(_ device: AudioDeviceID) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyDeviceUID,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var uid: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &uid) == noErr else {
            return nil
        }
        return uid?.takeRetainedValue() as String?
    }

    nonisolated static func defaultInputDevice() -> AudioDeviceID? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var device = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &device
        ) == noErr else { return nil }
        return device
    }

    private func select(_ deviceID: AudioDeviceID, on input: AVAudioInputNode) {
        guard let unit = input.audioUnit else { return }
        var identifier = deviceID
        AudioUnitSetProperty(
            unit,
            kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global,
            0,
            &identifier,
            UInt32(MemoryLayout<AudioDeviceID>.size)
        )
    }

    nonisolated static func inputDevice(matching match: String) -> AudioDeviceID? {
        var size = UInt32(0)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        guard AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size
        ) == noErr else { return nil }

        let count = Int(size) / MemoryLayout<AudioDeviceID>.size
        var devices = [AudioDeviceID](repeating: 0, count: count)
        guard AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &devices
        ) == noErr else { return nil }

        for device in devices where hasInput(device) {
            guard let deviceName = name(of: device) else { continue }
            if deviceName.localizedCaseInsensitiveContains(match) { return device }
        }
        return nil
    }

    private nonisolated static func hasInput(_ device: AudioDeviceID) -> Bool {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreams,
            mScope: kAudioObjectPropertyScopeInput,
            mElement: kAudioObjectPropertyElementMain
        )
        var size = UInt32(0)
        return AudioObjectGetPropertyDataSize(device, &address, 0, nil, &size) == noErr && size > 0
    }

    private nonisolated static func name(of device: AudioDeviceID) -> String? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioObjectPropertyName,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var name: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(device, &address, 0, nil, &size, &name) == noErr else {
            return nil
        }
        return name?.takeRetainedValue() as String?
    }
}
