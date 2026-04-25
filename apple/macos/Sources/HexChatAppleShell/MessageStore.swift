import Foundation

/// Persistence backbone for `ChatMessage`. Mirrors the Phase 6 `PersistenceStore`
/// shape: a small protocol with an in-memory implementation for tests and an
/// SQLite implementation for production. `conversation` is passed explicitly
/// instead of being derived from the message because the system pseudo-session
/// has no `ConversationKey` to derive — the controller owns the routing decision.
protocol MessageStore {
    func append(_ message: ChatMessage, conversation: ConversationKey) throws
    func page(conversation: ConversationKey, before: Date?, limit: Int) throws
        -> [ChatMessage]
    func count(conversation: ConversationKey) throws -> Int
}

final class InMemoryMessageStore: MessageStore {
    private var byConversation: [ConversationKey: [ChatMessage]] = [:]
    private var seenIDs: Set<UUID> = []

    init() {}

    func append(_ message: ChatMessage, conversation: ConversationKey) throws {
        guard !seenIDs.contains(message.id) else { return }
        seenIDs.insert(message.id)
        var bucket = byConversation[conversation] ?? []
        // Insertion-sort by timestamp so out-of-order arrivals (chathistory
        // back-fill in Phase 7.5) land in the right slot.
        let insertAt = bucket.firstIndex { $0.timestamp > message.timestamp } ?? bucket.endIndex
        bucket.insert(message, at: insertAt)
        byConversation[conversation] = bucket
    }

    func page(conversation: ConversationKey, before: Date?, limit: Int) throws -> [ChatMessage] {
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
        byConversation[conversation]?.count ?? 0
    }
}
