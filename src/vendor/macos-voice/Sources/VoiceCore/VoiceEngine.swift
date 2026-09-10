import AVFoundation
import Foundation

/// One buffer of audio crossing a thread boundary. `AVAudioPCMBuffer` is a reference type the
/// SDK does not mark `Sendable`; every buffer sent through here is freshly converted and never
/// touched again by the side that produced it, so handing it over is safe.
struct VoiceAudioBuffer: @unchecked Sendable {
    let buffer: AVAudioPCMBuffer
}

/// The microphone's recent loudness, written from the render thread and read from the main
/// actor. Kept as a short history of per-buffer peaks so a caller can ask how loud the last
/// second or two was — which is what separates a child talking over the Primer from the
/// residue of its own voice that echo cancellation leaves behind.
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

    /// The loudest buffer within the last `seconds`.
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

/// Mutable state owned by the audio render thread. The tap block is called serially, and
/// nothing else reads these, so the unchecked conformance is the accurate description.
/// One buffer handed to an `AVAudioConverter`. The SDK types the input block as `@Sendable`
/// even though it is called synchronously, before `convert` returns, on the calling thread.
private final class ConversionSource: @unchecked Sendable {
    let buffer: AVAudioPCMBuffer
    var consumed = false
    init(_ buffer: AVAudioPCMBuffer) { self.buffer = buffer }
}

private final class TapState: @unchecked Sendable {
    var converter: AVAudioConverter?
    var count = 0
    /// Highest sample seen on each input channel, so a silent channel is distinguishable from
    /// a silent microphone.
    var channelPeaks: [Float] = []
    var monoPeak: Float = 0
    /// Peak of the buffer being converted right now, before it is folded into the window.
    var bufferPeak: Float = 0
    let target: AVAudioFormat
    let levels: LevelMeter
    init(target: AVAudioFormat, levels: LevelMeter) {
        self.target = target
        self.levels = levels
    }
}

/// The single `AVAudioEngine` that both hears and speaks.
///
/// Input and output have to live on the same engine. Apple's voice processing unit cancels the
/// echo of what *this engine* renders; a reply spoken through the ordinary system output path
/// is not part of that reference signal and comes straight back through the microphone. That
/// one constraint is why playback is synthesized to buffers and scheduled on a player node
/// here rather than handed to `AVSpeechSynthesizer.speak`.
@MainActor
final class VoiceEngine {
    enum StartError: LocalizedError {
        case noInput
        case engineUnavailable(String)
        /// Every wiring was refused. Carries what each one returned, because which of them
        /// failed and how is the whole diagnosis.
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

    /// A freshly connected Bluetooth device is often still busy on the first attempt. The wait
    /// grows rather than being a flat 250ms each time: a device that is merely busy is ready
    /// within a few tens of milliseconds, and paying the long wait up front was most of the
    /// time a failed wiring cost before the next one was tried.
    private static let startAttempts = 6
    private static let startRetryDelays: [Duration] = [
        .milliseconds(60), .milliseconds(120), .milliseconds(240),
        .milliseconds(400), .milliseconds(600),
    ]
    /// Long enough for a Bluetooth input switch to land before the node's format is read.
    private static let deviceSettleDelay = Duration.milliseconds(600)
    /// A pause after the IO unit is rebuilt for voice processing, before starting into it.
    private static let voiceProcessingSettleDelay = Duration.milliseconds(150)

    private let engine = AVAudioEngine()
    private let player = AVAudioPlayerNode()
    private var micContinuation: AsyncStream<VoiceAudioBuffer>.Continuation?
    /// How loud the microphone has been lately.
    let levels = LevelMeter()
    private var configurationObserver: (any NSObjectProtocol)?
    private var scheduledBuffers = 0
    private var playbackGeneration = 0
    /// Whether the graph is live. Playback only reaches the speaker through here while it is,
    /// so a caller that only wants replies read aloud has to take the direct route instead.
    private(set) var isRunning = false
    /// False when the engine only came up with voice processing off. The Primer is then
    /// audible to its own microphone, so the caller has to stay strictly half-duplex.
    private(set) var isEchoCancelled = false

