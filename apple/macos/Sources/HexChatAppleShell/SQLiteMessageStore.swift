import Foundation
import SQLite3

enum SQLiteMessageStoreError: Error {
    case openFailed(code: Int32, message: String)
    case prepareFailed(code: Int32, message: String, sql: String)
    case stepFailed(code: Int32, message: String, sql: String)
    case migrationFailed(currentVersion: Int)
}

/// SQLite-backed `MessageStore`. Owns one connection opened with
/// `SQLITE_OPEN_FULLMUTEX`, which makes the connection internally serialised —
/// so concurrent calls from the main actor and the controller's write queue
/// are safe. The `@unchecked Sendable` is correct because the only mutable
/// state (`db`) is owned by SQLite's serialised mutex.
///
/// PRAGMA tuning: WAL journal mode for write-during-read concurrency, NORMAL
/// synchronous (the JSON state.json takes the durability slot for tier-1 data),
/// MEMORY temp_store, foreign_keys ON for forward-compat.
final class SQLiteMessageStore: MessageStore, @unchecked Sendable {
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
            if let handle { sqlite3_close_v2(handle) }
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

    // MARK: - MessageStore

    private static let insertSQL = """
        INSERT OR IGNORE INTO messages
            (id, network_id, channel_lower, channel_display, timestamp_ms,
             kind, body, extra_json, author_nick, raw)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """

    func append(_ message: ChatMessage, conversation: ConversationKey) throws {
        var stmt: OpaquePointer?
        guard sqlite3_prepare_v2(db, Self.insertSQL, -1, &stmt, nil) == SQLITE_OK, let stmt else {
            throw SQLiteMessageStoreError.prepareFailed(
                code: sqlite3_errcode(db),
                message: String(cString: sqlite3_errmsg(db)),
                sql: Self.insertSQL)
        }
        defer { sqlite3_finalize(stmt) }
        let timestampMs = Int64(message.timestamp.timeIntervalSince1970 * 1000)
        let payload = encodeKindPayload(message.kind)
        bind(stmt, index: 1, text: message.id.uuidString)
        bind(stmt, index: 2, text: conversation.networkID.uuidString)
        bind(stmt, index: 3, text: conversation.channel.lowercased())
        bind(stmt, index: 4, text: conversation.channel)
        sqlite3_bind_int64(stmt, 5, timestampMs)
        bind(stmt, index: 6, text: payload.tag)
        bind(stmt, index: 7, text: payload.body)
        bind(stmt, index: 8, text: payload.extraJSON)
        bind(stmt, index: 9, text: message.author?.nick)
        bind(stmt, index: 10, text: message.raw)
        let rc = sqlite3_step(stmt)
        guard rc == SQLITE_DONE else {
            throw SQLiteMessageStoreError.stepFailed(
                code: rc,
                message: String(cString: sqlite3_errmsg(db)),
                sql: Self.insertSQL)
        }
    }

    private static let pageSQL = """
        SELECT id, network_id, channel_display, timestamp_ms,
               kind, body, extra_json, author_nick, raw
        FROM messages
        WHERE network_id = ? AND channel_lower = ?
              AND (?3 = 0 OR timestamp_ms < ?4)
        ORDER BY timestamp_ms DESC, id DESC
        LIMIT ?
        """

    func page(conversation: ConversationKey, before: Date?, limit: Int) throws -> [ChatMessage] {
        guard limit > 0 else { return [] }
        var stmt: OpaquePointer?
        guard sqlite3_prepare_v2(db, Self.pageSQL, -1, &stmt, nil) == SQLITE_OK, let stmt else {
            throw SQLiteMessageStoreError.prepareFailed(
                code: sqlite3_errcode(db),
                message: String(cString: sqlite3_errmsg(db)),
                sql: Self.pageSQL)
        }
        defer { sqlite3_finalize(stmt) }
        bind(stmt, index: 1, text: conversation.networkID.uuidString)
        bind(stmt, index: 2, text: conversation.channel.lowercased())
        if let before {
            sqlite3_bind_int(stmt, 3, 1)
            sqlite3_bind_int64(stmt, 4, Int64(before.timeIntervalSince1970 * 1000))
        } else {
            sqlite3_bind_int(stmt, 3, 0)
            sqlite3_bind_int64(stmt, 4, 0)
        }
        sqlite3_bind_int(stmt, 5, Int32(limit))

        var rows: [ChatMessage] = []
        while sqlite3_step(stmt) == SQLITE_ROW {
            guard let row = decodeRow(stmt) else { continue }
            rows.append(row)
        }
        // ORDER BY DESC pulled newest first up to the limit; reverse for ascending.
        return rows.reversed()
    }

    func count(conversation: ConversationKey) throws -> Int {
        let sql = "SELECT COUNT(*) FROM messages WHERE network_id = ? AND channel_lower = ?"
        var stmt: OpaquePointer?
        guard sqlite3_prepare_v2(db, sql, -1, &stmt, nil) == SQLITE_OK, let stmt else {
            throw SQLiteMessageStoreError.prepareFailed(
                code: sqlite3_errcode(db),
                message: String(cString: sqlite3_errmsg(db)),
                sql: sql)
        }
        defer { sqlite3_finalize(stmt) }
        bind(stmt, index: 1, text: conversation.networkID.uuidString)
        bind(stmt, index: 2, text: conversation.channel.lowercased())
        guard sqlite3_step(stmt) == SQLITE_ROW else { return 0 }
        return Int(sqlite3_column_int64(stmt, 0))
    }

    // MARK: - Row encode/decode

