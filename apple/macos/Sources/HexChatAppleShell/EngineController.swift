import Foundation
import Observation
import AppleAdapterBridge

@Observable
final class EngineController {
    var isRunning = false
    var logs: [String] = []
    var input = ""

    private var callbackUserdata: UnsafeMutableRawPointer?

    func start() {
        let config = hc_apple_runtime_config(config_dir: nil, no_auto: 1, skip_plugins: 1)
        callbackUserdata = Unmanaged.passUnretained(self).toOpaque()
        withUnsafePointer(to: config) { configPtr in
            _ = hc_apple_runtime_start(configPtr, engineEventCallback, callbackUserdata)
        }
    }

    func stop() {
        hc_apple_runtime_stop()
    }

    func send(_ command: String) {
        let trimmed = command.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            return
        }
        logs.append("> \(trimmed)")
        trimmed.withCString { cString in
            _ = hc_apple_runtime_post_command(cString)
        }
    }
}

private let engineEventCallback: hc_apple_event_cb = { _, _ in
    // Callback trampoline is implemented in Task 4.
}
