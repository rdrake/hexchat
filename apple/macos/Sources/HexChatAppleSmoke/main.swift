import Foundation
import AppleAdapterBridge

final class SmokeState {
    private let lock = NSLock()
    private(set) var phases: [hc_apple_lifecycle_phase] = []
    let ready = DispatchSemaphore(value: 0)
    let stopped = DispatchSemaphore(value: 0)

    func push(_ phase: hc_apple_lifecycle_phase) {
        lock.lock()
        phases.append(phase)
        lock.unlock()
    }

    func currentPhases() -> [hc_apple_lifecycle_phase] {
        lock.lock()
        defer { lock.unlock() }
        return phases
    }
}

private func phaseName(_ phase: hc_apple_lifecycle_phase) -> String {
    switch phase {
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

let state = SmokeState()
let userdata = Unmanaged.passRetained(state).toOpaque()
defer {
    Unmanaged<SmokeState>.fromOpaque(userdata).release()
}

let callback: hc_apple_event_cb = { eventPtr, userData in
    guard let eventPtr, let userData else {
        return
    }
    let state = Unmanaged<SmokeState>.fromOpaque(userData).takeUnretainedValue()
    let event = eventPtr.pointee
    if event.kind == HC_APPLE_EVENT_LIFECYCLE {
        state.push(event.lifecycle_phase)
        if event.lifecycle_phase == HC_APPLE_LIFECYCLE_READY {
            state.ready.signal()
        } else if event.lifecycle_phase == HC_APPLE_LIFECYCLE_STOPPED {
            state.stopped.signal()
        }
    }
}

let config = hc_apple_runtime_config(config_dir: nil, no_auto: 1, skip_plugins: 1)
let started = withUnsafePointer(to: config) { configPtr in
    hc_apple_runtime_start(configPtr, callback, userdata)
}

guard started != 0 else {
    fputs("failed to start runtime\n", stderr)
    exit(2)
}

let readyResult = state.ready.wait(timeout: .now() + 15)
guard readyResult == .success else {
    hc_apple_runtime_stop()
    fputs("timeout waiting for READY lifecycle event\n", stderr)
    exit(4)
}

hc_apple_runtime_stop()

let waitResult = state.stopped.wait(timeout: .now() + 15)
guard waitResult == .success else {
    fputs("timeout waiting for STOPPED lifecycle event\n", stderr)
    exit(3)
}

let observed = state.currentPhases()
let expected: [hc_apple_lifecycle_phase] = [
    HC_APPLE_LIFECYCLE_STARTING,
    HC_APPLE_LIFECYCLE_READY,
    HC_APPLE_LIFECYCLE_STOPPING,
    HC_APPLE_LIFECYCLE_STOPPED,
]

let sequenceLine = observed.map(phaseName).joined(separator: ", ")
print(sequenceLine)

guard observed == expected else {
    fputs("missing expected lifecycle sequence\n", stderr)
    exit(1)
}
