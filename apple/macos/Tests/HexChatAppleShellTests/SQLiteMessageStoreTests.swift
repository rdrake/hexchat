#if canImport(XCTest)
import XCTest

@testable import HexChatAppleShell

final class SQLiteMessageStoreTests: XCTestCase {
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

    func testOpenCreatesFileAtSchemaVersion1() throws {
        let url = scratchDir.appendingPathComponent("messages.sqlite")
        XCTAssertFalse(FileManager.default.fileExists(atPath: url.path))
        let store = try SQLiteMessageStore(fileURL: url)
        XCTAssertTrue(FileManager.default.fileExists(atPath: url.path))
        XCTAssertEqual(try store.userVersion(), 1)
    }

    func testReopenIsIdempotent() throws {
        let url = scratchDir.appendingPathComponent("messages.sqlite")
        let first = try SQLiteMessageStore(fileURL: url)
        XCTAssertEqual(try first.userVersion(), 1)
        // Drop our reference; the file's PRAGMA user_version stays at 1 on reopen.
        let second = try SQLiteMessageStore(fileURL: url)
        XCTAssertEqual(try second.userVersion(), 1)
    }

    func testOpenThrowsOnNonDatabaseFile() throws {
        let url = scratchDir.appendingPathComponent("messages.sqlite")
        try Data("not a database".utf8).write(to: url)
        XCTAssertThrowsError(try SQLiteMessageStore(fileURL: url))
    }
}
#endif