    /// Called when the audio route changes under us — AirPods going to sleep, a case opening,
    /// another device taking them. The controller rebuilds rather than trying to patch up a
    /// graph whose formats have moved.
    var onConfigurationChange: (() -> Void)?
    /// Called once the last scheduled buffer has actually been heard, which is when the echo
    /// gate can start counting.
    var onPlaybackDrained: (() -> Void)?
    /// Scales already-scheduled playback as well as new buffers. 0 is silence, 1 is full.
    var playbackVolume: Float = 1 {
        didSet { player.volume = max(0, min(1, playbackVolume)) }
    }

    var playbackFormat: AVAudioFormat {
        engine.mainMixerNode.outputFormat(forBus: 0)
    }

    var isSpeaking: Bool { scheduledBuffers > 0 }

    /// One way of wiring the graph. Voice processing is what makes barge-in possible, and a
    /// named input device is what puts the child's headset ahead of the built-in microphone —
    /// but either can be the reason the unit refuses to initialise, so they are tried in
    /// descending order of what they buy rather than assumed to work.
    private struct Attempt {
        let voiceProcessing: Bool
        let preferredDevice: Bool

        var label: String {
            "\(voiceProcessing ? "echo-cancelled" : "plain")/\(preferredDevice ? "named" : "default")"
        }

        /// Bumped whenever the graph is wired differently, so a downgrade remembered under the
        /// old wiring is not carried into the new one.
        private static let wiringVersion = 2

        /// Keyed by input device: what works with AirPods is not what works with the built-in
        /// microphone, and the child switches between them.
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

        /// When the full list was last tried on this device.
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

    /// The default device comes first. Setting the input device explicitly rebuilds the IO
    /// unit in a way voice processing refuses (-10875), and the child's headset is normally
    /// already the system default — so the override is a fallback for when it is not, not the
    /// preferred path.
    private static let attempts = [
        Attempt(voiceProcessing: true, preferredDevice: false),
        Attempt(voiceProcessing: true, preferredDevice: true),
        Attempt(voiceProcessing: false, preferredDevice: false),
        Attempt(voiceProcessing: false, preferredDevice: true),
    ]

    /// Starts capture and playback, returning the stream of microphone audio converted to
    /// `micFormat`. `preferredInputName` selects a device by name substring — "AirPods" — so
    /// the child's headset is used even when it is not the system default.
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

    /// How long a remembered downgrade stands before voice processing is tried again.
    static let reprobeInterval: TimeInterval = 24 * 60 * 60

    /// The wiring that worked last time on this input device, first.
    ///
    /// Which wirings a Mac accepts is a property of its hardware, not something to rediscover
    /// every time the microphone is switched on. Whether voice processing comes up varies
    /// between machines and headsets, and finding out costs seconds — enabling it alone is
    /// well over a second, and undoing it more.
    ///
    /// The exception is once a day. Voice processing is worth real money in behaviour —
    /// it is the difference between being able to talk over the Primer and not — and it has
    /// proved intermittent on the same hardware, so a remembered downgrade is retried now and
    /// then rather than being settled forever by one bad attempt. Not once per launch: on a
    /// Mac that refuses it, that made the first press after every launch wait six seconds.
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

    /// `kAudioUnitErr_FailedInitialization`: this graph cannot be built, as opposed to a
    /// device that is momentarily busy. Worth distinguishing, because only one of the two is
    /// worth waiting for.
    private static func isConfigurationRefusal(_ error: any Error) -> Bool {
        (error as NSError).code == -10875
    }

    /// OSStatus codes are what actually identify a Core Audio failure; the localized string
    /// for all of them is the same sentence about an operation that could not be completed.
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
        // Touch the input node first: instantiating it is what creates the IO unit that
        // everything below is configured against.
        let input = engine.inputNode

