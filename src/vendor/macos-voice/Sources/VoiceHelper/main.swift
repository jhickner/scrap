
import AppKit
import AVFoundation
import Foundation
import VoiceCore

var socketPath = ""
var settings = VoiceSettings.default
var args = CommandLine.arguments
var i = 1
while i < args.count {
    let value = i + 1 < args.count ? args[i + 1] : ""
    switch args[i] {
    case "--socket": socketPath = value; i += 1
    case "--voice": settings.voice = value.isEmpty ? nil : value; i += 1
    case "--rate": settings.rate = Float(value) ?? settings.rate; i += 1
    case "--silence": settings.silence = Double(value) ?? settings.silence; i += 1
    case "--volume":
        if let n = Float(value) { settings.volume = min(max(n, 0), 1) }
        i += 1
    case "--input": settings.preferredInput = value.isEmpty ? nil : value; i += 1
    default: break
    }
    i += 1
}
guard !socketPath.isEmpty else {
    FileHandle.standardError.write(Data("missing --socket\n".utf8))
    exit(2)
}

signal(SIGPIPE, SIG_IGN)

func unixAddress(_ path: String) -> sockaddr_un {
    var addr = sockaddr_un()
    addr.sun_family = sa_family_t(AF_UNIX)
    let capacity = MemoryLayout.size(ofValue: addr.sun_path)
    _ = withUnsafeMutablePointer(to: &addr.sun_path) {
        $0.withMemoryRebound(to: CChar.self, capacity: capacity) {
            strncpy($0, path, capacity - 1)
        }
    }
    return addr
}

let listener = socket(AF_UNIX, SOCK_STREAM, 0)
guard listener >= 0 else { exit(2) }
var addr = unixAddress(socketPath)
var bound = withUnsafePointer(to: &addr) {
    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
        bind(listener, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
    }
}
if bound != 0 && errno == EADDRINUSE {

    let probe = socket(AF_UNIX, SOCK_STREAM, 0)
    let live = withUnsafePointer(to: &addr) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            connect(probe, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
        }
    } == 0
    close(probe)
    if live { close(listener); exit(0) }
    unlink(socketPath)
    bound = withUnsafePointer(to: &addr) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            bind(listener, $0, socklen_t(MemoryLayout<sockaddr_un>.size))
        }
    }
}
guard bound == 0, listen(listener, 8) == 0 else { close(listener); exit(2) }
chmod(socketPath, 0o600)

private func socketIdentity(_ path: String) -> (dev_t, ino_t)? {
    var st = stat()
    guard stat(path, &st) == 0 else { return nil }
    return (st.st_dev, st.st_ino)
}
let ownedSocket = socketIdentity(socketPath)
let socketWatch = DispatchSource.makeTimerSource(queue: .main)
socketWatch.schedule(deadline: .now() + 2, repeating: 2)
socketWatch.setEventHandler {
    guard let owned = ownedSocket else { return }
    let now = socketIdentity(socketPath)
    if let now, now == owned { return }
    VoiceLog.note("socket path taken over by another helper; exiting")
    exit(0)
}
socketWatch.resume()

let writeLock = NSLock()
func send(_ line: String, to fd: Int32) {
    let flat = line.replacingOccurrences(of: "\n", with: "\\n")
    let data = Array((flat + "\n").utf8)
    writeLock.lock()
    _ = data.withUnsafeBytes { write(fd, $0.baseAddress!, data.count) }
    writeLock.unlock()
}
func unescape(_ text: String) -> String {
    text.replacingOccurrences(of: "\\n", with: "\n")
}

@MainActor
final class Session {
    let voice = VoiceController(settings: settings)
    var clients = VoiceClients()
    var active: Int32?
    var announced = false
    var emptyGeneration = 0

    init() {
        voice.onSend = { [weak self] text in self?.sendActive("T " + text) }
        voice.onInterrupt = { [weak self] in self?.sendActive("INTERRUPT") }
        voice.onDrop = { [weak self] in self?.sendActive("DROP") }
        voice.onHeard = { [weak self] text in self?.sendActive("P " + text) }
        voice.onSpeaking = { [weak self] speaking in
            self?.broadcast("SPEAKING " + (speaking ? "1" : "0"))
        }
        voice.onError = { [weak self] message in self?.broadcast("ERR " + message) }
        voice.onNotice = { [weak self] message in self?.broadcast("NOTE " + message) }
        voice.onMode = { [weak self] mode in
            guard let self else { return }
            sendActive("MODE \(mode)")
            if mode == .listening, !announced {
                announced = true
                broadcast("READY")
            }
        }
    }

    func add(_ fd: Int32) {
        emptyGeneration += 1
        clients.add(fd)
        updateMic()
        if announced { send("READY", to: fd) }
    }

