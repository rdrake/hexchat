import Foundation
import SQLite3

enum SQLiteMessageStoreError: Error {
    case openFailed(code: Int32, message: String)
    case prepareFailed(code: Int32, message: String, sql: String)
    case stepFailed(code: Int32, message: String, sql: String)
    case migrationFailed(currentVersion: Int)
}

/// SQLite-backed `MessageStore`. Owns one connection; not thread-safe by itself
/// — callers serialize through a dedicated dispatch queue (Phase 7 task 6).
///
/// PRAGMA tuning: WAL journal mode for write-during-read concurrency, NORMAL
/// synchronous (the JSON state.json takes the durability slot for tier-1 data),
/// MEMORY temp_store, foreign_keys ON for forward-compat.
final class SQLiteMessageStore: MessageStore {
    static let currentSchemaVersion: Int32 = 1

    let fileURL: URL
    private let db: OpaquePointer

    init(fileURL: URL) throws {
        self.fileURL = fileURL
        try FileManager.default.createDirectory(
            at: fileURL.deletingLastPathComponent(), withIntermediateDirectories: true)

        var handle: OpaquePointer?
        let flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX
        let rc = sqlite3_open_v2(fileURL.path, &handle, flags, nil)
        guard rc == SQLITE_OK, let handle else {
            let msg = handle.map { String(cString: sqlite3_errmsg($0)) } ?? "open failed"
            handle.map { sqlite3_close_v2($0) }
            throw SQLiteMessageStoreError.openFailed(code: rc, message: msg)
        }
        self.db = handle

        do {
            try Self.applyConnectionPragmas(db)
            try Self.migrateIfNeeded(db)
        } catch {
            sqlite3_close_v2(db)
            throw error
        }
    }

    deinit {
        sqlite3_close_v2(db)
    }

    func userVersion() throws -> Int {
        var stmt: OpaquePointer?
        let sql = "PRAGMA user_version"
        guard sqlite3_prepare_v2(db, sql, -1, &stmt, nil) == SQLITE_OK, let stmt else {
            throw SQLiteMessageStoreError.prepareFailed(
                code: sqlite3_errcode(db),
                message: String(cString: sqlite3_errmsg(db)),
                sql: sql)
        }
        defer { sqlite3_finalize(stmt) }
        guard sqlite3_step(stmt) == SQLITE_ROW else { return 0 }
        return Int(sqlite3_column_int(stmt, 0))
    }

    // MARK: - MessageStore (stubs filled in Tasks 4 & 5)

    func append(_ message: ChatMessage, conversation: ConversationKey) throws {
        // Task 4 fills this in.
        _ = (message, conversation)
    }

    func page(conversation: ConversationKey, before: Date?, limit: Int) throws -> [ChatMessage] {
        // Task 5 fills this in.
        _ = (conversation, before, limit)
        return []
    }

    func count(conversation: ConversationKey) throws -> Int {
        // Task 5 fills this in.
        _ = conversation
        return 0
    }

    // MARK: - Setup

    private static func applyConnectionPragmas(_ db: OpaquePointer) throws {
        for pragma in [
            "PRAGMA journal_mode = WAL",
            "PRAGMA synchronous = NORMAL",
            "PRAGMA temp_store = MEMORY",
            "PRAGMA foreign_keys = ON",
        ] {
            try exec(db, pragma)
        }
    }

    private static func migrateIfNeeded(_ db: OpaquePointer) throws {
        let current = try readUserVersion(db)
        if current == 0 {
            try exec(
                db,
                """
                CREATE TABLE IF NOT EXISTS messages (
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
                )
                """)
            try exec(
                db,
                """
                CREATE INDEX IF NOT EXISTS idx_messages_conv_ts
                ON messages (network_id, channel_lower, timestamp_ms DESC)
                """)
            try exec(db, "PRAGMA user_version = \(currentSchemaVersion)")
            return
        }
        if Int32(current) != currentSchemaVersion {
            throw SQLiteMessageStoreError.migrationFailed(currentVersion: current)
        }
    }

    private static func readUserVersion(_ db: OpaquePointer) throws -> Int {
        var stmt: OpaquePointer?
        let sql = "PRAGMA user_version"
        guard sqlite3_prepare_v2(db, sql, -1, &stmt, nil) == SQLITE_OK, let stmt else {
            throw SQLiteMessageStoreError.prepareFailed(
                code: sqlite3_errcode(db),
                message: String(cString: sqlite3_errmsg(db)),
                sql: sql)
        }
        defer { sqlite3_finalize(stmt) }
        guard sqlite3_step(stmt) == SQLITE_ROW else { return 0 }
        return Int(sqlite3_column_int(stmt, 0))
    }

    private static func exec(_ db: OpaquePointer, _ sql: String) throws {
        var err: UnsafeMutablePointer<CChar>?
        let rc = sqlite3_exec(db, sql, nil, nil, &err)
        if rc != SQLITE_OK {
            let msg = err.map { String(cString: $0) } ?? "exec failed"
            sqlite3_free(err)
            throw SQLiteMessageStoreError.stepFailed(code: rc, message: msg, sql: sql)
        }
    }
}
