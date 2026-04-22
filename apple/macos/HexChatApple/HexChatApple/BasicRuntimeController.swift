import AppleAdapterBridge
import Foundation
import Observation

@MainActor
@Observable
final class BasicRuntimeController {
    var isRunning = false
    var logs: [String] = []
    var commandInput = ""

    @ObservationIgnored
    private let runtime: RuntimeClient

    private let logCap: Int

    init(runtime: RuntimeClient? = nil, logCap: Int = 1000) {
        self.runtime = runtime ?? LiveRuntimeClient()
        self.logCap = max(0, logCap)
    }

    func start() {
        guard !isRunning else {
            return
        }

        let started = runtime.start(noAuto: true, skipPlugins: true) { [weak self] event in
            self?.handleRuntimeEvent(event)
        }

        if !started {
            appendLog("! runtime start failed")
        }
    }

    func stop() {
        guard isRunning else {
            return
        }

        runtime.stop()
    }

    func sendCurrentCommand() {
        guard isRunning else {
            return
        }

        let command = commandInput.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !command.isEmpty else {
            return
        }

        appendLog("> \(command)")
        let posted = runtime.postCommand(command)
        if !posted {
            appendLog("! failed to send command")
            return
        }
        commandInput = ""
    }

    func appendLog(_ line: String) {
        logs.append(line)
        trimLogsIfNeeded()
    }

    private func handleRuntimeEvent(_ event: RuntimeEvent) {
        if event.kind == HC_APPLE_EVENT_LIFECYCLE {
            switch event.phase {
            case HC_APPLE_LIFECYCLE_READY:
                isRunning = true
            case HC_APPLE_LIFECYCLE_STOPPED:
                isRunning = false
            default:
                break
            }

            if let text = event.text, !text.isEmpty {
                appendLog("[\(phaseName(event.phase))] \(text)")
            }
            return
        }

        if let text = event.text, !text.isEmpty {
            appendLog(text)
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

    private func trimLogsIfNeeded() {
        guard logCap > 0 else {
            logs.removeAll(keepingCapacity: true)
            return
        }

        let overflow = logs.count - logCap
        if overflow > 0 {
            logs.removeFirst(overflow)
        }
    }
}