        // Only pay the settle wait when the device actually changes. When the child's headset
        // is already the system default — the normal case — there is nothing to wait for.
        if let deviceName, !deviceName.isEmpty, let device = Self.inputDevice(matching: deviceName),
           device != Self.defaultInputDevice() {
            select(device, on: input)
            try? await Task.sleep(for: Self.deviceSettleDelay)
        }

        // Enabling voice processing rebuilds the IO unit with different formats, so it has to
        // happen before anything reads a format or is connected to the mixer.
        // Toggling this renegotiates the Bluetooth link and costs well over a second. Both
        // voice-processed wirings want it on, so leave it on between them rather than paying
        // for it again on every attempt.
        if input.isVoiceProcessingEnabled != voiceProcessing {
            let clock = ContinuousClock.now
            if voiceProcessing {
                try input.setVoiceProcessingEnabled(true)
            } else {
                try? input.setVoiceProcessingEnabled(false)
            }
            VoiceLog.note("voice processing \(voiceProcessing) took \(clock.elapsedMilliseconds)ms")
            // The unit is rebuilt underneath; starting into it immediately is what returns
            // -10875 often enough to cost a whole retry cycle.
            try? await Task.sleep(for: Self.voiceProcessingSettleDelay)
        }

        // Voice processing assumes it owns the foreground conversation and, by default,
        // turns down every other audio stream on the Mac. That includes music and video in
        // unrelated apps for the entire time this always-listening engine is running. Keep
        // ducking fixed at Apple's minimum so enabling Primer does not make those apps surge
        // up and down while retaining the echo cancellation that full-duplex speech needs.
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

        // Wire the mixer to the output explicitly. Left to itself the engine connects them
        // with the unit's default client format, and the voice-processing unit then refuses
        // to initialise (-10875, "client-side input and output formats do not match")
        // because its microphone side runs at the hardware rate. Stereo at the input's rate
        // satisfies it and is what the speaker wants anyway.
        let mixer = engine.mainMixerNode
        let outputRate = voiceProcessing
            ? inputFormat.sampleRate : engine.outputNode.outputFormat(forBus: 0).sampleRate
        if let stereo = AVAudioFormat(standardFormatWithSampleRate: outputRate, channels: 2) {
            engine.connect(mixer, to: engine.outputNode, format: stereo)
        }

        // Connect the player with the mixer's own format. Passing nil here would take the
        // player's default, which after a voice-processing rebuild no longer matches.
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: playbackFormat)
        player.volume = max(0, min(1, playbackVolume))

        let (stream, continuation) = AsyncStream<VoiceAudioBuffer>.makeStream(
            bufferingPolicy: .bufferingNewest(64)
        )
        micContinuation = continuation

