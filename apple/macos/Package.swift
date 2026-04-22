// swift-tools-version:5.10
import PackageDescription

let adapterLibDir = "../../builddir/src/fe-apple"

let package = Package(
    name: "HexChatApple",
    platforms: [
        .macOS("26.0"),
    ],
    products: [
        .executable(name: "HexChatAppleShell", targets: ["HexChatAppleShell"]),
        .executable(name: "HexChatAppleSmoke", targets: ["HexChatAppleSmoke"]),
        .executable(name: "HexChatAppleNetworkE2E", targets: ["HexChatAppleNetworkE2E"]),
    ],
    targets: [
        .target(
            name: "AppleAdapterBridge",
            path: "Sources/AppleAdapterBridge",
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("include"),
            ]
        ),
        .executableTarget(
            name: "HexChatAppleShell",
            dependencies: ["AppleAdapterBridge"],
            path: "Sources/HexChatAppleShell",
            linkerSettings: [
                .unsafeFlags([
                    "-L\(adapterLibDir)",
                    "-lhexchatappleadapter",
                    "-Xlinker", "-rpath",
                    "-Xlinker", adapterLibDir,
                ]),
            ]
        ),
        .executableTarget(
            name: "HexChatAppleSmoke",
            dependencies: ["AppleAdapterBridge"],
            path: "Sources/HexChatAppleSmoke",
            linkerSettings: [
                .unsafeFlags([
                    "-L\(adapterLibDir)",
                    "-lhexchatappleadapter",
                    "-Xlinker", "-rpath",
                    "-Xlinker", adapterLibDir,
                ]),
            ]
        ),
        .executableTarget(
            name: "HexChatAppleNetworkE2E",
            dependencies: ["AppleAdapterBridge"],
            path: "Sources/HexChatAppleNetworkE2E",
            linkerSettings: [
                .unsafeFlags([
                    "-L\(adapterLibDir)",
                    "-lhexchatappleadapter",
                    "-Xlinker", "-rpath",
                    "-Xlinker", adapterLibDir,
                ]),
            ]
        ),
        .testTarget(
            name: "HexChatAppleShellTests",
            dependencies: ["HexChatAppleShell"],
            path: "Tests/HexChatAppleShellTests"
        ),
    ]
)
