import Foundation

protocol PersistenceStore {
    func load() throws -> AppState?
    func save(_ state: AppState) throws
}

final class InMemoryPersistenceStore: PersistenceStore {
    private var cached: AppState?
    init(initial: AppState? = nil) { cached = initial }
    func load() throws -> AppState? { cached }
    func save(_ state: AppState) throws { cached = state }
}

final class FileSystemPersistenceStore: PersistenceStore {
    let fileURL: URL

    init(fileURL: URL) {
        self.fileURL = fileURL
    }

    convenience init() {
        // Deterministic path: macOS always has a writable
        // ~/Library/Application Support; if it does not, persistence should fail
        // loudly through the load/save throw rather than silently fall back to
        // /tmp and lose data on reboot.
        let url = URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)
            .appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("Application Support", isDirectory: true)
            .appendingPathComponent("HexChat", isDirectory: true)
            .appendingPathComponent("state.json")
        self.init(fileURL: url)
    }

    func load() throws -> AppState? {
        let data: Data
        do {
            data = try Data(contentsOf: fileURL)
        } catch let error as CocoaError
            where error.code == .fileNoSuchFile || error.code == .fileReadNoSuchFile
        {
            return nil
        }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return try decoder.decode(AppState.self, from: data)
    }

    func save(_ state: AppState) throws {
        let parent = fileURL.deletingLastPathComponent()
        // Idempotent — safer than a fileExists check that races with concurrent creation.
        try FileManager.default.createDirectory(
            at: parent, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(state)
        try data.write(to: fileURL, options: .atomic)
    }
}
