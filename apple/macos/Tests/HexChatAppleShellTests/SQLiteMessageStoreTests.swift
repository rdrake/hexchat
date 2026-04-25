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

    // MARK: - Task 4: append

    private func freshStore() throws -> SQLiteMessageStore {
        try SQLiteMessageStore(fileURL: scratchDir.appendingPathComponent("messages.sqlite"))
    }

    private func msg(
        in key: ConversationKey, kind: ChatMessageKind = .message(body: "x"),
        body: String = "x",
        ts: TimeInterval = 1_700_000_000
    ) -> ChatMessage {
        ChatMessage(
            sessionID: UUID(), raw: body, kind: kind,
            author: MessageAuthor(nick: "alice", userID: nil),
            timestamp: Date(timeIntervalSince1970: ts))
    }

    func testAppendPersistsAcrossReopen() throws {
        let url = scratchDir.appendingPathComponent("messages.sqlite")
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let m = ChatMessage(
            sessionID: UUID(), raw: "raw text", kind: .message(body: "hello"),
            author: MessageAuthor(nick: "alice", userID: nil),
            timestamp: Date(timeIntervalSince1970: 1_700_000_000))
        do {
            let store = try SQLiteMessageStore(fileURL: url)
            try store.append(m, conversation: key)
            XCTAssertEqual(try store.count(conversation: key), 1)
        }
        let reopened = try SQLiteMessageStore(fileURL: url)
        XCTAssertEqual(try reopened.count(conversation: key), 1)
    }

    func testAppendDuplicateIDIsIdempotent() throws {
        let store = try freshStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let m = msg(in: key)
        try store.append(m, conversation: key)
        try store.append(m, conversation: key)
        XCTAssertEqual(try store.count(conversation: key), 1)
    }

    private func roundTripKindThroughSQLite(_ kind: ChatMessageKind) throws {
        let url = scratchDir.appendingPathComponent("rt-\(UUID().uuidString).sqlite")
        let key = ConversationKey(networkID: UUID(), channel: "#rt")
        let original = msg(in: key, kind: kind, body: "raw payload")
        do {
            let store = try SQLiteMessageStore(fileURL: url)
            try store.append(original, conversation: key)
        }
        let reopened = try SQLiteMessageStore(fileURL: url)
        let page = try reopened.page(conversation: key, before: nil, limit: 10)
        XCTAssertEqual(page.count, 1, "expected 1 row for kind \(kind)")
        let back = page[0]
        XCTAssertEqual(back.id, original.id)
        XCTAssertEqual(back.kind, original.kind, "kind mismatch for \(kind)")
        XCTAssertEqual(back.author?.nick, "alice")
        XCTAssertEqual(back.raw, "raw payload")
        XCTAssertEqual(
            back.timestamp.timeIntervalSince1970, original.timestamp.timeIntervalSince1970,
            accuracy: 0.001)
    }

    func testRoundTripsMessageKind() throws { try roundTripKindThroughSQLite(.message(body: "hi")) }
    func testRoundTripsNoticeKind() throws { try roundTripKindThroughSQLite(.notice(body: "fyi")) }
    func testRoundTripsActionKind() throws { try roundTripKindThroughSQLite(.action(body: "acts")) }
    func testRoundTripsCommandKind() throws {
        try roundTripKindThroughSQLite(.command(body: "/join"))
    }
    func testRoundTripsErrorKind() throws { try roundTripKindThroughSQLite(.error(body: "boom")) }
    func testRoundTripsLifecycleKind() throws {
        try roundTripKindThroughSQLite(.lifecycle(phase: "READY", body: "ready"))
    }
    func testRoundTripsJoinKind() throws { try roundTripKindThroughSQLite(.join) }
    func testRoundTripsPartKind() throws {
        try roundTripKindThroughSQLite(.part(reason: "bye"))
        try roundTripKindThroughSQLite(.part(reason: nil))
    }
    func testRoundTripsQuitKind() throws {
        try roundTripKindThroughSQLite(.quit(reason: "ping timeout"))
        try roundTripKindThroughSQLite(.quit(reason: nil))
    }
    func testRoundTripsKickKind() throws {
        try roundTripKindThroughSQLite(.kick(target: "bob", reason: "spam"))
        try roundTripKindThroughSQLite(.kick(target: "bob", reason: nil))
    }
    func testRoundTripsNickChangeKind() throws {
        try roundTripKindThroughSQLite(.nickChange(from: "alice", to: "alice_"))
    }
    func testRoundTripsModeChangeKind() throws {
        try roundTripKindThroughSQLite(.modeChange(modes: "+o", args: "alice"))
        try roundTripKindThroughSQLite(.modeChange(modes: "+i", args: nil))
    }
}
#endif