        // Tapping with `format: nil` makes the node use its own current format. Passing an
        // explicit format that does not match the hardware throws an Objective-C exception
        // that cannot be caught, so the converter is built from the first real buffer instead.
        let state = TapState(target: micFormat, levels: levels)
        // Explicitly `@Sendable`: the SDK does not mark the tap block, so it would otherwise
        // inherit this type's main-actor isolation and trap on the audio thread the first time
        // a buffer arrives. Everything it captures is already safe to hand across.
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
            // Levels over a window, not from the first buffer: the engine has just started
            // then, so buffer one is always silence and says nothing about the microphone.
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
                // A wiring the unit refuses is refused identically every time — every
                // successful start in the logs came on the first try, and every failure
                // exhausted its retries. Waiting only delays trying something that can work.
                if Self.isConfigurationRefusal(error) { break }
                guard attempt < Self.startRetryDelays.count else { break }
                try? await Task.sleep(for: Self.startRetryDelays[attempt])
            }
        }
        throw StartError.engineUnavailable(
            lastError?.localizedDescription ?? "unknown error"
        )
    }

    /// Returns the graph to the state a fresh attempt expects. `disablingVoiceProcessing` is
    /// false between attempts — each toggle is another second of Bluetooth renegotiation, and
    /// the next attempt usually wants it on anyway.
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
        // Here it does turn voice processing off, which matters beyond tidiness: leaving it on
        // holds the Bluetooth link in its two-way codec, and that is what makes music sound
        // worse after the child has left the conversation.
        teardown(disablingVoiceProcessing: true)
        isEchoCancelled = false
    }

    /// Queues one utterance. The synthesizer hands back many small buffers; they are joined
    /// into one before conversion because converting each separately restarts the resampler at
    /// every boundary, and those discontinuities are audible as a grainy, crackling voice.
    func schedule(_ buffers: [AVAudioPCMBuffer]) {
        guard isRunning, !buffers.isEmpty else { return }
        guard let joined = Self.join(buffers) else { return }
        schedule(joined)
    }

    /// Plays an acknowledgement tone, generated at the format the graph is already running at.
    /// Untracked: a chime is not speech, so it must not hold `isSpeaking` true or trigger the
    /// echo gate that follows a spoken reply.
    func play(_ chime: VoiceChime) {
        guard isRunning else { return }
        guard let buffer = VoiceChimes.buffer(for: chime, format: playbackFormat) else { return }
        schedule(buffer, tracked: false)
    }

    /// Queues one buffer. Buffers in any format are converted to the mixer's, so a voice that
    /// synthesizes at 22 kHz plays on 48 kHz hardware.
    private func schedule(_ buffer: AVAudioPCMBuffer, tracked: Bool = true) {
        guard isRunning, buffer.frameLength > 0 else { return }
        guard let ready = Self.convert(buffer, to: playbackFormat) else { return }
        if tracked { scheduledBuffers += 1 }
        // Stopping playback delivers the completion callbacks anyway, for audio that was never
        // heard. Without this generation check an interruption would report itself as a reply
        // that finished playing, and close the echo gate over the child mid-sentence.
        let generation = playbackGeneration
        // Also called off the main actor, and for the same reason must not inherit isolation.
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

    // MARK: - conversion
    //
    // Pure, and `nonisolated` because the audio tap calls them from the render thread. Bound
    // to the main actor they would be unreachable from exactly the place they exist for.

    private nonisolated static func convert(
        _ buffer: AVAudioPCMBuffer,
        using state: TapState
    ) -> AVAudioPCMBuffer? {
        // Voice processing hands back a multichannel input node, and `AVAudioConverter` will
        // not fold that down to the recognizer's mono for us — it takes the first channel,
        // which is not the one carrying the voice. Mix the channels first, then let the
        // converter do the one thing it is reliable at: changing the sample rate.
        state.bufferPeak = 0
        let source = downmix(buffer, state: state) ?? measure(buffer, state: state)
        if state.converter == nil || state.converter?.inputFormat != source.format {
            state.converter = AVAudioConverter(from: source.format, to: state.target)
            state.converter?.primeMethod = .none
        }
        guard let converter = state.converter else { return nil }
        return run(converter, on: source, to: state.target)
    }

    /// Concatenates same-format buffers into one. Returns nil if they disagree on format,
    /// which the synthesizer's output never does within a single utterance.
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

    /// Sums the input channels into one. Summed rather than averaged: on a voice-processed
    /// input only one channel carries the microphone and the rest are silent, so averaging
    /// would quietly attenuate the voice by the number of channels.
    /// The peak of an already-mono buffer, for the level meter.
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
        // Averaged, not summed: voice processing hands back the same signal on every channel,
        // and summing three copies clips the recognizer's input and triples any leaked reply.
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

    // MARK: - devices

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

    /// Points the input node at the first input device whose name contains `name`.
    /// The device's persistent identifier, which survives reboots and reconnections in a way
    /// the numeric `AudioDeviceID` does not.
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
