import Foundation
import os

@MainActor
final class PersistenceCoordinator {
    private static let log = Logger(subsystem: "net.afternet.hexchat", category: "persistence")

    private let store: PersistenceStore
    private let debounceInterval: Duration
    private var pending: Task<Void, Never>?
    private let snapshot: @MainActor () -> AppState

    init(
        store: PersistenceStore,
        debounceInterval: Duration,
        snapshot: @escaping @MainActor () -> AppState
    ) {
        self.store = store
        self.debounceInterval = debounceInterval
        self.snapshot = snapshot
    }

    func markDirty() {
        pending?.cancel()
        let store = store
        let interval = debounceInterval
        pending = Task { @MainActor [weak self] in
            if interval > .zero {
                try? await Task.sleep(for: interval)
                if Task.isCancelled { return }
            }
            guard let self else { return }
            let state = self.snapshot()
            // Second cancellation check guards against `flushNow()` winning the race
            // after we've taken our snapshot but before the store accepts the write.
            if Task.isCancelled { return }
            do {
                try store.save(state)
            } catch {
                Self.log.error("debounced save failed: \(String(describing: error))")
            }
        }
    }

    /// Synchronous flush used on lifecycle STOPPED. A failure here means the
    /// last mutation is lost at shutdown — acceptable because the app is exiting.
    func flushNow() {
        pending?.cancel()
        pending = nil
        do {
            try store.save(snapshot())
        } catch {
            Self.log.error("final flush failed: \(String(describing: error))")
        }
    }
}
