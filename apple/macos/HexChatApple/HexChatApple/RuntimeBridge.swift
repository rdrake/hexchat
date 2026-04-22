import AppleAdapterBridge
import Foundation

struct RuntimeEvent {
    let kind: hc_apple_event_kind
    let text: String?
    let phase: hc_apple_lifecycle_phase
}

protocol RuntimeClient {
    func start(noAuto: Bool, skipPlugins: Bool, onEvent: @escaping (RuntimeEvent) -> Void) -> Bool
    func stop()
    func postCommand(_ command: String) -> Bool
}

private final class RuntimeCallbackContext {
    let generation: UInt64
    weak var client: LiveRuntimeClient?

    init(generation: UInt64, client: LiveRuntimeClient) {
        self.generation = generation
        self.client = client
    }
}

final class LiveRuntimeClient: RuntimeClient {
    private let lock = NSLock()
    private var nextGeneration: UInt64 = 0
    private var activeGeneration: UInt64?
    private var onEvent: ((RuntimeEvent) -> Void)?
    private var callbackContexts: [UInt64: RuntimeCallbackContext] = [:]

    deinit {
        stop()
        lock.lock()
        callbackContexts.removeAll()
        onEvent = nil
        activeGeneration = nil
        lock.unlock()
    }

    func start(noAuto: Bool, skipPlugins: Bool, onEvent: @escaping (RuntimeEvent) -> Void) -> Bool {
        lock.lock()
        let generation = nextGeneration &+ 1
        nextGeneration = generation
        let previousGeneration = activeGeneration
        let previousOnEvent = self.onEvent
        let context = RuntimeCallbackContext(generation: generation, client: self)
        callbackContexts[generation] = context
        activeGeneration = generation
        self.onEvent = onEvent
        lock.unlock()

        let config = hc_apple_runtime_config(
            config_dir: nil,
            no_auto: noAuto ? 1 : 0,
            skip_plugins: skipPlugins ? 1 : 0
        )

        let userdata = Unmanaged.passUnretained(context).toOpaque()
        let started = withUnsafePointer(to: config) { configPtr in
            hc_apple_runtime_start(configPtr, runtimeEventCallback, userdata)
        }
        if started == 0 {
            lock.lock()
            callbackContexts[generation] = nil
            activeGeneration = previousGeneration
            self.onEvent = previousOnEvent
            lock.unlock()
            return false
        }
        return true
    }

    func stop() {
        hc_apple_runtime_stop()
    }

    func postCommand(_ command: String) -> Bool {
        command.withCString { hc_apple_runtime_post_command($0) != 0 }
    }

    fileprivate func receive(_ event: RuntimeEvent, generation: UInt64) {
        DispatchQueue.main.async { [weak self] in
            self?.deliver(event, generation: generation)
        }
    }

    private func deliver(_ event: RuntimeEvent, generation: UInt64) {
        lock.lock()
        guard generation == activeGeneration else {
            if event.kind == HC_APPLE_EVENT_LIFECYCLE && event.phase == HC_APPLE_LIFECYCLE_STOPPED {
                callbackContexts[generation] = nil
            }
            lock.unlock()
            return
        }

        let sink = onEvent
        let isStopped = event.kind == HC_APPLE_EVENT_LIFECYCLE && event.phase == HC_APPLE_LIFECYCLE_STOPPED
        if isStopped {
            activeGeneration = nil
            onEvent = nil
            callbackContexts[generation] = nil
        }
        lock.unlock()

        guard let sink else {
            return
        }
        sink(event)
    }
}

private let runtimeEventCallback: hc_apple_event_cb = { eventPtr, userData in
    guard let eventPtr, let userData else {
        return
    }

    let context = Unmanaged<RuntimeCallbackContext>.fromOpaque(userData).takeUnretainedValue()
    guard let client = context.client else {
        return
    }
    let raw = eventPtr.pointee
    client.receive(
        RuntimeEvent(
            kind: raw.kind,
            text: raw.text.map { String(cString: $0) },
            phase: raw.lifecycle_phase
        ),
        generation: context.generation
    )
}
