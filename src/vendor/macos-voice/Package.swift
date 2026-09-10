// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "VoiceHelper",
    platforms: [.macOS("26.0")],
    targets: [
        .target(name: "VoiceCore"),
        .executableTarget(name: "VoiceHelper", dependencies: ["VoiceCore"]),
        .testTarget(name: "VoiceHelperTests", dependencies: ["VoiceCore"]),
    ]
)
