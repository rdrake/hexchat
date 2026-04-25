#if canImport(XCTest)
import XCTest

@testable import HexChatAppleShell

final class FileSystemPersistenceStoreTests: XCTestCase {
    var scratchDir: URL!

    override func setUpWithError() throws {
        scratchDir = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(
            at: scratchDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: scratchDir)
    }

    func testLoadReturnsNilWhenFileAbsent() throws {
        let store = FileSystemPersistenceStore(
            fileURL: scratchDir.appendingPathComponent("state.json"))
        XCTAssertNil(try store.load())
    }

    func testSaveThenLoadRoundTrip() throws {
        let url = scratchDir.appendingPathComponent("state.json")
        let store = FileSystemPersistenceStore(fileURL: url)
        let state = AppState(
            networks: [Network(id: UUID(), displayName: "X")],
            commandHistory: ["/a"]
        )
        try store.save(state)
        XCTAssertTrue(FileManager.default.fileExists(atPath: url.path))
        XCTAssertEqual(try store.load(), state)
    }

    func testSaveCreatesMissingParentDirectory() throws {
        let url =
            scratchDir
            .appendingPathComponent("nested", isDirectory: true)
            .appendingPathComponent("dir", isDirectory: true)
            .appendingPathComponent("state.json")
        let store = FileSystemPersistenceStore(fileURL: url)
        try store.save(AppState())
        XCTAssertTrue(FileManager.default.fileExists(atPath: url.path))
    }

    func testLoadThrowsOnCorruptJSON() throws {
        let url = scratchDir.appendingPathComponent("state.json")
        try Data("{ not json }".utf8).write(to: url)
        let store = FileSystemPersistenceStore(fileURL: url)
        XCTAssertThrowsError(try store.load())
    }

    func testWriteIsAtomic() throws {
        let url = scratchDir.appendingPathComponent("state.json")
        let store = FileSystemPersistenceStore(fileURL: url)
        try store.save(AppState(commandHistory: ["first"]))
        try store.save(AppState(commandHistory: ["second"]))
        let loaded = try store.load()
        XCTAssertEqual(loaded?.commandHistory, ["second"])
        let entries = try FileManager.default.contentsOfDirectory(
            at: scratchDir, includingPropertiesForKeys: nil)
        XCTAssertEqual(entries.map { $0.lastPathComponent }.sorted(), ["state.json"])
    }
}
#endif
