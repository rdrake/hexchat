import Foundation

/// Persistence backbone for `ChatMessage`. Mirrors the Phase 6 `PersistenceStore`
/// shape: a small protocol with an in-memory implementation for tests and an
/// SQLite implementation for production. `conversation` is passed explicitly
/// instead of being derived from the message because the system pseudo-session
/// has no `ConversationKey` to derive — the controller owns the routing decision.
///
/// `append` returns `true` for an inserted row, `false` for a duplicate ignored
/// at the storage layer (PRIMARY KEY collision on `id`, or the partial UNIQUE
/// INDEX on `(network_id, channel_lower, server_msgid, timestamp_ms)` for
/// msgid-tagged rows). Phase 7.5's invariant: `EngineController.append` mutates
/// its in-memory ring only when the store reports `true`.
protocol MessageStore: Sendable {
    @discardableResult
    func append(_ message: ChatMessage, conversation: ConversationKey) throws -> Bool
    func page(conversation: ConversationKey, before: Date?, limit: Int) throws
        -> [ChatMessage]
    func count(conversation: ConversationKey) throws -> Int
}

/// Thread-safe in-memory message store. Phase 7.5 makes writes synchronous on
/// the main actor (the Phase 7 background queue is gone), so the lock guards
/// against any future async page() callers; current call sites are all
/// main-actor-bound.
final class InMemoryMessageStore: MessageStore, @unchecked Sendable {
    /// Storage-layer dedup key. Mirrors SQLite's
    /// `(network_id, channel_lower, server_msgid, timestamp_ms)` partial
    /// UNIQUE INDEX so the in-memory store rejects the same set of duplicates
    /// the production store does — tests behave identically against either.
    private struct ServerKey: Hashable {
        let networkID: UUID
        let channelLowercased: String
        let serverMsgID: String
        let timestampMs: Int64
    }

    private let lock = NSLock()
    private var byConversation: [ConversationKey: [ChatMessage]] = [:]
    private var seenIDs: Set<UUID> = []
    private var seenServerKeys: Set<ServerKey> = []

    init() {}

    @discardableResult
    func append(_ message: ChatMessage, conversation: ConversationKey) throws -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if seenIDs.contains(message.id) { return false }
        // Mirror SQLiteMessageStore.append's normalization: empty strings and
        // `pending:*` placeholders fall outside the dedup grain.
        if let msgid = message.serverMsgID, !msgid.isEmpty,
            !msgid.hasPrefix("pending:")
        {
            let key = ServerKey(
                networkID: conversation.networkID,
                channelLowercased: conversation.channel.lowercased(),
                serverMsgID: msgid,
                timestampMs: Int64(message.timestamp.timeIntervalSince1970 * 1000))
            if !seenServerKeys.insert(key).inserted { return false }
        }
        seenIDs.insert(message.id)
        var bucket = byConversation[conversation] ?? []
        // Insertion-sort by timestamp so out-of-order arrivals (chathistory
        // back-fill) land in the right slot.
        let insertAt = bucket.firstIndex { $0.timestamp > message.timestamp } ?? bucket.endIndex
        bucket.insert(message, at: insertAt)
        byConversation[conversation] = bucket
        return true
    }

    func page(conversation: ConversationKey, before: Date?, limit: Int) throws -> [ChatMessage] {
        lock.lock()
        defer { lock.unlock() }
        guard let bucket = byConversation[conversation] else { return [] }
        let candidates: [ChatMessage]
        if let before {
            candidates = bucket.filter { $0.timestamp < before }
        } else {
            candidates = bucket
        }
        return Array(candidates.suffix(limit))
    }

    func count(conversation: ConversationKey) throws -> Int {
        lock.lock()
        defer { lock.unlock() }
        return byConversation[conversation]?.count ?? 0
    }
}
