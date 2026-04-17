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
        guard !isRunning else {
            return
        }
        let config = hc_apple_runtime_config(config_dir: nil, no_auto: 1, skip_plugins: 1)
        callbackUserdata = Unmanaged.passUnretained(self).toOpaque()
        withUnsafePointer(to: config) { configPtr in
            let started = hc_apple_runtime_start(configPtr, engineEventCallback, callbackUserdata)
            if started == 0 {
                logs.append("! runtime start failed")
            }
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
            let code = hc_apple_runtime_post_command(cString)
            if code == 0 {
                logs.append("! failed to post command")
            }
        }
    }

    func handleRuntimeEvent(_ event: RuntimeEvent) {
        switch event.kind {
        case HC_APPLE_EVENT_LOG_LINE:
            if let text = event.text {
                logs.append(text)
            }
        case HC_APPLE_EVENT_LIFECYCLE:
            if let text = event.text {
                logs.append("[\(event.phase.name)] \(text)")
            }
            if event.phase == HC_APPLE_LIFECYCLE_READY {
                isRunning = true
            } else if event.phase == HC_APPLE_LIFECYCLE_STOPPED {
                isRunning = false
            }
        case HC_APPLE_EVENT_COMMAND:
            if event.code != 0 {
                let commandText = event.text ?? ""
                logs.append("! command rejected (\(event.code)): \(commandText)")
            }
        default:
            break
        }
    }
}

private struct RuntimeEvent {
    let kind: hc_apple_event_kind
    let text: String?
    let phase: hc_apple_lifecycle_phase
    let code: Int32
}

private extension hc_apple_lifecycle_phase {
    var name: String {
        switch self {
        case HC_APPLE_LIFECYCLE_STARTING:
            return "STARTING"
        case HC_APPLE_LIFECYCLE_READY:
            return "READY"
        case HC_APPLE_LIFECYCLE_STOPPING:
            return "STOPPING"
        case HC_APPLE_LIFECYCLE_STOPPED:
            return "STOPPED"
        default:
            return "UNKNOWN"
        }
    }
}

private func makeRuntimeEvent(from pointer: UnsafePointer<hc_apple_event>) -> RuntimeEvent {
    let raw = pointer.pointee
    let copiedText: String?
    if let textPtr = raw.text {
        copiedText = String(cString: textPtr)
    } else {
        copiedText = nil
    }
    return RuntimeEvent(kind: raw.kind, text: copiedText, phase: raw.lifecycle_phase, code: Int32(raw.code))
}

private let engineEventCallback: hc_apple_event_cb = { eventPtr, userData in
    guard let eventPtr, let userData else {
        return
    }

    // Copy C payloads immediately on the engine thread before hopping threads.
    let event = makeRuntimeEvent(from: eventPtr)
    let controller = Unmanaged<EngineController>.fromOpaque(userData).takeUnretainedValue()
    Task { @MainActor in
        controller.handleRuntimeEvent(event)
    }
}