    private struct KindPayload {
        let tag: String
        let body: String?
        let extraJSON: String?
    }

    private func encodeKindPayload(_ kind: ChatMessageKind) -> KindPayload {
        // Match the Codable shape from Task 1: discriminator + body + per-case extras.
        // Body lives in its own column; non-body extras land in a JSON blob.
        switch kind {
        case .message(let b): return KindPayload(tag: "message", body: b, extraJSON: nil)
        case .notice(let b): return KindPayload(tag: "notice", body: b, extraJSON: nil)
        case .action(let b): return KindPayload(tag: "action", body: b, extraJSON: nil)
        case .command(let b): return KindPayload(tag: "command", body: b, extraJSON: nil)
        case .error(let b): return KindPayload(tag: "error", body: b, extraJSON: nil)
        case .lifecycle(let phase, let body):
            return KindPayload(tag: "lifecycle", body: body, extraJSON: encodeJSON(["phase": phase]))
        case .join: return KindPayload(tag: "join", body: nil, extraJSON: nil)
        case .part(let reason):
            return KindPayload(
                tag: "part", body: nil,
                extraJSON: reason.map { encodeJSON(["reason": $0]) })
        case .quit(let reason):
            return KindPayload(
                tag: "quit", body: nil,
                extraJSON: reason.map { encodeJSON(["reason": $0]) })
        case .kick(let target, let reason):
            var extra: [String: String] = ["target": target]
            if let reason { extra["reason"] = reason }
            return KindPayload(tag: "kick", body: nil, extraJSON: encodeJSON(extra))
        case .nickChange(let from, let to):
            return KindPayload(
                tag: "nickChange", body: nil,
                extraJSON: encodeJSON(["from": from, "to": to]))
        case .modeChange(let modes, let args):
            var extra: [String: String] = ["modes": modes]
            if let args { extra["args"] = args }
            return KindPayload(tag: "modeChange", body: nil, extraJSON: encodeJSON(extra))
        }
    }

    private func decodeKindPayload(tag: String, body: String?, extraJSON: String?)
        -> ChatMessageKind?
    {
        let extra = extraJSON.flatMap(decodeJSON) ?? [:]
        switch tag {
        case "message": return body.map { .message(body: $0) }
        case "notice": return body.map { .notice(body: $0) }
        case "action": return body.map { .action(body: $0) }
        case "command": return body.map { .command(body: $0) }
        case "error": return body.map { .error(body: $0) }
        case "lifecycle":
            guard let body, let phase = extra["phase"] else { return nil }
            return .lifecycle(phase: phase, body: body)
        case "join": return .join
        case "part": return .part(reason: extra["reason"])
        case "quit": return .quit(reason: extra["reason"])
        case "kick":
            guard let target = extra["target"] else { return nil }
            return .kick(target: target, reason: extra["reason"])
        case "nickChange":
            guard let from = extra["from"], let to = extra["to"] else { return nil }
            return .nickChange(from: from, to: to)
        case "modeChange":
            guard let modes = extra["modes"] else { return nil }
            return .modeChange(modes: modes, args: extra["args"])
        default: return nil
        }
    }

    private func decodeRow(_ stmt: OpaquePointer) -> ChatMessage? {
        guard
            let idStr = columnText(stmt, 0), let id = UUID(uuidString: idStr),
            columnText(stmt, 1) != nil,  // network_id (already known by caller)
            let channelDisplay = columnText(stmt, 2)
        else { return nil }
        let timestampMs = sqlite3_column_int64(stmt, 3)
        guard let tag = columnText(stmt, 4) else { return nil }
        let body = columnText(stmt, 5)
        let extraJSON = columnText(stmt, 6)
        let authorNick = columnText(stmt, 7)
        guard let raw = columnText(stmt, 8) else { return nil }
        guard let kind = decodeKindPayload(tag: tag, body: body, extraJSON: extraJSON)
        else { return nil }
        _ = channelDisplay  // reserved for future use
        return ChatMessage(
            id: id,
            sessionID: UUID(),  // runtime; rehydrated by the controller from conversation key
            raw: raw,
            kind: kind,
            author: authorNick.map { MessageAuthor(nick: $0, userID: nil) },
            timestamp: Date(timeIntervalSince1970: TimeInterval(timestampMs) / 1000))
    }

    private func encodeJSON(_ dict: [String: String]) -> String {
        let enc = JSONEncoder()
        enc.outputFormatting = [.sortedKeys]
        guard let data = try? enc.encode(dict),
            let s = String(data: data, encoding: .utf8)
        else { return "{}" }
        return s
    }

    private func decodeJSON(_ s: String) -> [String: String]? {
        guard let data = s.data(using: .utf8) else { return nil }
        return try? JSONDecoder().decode([String: String].self, from: data)
    }

    // MARK: - SQLite binding helpers

    private func bind(_ stmt: OpaquePointer, index: Int32, text: String?) {
        if let text {
            // sqliteTransient is SQLite's SQLITE_TRANSIENT sentinel: tells SQLite
            // to copy the string before sqlite3_step returns, since the Swift
            // String could be freed before the row is finalised.
            let sqliteTransient = unsafeBitCast(
                OpaquePointer(bitPattern: -1), to: sqlite3_destructor_type.self)
            sqlite3_bind_text(stmt, index, text, -1, sqliteTransient)
        } else {
            sqlite3_bind_null(stmt, index)
        }
    }

    private func columnText(_ stmt: OpaquePointer, _ index: Int32) -> String? {
        guard let cstr = sqlite3_column_text(stmt, index) else { return nil }
        return String(cString: cstr)
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