    func remove(_ fd: Int32) {
        guard clients.remove(fd) else { return }
        if active == fd {
            if let leftover = voice.handoff() { send("T " + leftover, to: fd) }
            send("P ", to: fd)
            active = nil
        }
        close(fd)

        if !clients.isEmpty { updateMic() }
        if clients.isEmpty {
            emptyGeneration += 1
            let generation = emptyGeneration
            DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(500)) { [weak self] in
                guard let self, clients.isEmpty, emptyGeneration == generation else { return }
                quit()
            }
        }
    }

    func updateMic() {
        voice.setMic(clients.micWanted)
    }

    func broadcast(_ line: String) {
        for fd in clients.all { send(line, to: fd) }
    }

    func sendActive(_ line: String) {
        if let active { send(line, to: active) }
    }

    func focus(_ fd: Int32, _ focused: Bool) {
        guard clients.contains(fd) else { return }
        if focused {
            guard active != fd else { return }
            if let old = active {
                if let leftover = voice.handoff() { send("T " + leftover, to: old) }
                send("P ", to: old)
            } else {
                _ = voice.handoff()
            }
            active = fd
            voice.setRate(clients.rate(fd, fallback: settings.rate))
            send("P ", to: fd)
        } else if active == fd {
            if let leftover = voice.handoff() { send("T " + leftover, to: fd) }
            send("P ", to: fd)
            active = nil
        }
    }

    func handle(_ line: String, from fd: Int32) {
        let parts = line.split(separator: " ", maxSplits: 1, omittingEmptySubsequences: false)
        let verb = parts.first.map(String.init) ?? ""
        let rest = parts.count > 1 ? String(parts[1]) : ""
        switch verb {
        case "FOCUS": focus(fd, rest == "1")
        case "MIC":
            if rest == "0 all" {
                for other in clients.micOffAll(from: fd) { send("MIC 0", to: other) }
            } else {
                clients.setMic(fd, rest == "1")
            }
            updateMic()
        case "QUIT": remove(fd)

        case "SHUTDOWN": quit()
        case "VOLUME":
            if let n = Float(rest) { voice.setVolume(n) }
        case "RATE":
            if let n = Float(rest), n.isFinite {
                clients.setRate(fd, n)
                if active == fd { voice.setRate(clients.rate(fd, fallback: settings.rate)) }
            }
        case "SILENCE":
            if let n = Double(rest) { voice.setSilence(n) }
        case "ANNOUNCE":
            if !rest.isEmpty {
                voice.setRate(clients.rate(fd, fallback: settings.rate))
                voice.announce(unescape(rest))
                if let active { voice.setRate(clients.rate(active, fallback: settings.rate)) }
            }
        default:
            guard active == fd else { return }
            switch verb {
            case "SAY": voice.appendReply(unescape(rest))
            case "FINISH": voice.finishReply()
            case "CANCEL": voice.cancelReply()
            case "MUTE": voice.stopSpeaking()
            case "BUSY": voice.setBusy(rest == "1")
            case "CHIME":
                if let chime = VoiceChime(rawValue: rest) { voice.play(chime) }
            default: send("ERR unknown command: \(verb)", to: fd)
            }
        }
    }

    func quit() {
        voice.stopConversation()
        close(listener)
        unlink(socketPath)
        exit(0)
    }
}

let app = NSApplication.shared
app.setActivationPolicy(.accessory)
let session = Session()

let readClient: @Sendable (Int32) -> Void = { fd in
    var buffer = [UInt8]()
    var chunk = [UInt8](repeating: 0, count: 4096)
    while true {
        let n = read(fd, &chunk, chunk.count)
        if n <= 0 { break }
        buffer.append(contentsOf: chunk[0..<n])
        while let nl = buffer.firstIndex(of: 10) {
            let line = String(decoding: buffer[0..<nl], as: UTF8.self)
            buffer.removeSubrange(0...nl)
            DispatchQueue.main.async { session.handle(line, from: fd) }
        }
    }
    DispatchQueue.main.async { session.remove(fd) }
}

let accepter = Thread {
    while true {
        let fd = accept(listener, nil, nil)
        if fd < 0 {
            if errno == EINTR { continue }
            break
        }
        _ = fcntl(fd, F_SETFD, FD_CLOEXEC)
        DispatchQueue.main.sync { session.add(fd) }
        Thread { readClient(fd) }.start()
    }
}
accepter.start()

let voiceAuthorized = DispatchSemaphore(value: 0)
AVSpeechSynthesizer.requestPersonalVoiceAuthorization { status in
    VoiceLog.note("personal voice authorization: \(status.rawValue) (3 = authorized)")
    voiceAuthorized.signal()
}
DispatchQueue.global().async {
    if voiceAuthorized.wait(timeout: .now() + 10) == .timedOut {
        VoiceLog.note("personal voice authorization still pending; starting anyway")
    }
    DispatchQueue.main.async { session.voice.startConversation() }
}
app.run()
