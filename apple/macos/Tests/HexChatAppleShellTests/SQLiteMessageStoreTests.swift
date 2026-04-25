#if canImport(XCTest)
import SQLite3
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
        XCTAssertEqual(try store.userVersion(), 2)
    }

    func testReopenIsIdempotent() throws {
        let url = scratchDir.appendingPathComponent("messages.sqlite")
        let first = try SQLiteMessageStore(fileURL: url)
        XCTAssertEqual(try first.userVersion(), 2)
        // Drop our reference; the file's PRAGMA user_version stays at 2 on reopen.
        let second = try SQLiteMessageStore(fileURL: url)
        XCTAssertEqual(try second.userVersion(), 2)
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

    // MARK: - Phase 7.5 task-1a: schema v2 + serverMsgID + Bool-returning append

    private func writeV1Database(at url: URL, sentinelNetwork: UUID, sentinelChannel: String)
        throws
    {
        var raw: OpaquePointer?
        let flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
        guard sqlite3_open_v2(url.path, &raw, flags, nil) == SQLITE_OK, let db = raw else {
            XCTFail("could not synthesise v1 db")
            return
        }
        defer { sqlite3_close_v2(db) }

        let v1Schema = """
            CREATE TABLE messages (
                id              TEXT PRIMARY KEY,
                network_id      TEXT NOT NULL,
                channel_lower   TEXT NOT NULL,
                channel_display TEXT NOT NULL,
                timestamp_ms    INTEGER NOT NULL,
                kind            TEXT NOT NULL,
                body            TEXT,
                extra_json      TEXT,
                author_nick     TEXT,
                raw             TEXT NOT NULL
            );
            CREATE INDEX idx_messages_conv_ts ON messages
                (network_id, channel_lower, timestamp_ms DESC);
            PRAGMA user_version = 1;
            """
        XCTAssertEqual(sqlite3_exec(db, v1Schema, nil, nil, nil), SQLITE_OK)

        let insert = """
            INSERT INTO messages
                (id, network_id, channel_lower, channel_display, timestamp_ms,
                 kind, body, extra_json, author_nick, raw)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """
        var stmt: OpaquePointer?
        XCTAssertEqual(sqlite3_prepare_v2(db, insert, -1, &stmt, nil), SQLITE_OK)
        let sqliteTransient = unsafeBitCast(
            OpaquePointer(bitPattern: -1), to: sqlite3_destructor_type.self)
        sqlite3_bind_text(stmt, 1, UUID().uuidString, -1, sqliteTransient)
        sqlite3_bind_text(stmt, 2, sentinelNetwork.uuidString, -1, sqliteTransient)
        sqlite3_bind_text(stmt, 3, sentinelChannel.lowercased(), -1, sqliteTransient)
        sqlite3_bind_text(stmt, 4, sentinelChannel, -1, sqliteTransient)
        sqlite3_bind_int64(stmt, 5, 1_700_000_000_000)
        sqlite3_bind_text(stmt, 6, "message", -1, sqliteTransient)
        sqlite3_bind_text(stmt, 7, "v1 sentinel", -1, sqliteTransient)
        sqlite3_bind_null(stmt, 8)
        sqlite3_bind_text(stmt, 9, "alice", -1, sqliteTransient)
        sqlite3_bind_text(stmt, 10, "v1 sentinel raw", -1, sqliteTransient)
        XCTAssertEqual(sqlite3_step(stmt), SQLITE_DONE)
        sqlite3_finalize(stmt)
    }

    func testV1MigratesToV2PreservingRows() throws {
        let url = scratchDir.appendingPathComponent("messages.sqlite")
        let netID = UUID()
        try writeV1Database(at: url, sentinelNetwork: netID, sentinelChannel: "#a")

        let store = try SQLiteMessageStore(fileURL: url)
        XCTAssertEqual(try store.userVersion(), 2)
        let key = ConversationKey(networkID: netID, channel: "#a")
        XCTAssertEqual(try store.count(conversation: key), 1)
    }

    func testAppendReturnsTrueOnInsert() throws {
        let store = try freshStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let m = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            author: nil, timestamp: Date(timeIntervalSince1970: 1_700_000_000),
            serverMsgID: "abc")
        XCTAssertTrue(try store.append(m, conversation: key))
    }

    func testAppendReturnsFalseOnDuplicateServerMsgID() throws {
        let store = try freshStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let m = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            author: nil, timestamp: Date(timeIntervalSince1970: 1_700_000_000),
            serverMsgID: "abc")
        XCTAssertTrue(try store.append(m, conversation: key))
        // Same (network, channel, msgid, timestamp): UNIQUE INDEX rejects.
        let dup = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            author: nil, timestamp: Date(timeIntervalSince1970: 1_700_000_000),
            serverMsgID: "abc")
        XCTAssertFalse(try store.append(dup, conversation: key))
        XCTAssertEqual(try store.count(conversation: key), 1)
    }

    func testSameServerMsgIDDifferentTimestampInsertsBoth() throws {
        let store = try freshStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let m1 = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            timestamp: Date(timeIntervalSince1970: 1_700_000_000), serverMsgID: "abc")
        let m2 = ChatMessage(
            sessionID: UUID(), raw: "y", kind: .message(body: "y"),
            timestamp: Date(timeIntervalSince1970: 1_700_000_001), serverMsgID: "abc")
        XCTAssertTrue(try store.append(m1, conversation: key))
        XCTAssertTrue(try store.append(m2, conversation: key))
        XCTAssertEqual(try store.count(conversation: key), 2)
    }

    func testSameServerMsgIDDifferentChannelInsertsBoth() throws {
        let store = try freshStore()
        let netID = UUID()
        let keyA = ConversationKey(networkID: netID, channel: "#a")
        let keyB = ConversationKey(networkID: netID, channel: "#b")
        // Distinct ChatMessage IDs — two physical rows, same logical msgid that
        // legitimately appears under two channels in the same server's history.
        let mA = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            timestamp: Date(timeIntervalSince1970: 1_700_000_000), serverMsgID: "abc")
        let mB = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            timestamp: Date(timeIntervalSince1970: 1_700_000_000), serverMsgID: "abc")
        XCTAssertTrue(try store.append(mA, conversation: keyA))
        XCTAssertTrue(try store.append(mB, conversation: keyB))
        XCTAssertEqual(try store.count(conversation: keyA), 1)
        XCTAssertEqual(try store.count(conversation: keyB), 1)
    }

    func testSameServerMsgIDDifferentNetworkInsertsBoth() throws {
        let store = try freshStore()
        let keyA = ConversationKey(networkID: UUID(), channel: "#a")
        let keyB = ConversationKey(networkID: UUID(), channel: "#a")
        let mA = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            timestamp: Date(timeIntervalSince1970: 1_700_000_000), serverMsgID: "abc")
        let mB = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            timestamp: Date(timeIntervalSince1970: 1_700_000_000), serverMsgID: "abc")
        XCTAssertTrue(try store.append(mA, conversation: keyA))
        XCTAssertTrue(try store.append(mB, conversation: keyB))
    }

    func testNullServerMsgIDDoesNotDedup() throws {
        // Untagged rows fall outside the partial UNIQUE INDEX and rely on the
        // PRIMARY KEY (id) for dedup, so two distinct ChatMessages with nil
        // serverMsgID and identical other fields both insert.
        let store = try freshStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let m1 = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            timestamp: Date(timeIntervalSince1970: 1_700_000_000), serverMsgID: nil)
        let m2 = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            timestamp: Date(timeIntervalSince1970: 1_700_000_000), serverMsgID: nil)
        XCTAssertTrue(try store.append(m1, conversation: key))
        XCTAssertTrue(try store.append(m2, conversation: key))
        XCTAssertEqual(try store.count(conversation: key), 2)
    }

    func testServerMsgIDRoundTrip() throws {
        let store = try freshStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let m = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .message(body: "x"),
            author: MessageAuthor(nick: "alice", userID: nil),
            timestamp: Date(timeIntervalSince1970: 1_700_000_000),
            serverMsgID: "abc")
        XCTAssertTrue(try store.append(m, conversation: key))
        let page = try store.page(conversation: key, before: nil, limit: 5)
        XCTAssertEqual(page.count, 1)
        XCTAssertEqual(page[0].serverMsgID, "abc")
    }
}
#endif
