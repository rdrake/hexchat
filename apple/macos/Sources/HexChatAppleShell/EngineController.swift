import AppleAdapterBridge
import CoreTransferable
import Foundation
import Observation
import SwiftUI
import UniformTypeIdentifiers
import os

struct ChatSession: Identifiable, Hashable, Codable, Transferable {
    let id: UUID
    var connectionID: UUID
    var channel: String
    var isActive: Bool
    var locator: SessionLocator

    init(
        id: UUID = UUID(),
        connectionID: UUID,
        channel: String,
        isActive: Bool,
        locator: SessionLocator? = nil
    ) {
        self.id = id
        self.connectionID = connectionID
        self.channel = channel
        self.isActive = isActive
        self.locator = locator ?? .composed(connectionID: connectionID, channel: channel)
    }

    var composedKey: String { locator.composedKey }

    var isChannel: Bool {
        channel.hasPrefix("#") || channel.hasPrefix("&")
    }

    var plainTextDescription: String { channel }

    /// Codable shape: `{ id, connectionID, channel, isActive }` — `locator` is
    /// intentionally NOT encoded. Transfer round-trips re-derive the locator on
    /// decode as `.composed(connectionID, channel)`, so a session originally
    /// created with `.runtime(id: …)` decodes with a different locator.
    ///
    /// This is safe for Phase 8 drop integrations because they use the
    /// preserved `id: UUID` to look up live sessions
    /// (`controller.sessions.contains { $0.id == dropped.id }`), NOT the locator.
    /// Future code that wants to resolve a transferred session via
    /// `sessionByLocator[…]` must reconcile this — `upsertSession` registers
    /// exactly one locator per session, so a lookup keyed on the decoded
    /// `.composed(…)` locator may miss a session originally registered as
    /// `.runtime(id:)`.
    private enum CodingKeys: String, CodingKey {
        case id, connectionID, channel, isActive
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let id = try c.decode(UUID.self, forKey: .id)
        let connectionID = try c.decode(UUID.self, forKey: .connectionID)
        let channel = try c.decode(String.self, forKey: .channel)
        let isActive = try c.decode(Bool.self, forKey: .isActive)
        self.init(id: id, connectionID: connectionID, channel: channel, isActive: isActive, locator: nil)
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(id, forKey: .id)
        try c.encode(connectionID, forKey: .connectionID)
        try c.encode(channel, forKey: .channel)
        try c.encode(isActive, forKey: .isActive)
    }

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
        ProxyRepresentation(exporting: \.plainTextDescription)
    }
}

struct NetworkSection: Identifiable {
    let id: String
    let name: String
    let sessions: [ChatSession]
}

/// Pairs the nick string from an originating IRC event with the optional stable
/// `User.id` UUID resolved via the `usersByConnectionAndNick` index. `userID` is
/// `nil` when the nick has not yet been seen on the connection — for example, a
/// typed JOIN arriving before its USERLIST_INSERT companion. The nick string
/// preserves the casing used in the originating IRC event.
struct MessageAuthor: Codable, Hashable {
    let nick: String
    let userID: UUID?
}

/// The typed payload carried by `ChatMessage`. Free-text cases (`.message`,
/// `.notice`, `.action`, `.command`, `.error`, `.lifecycle`) carry the displayable
/// body via the `body` computed property. Structured cases (`.join`, `.part`,
/// `.quit`, `.kick`, `.nickChange`, `.modeChange`) carry parameters and have no
/// free-form body — `body` returns `nil` for these. `.action` is included for
/// forward compatibility: Phase 5 does not emit it; Phase 7 will once typed
/// PRIVMSG/ACTION arrives.
enum ChatMessageKind: Codable, Hashable {
    case message(body: String)
    case notice(body: String)
    case action(body: String)
    case command(body: String)
    case error(body: String)
    case lifecycle(phase: String, body: String)
    case join
    case part(reason: String?)
    case quit(reason: String?)
    case kick(target: String, reason: String?)
    case nickChange(from: String, to: String)
    case modeChange(modes: String, args: String?)

    // Stable JSON shape: { tag: <discriminator>, body?, phase?, reason?, target?,
    // from?, to?, modes?, args? }. The same field set maps to SQLite's body+extra_json
    // pair in Phase 7 task-4 (SQLiteMessageStore.append).
    private enum CodingKeys: String, CodingKey {
        case tag, body, phase, reason, target, from, to, modes, args
    }

    private enum Tag: String, Codable {
        case message, notice, action, command, error, lifecycle
        case join, part, quit, kick, nickChange, modeChange
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let tag = try c.decode(Tag.self, forKey: .tag)
        switch tag {
        case .message: self = .message(body: try c.decode(String.self, forKey: .body))
        case .notice: self = .notice(body: try c.decode(String.self, forKey: .body))
        case .action: self = .action(body: try c.decode(String.self, forKey: .body))
        case .command: self = .command(body: try c.decode(String.self, forKey: .body))
        case .error: self = .error(body: try c.decode(String.self, forKey: .body))
        case .lifecycle:
            self = .lifecycle(
                phase: try c.decode(String.self, forKey: .phase),
                body: try c.decode(String.self, forKey: .body))
        case .join: self = .join
        case .part:
            self = .part(reason: try c.decodeIfPresent(String.self, forKey: .reason))
        case .quit:
            self = .quit(reason: try c.decodeIfPresent(String.self, forKey: .reason))
        case .kick:
            self = .kick(
                target: try c.decode(String.self, forKey: .target),
                reason: try c.decodeIfPresent(String.self, forKey: .reason))
        case .nickChange:
            self = .nickChange(
                from: try c.decode(String.self, forKey: .from),
                to: try c.decode(String.self, forKey: .to))
        case .modeChange:
            self = .modeChange(
                modes: try c.decode(String.self, forKey: .modes),
                args: try c.decodeIfPresent(String.self, forKey: .args))
        }
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .message(let b):
            try c.encode(Tag.message, forKey: .tag)
            try c.encode(b, forKey: .body)
        case .notice(let b):
            try c.encode(Tag.notice, forKey: .tag)
            try c.encode(b, forKey: .body)
        case .action(let b):
            try c.encode(Tag.action, forKey: .tag)
            try c.encode(b, forKey: .body)
        case .command(let b):
            try c.encode(Tag.command, forKey: .tag)
            try c.encode(b, forKey: .body)
        case .error(let b):
            try c.encode(Tag.error, forKey: .tag)
            try c.encode(b, forKey: .body)
        case .lifecycle(let phase, let body):
            try c.encode(Tag.lifecycle, forKey: .tag)
            try c.encode(phase, forKey: .phase)
            try c.encode(body, forKey: .body)
        case .join:
            try c.encode(Tag.join, forKey: .tag)
        case .part(let reason):
            try c.encode(Tag.part, forKey: .tag)
            try c.encodeIfPresent(reason, forKey: .reason)
        case .quit(let reason):
            try c.encode(Tag.quit, forKey: .tag)
            try c.encodeIfPresent(reason, forKey: .reason)
        case .kick(let target, let reason):
            try c.encode(Tag.kick, forKey: .tag)
            try c.encode(target, forKey: .target)
            try c.encodeIfPresent(reason, forKey: .reason)
        case .nickChange(let from, let to):
            try c.encode(Tag.nickChange, forKey: .tag)
            try c.encode(from, forKey: .from)
            try c.encode(to, forKey: .to)
        case .modeChange(let modes, let args):
            try c.encode(Tag.modeChange, forKey: .tag)
            try c.encode(modes, forKey: .modes)
            try c.encodeIfPresent(args, forKey: .args)
        }
    }
}

struct ChatMessage: Codable, Identifiable {
    let id: UUID
    let sessionID: UUID
    let raw: String
    let kind: ChatMessageKind
    let author: MessageAuthor?
    let timestamp: Date
    /// IRCv3 `msgid` tag, or nil for locally-generated messages and untagged
    /// PRIVMSGs. Combined with `(networkID, channel, timestamp)` it forms the
    /// dedup key for chathistory replays — see SQLiteMessageStore's partial
    /// UNIQUE INDEX. Empty strings and `pending:*` placeholders normalize to
    /// nil at the controller boundary; only "real" server msgids reach storage.
    let serverMsgID: String?

    init(
        id: UUID = UUID(),
        sessionID: UUID,
        raw: String,
        kind: ChatMessageKind,
        author: MessageAuthor? = nil,
        timestamp: Date = Date(),
        serverMsgID: String? = nil
    ) {
        self.id = id
        self.sessionID = sessionID
        self.raw = raw
        self.kind = kind
        self.author = author
        self.timestamp = timestamp
        self.serverMsgID = serverMsgID
    }

    var body: String? {
        switch kind {
        case .message(let b), .notice(let b), .action(let b),
            .command(let b), .error(let b), .lifecycle(_, let b):
            return b
        case .join, .part, .quit, .kick, .nickChange, .modeChange:
            return nil
        }
    }
}

extension ChatMessage: Transferable {
    var plainTextDescription: String { body ?? raw }

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
        ProxyRepresentation(exporting: \.plainTextDescription)
    }
}

/// View-facing DTO assembled on demand from `User` + `ChannelMembership` via the
/// computed `usersBySession` projection. `id` is `"\(connectionID)::\(nick.lowercased())"`,
/// which is unique per connection and case-insensitive on nick. Stable cross-
/// channel identity lives on `User.id` (UUID), looked up through
/// `usersByConnectionAndNick`.
struct ChatUser: Identifiable, Hashable, Codable, Transferable {
    var connectionID: UUID
    var nick: String
    var modePrefix: Character?
    var account: String?
    var host: String?
    var isMe: Bool
    var isAway: Bool

    init(
        connectionID: UUID,
        nick: String,
        modePrefix: Character? = nil,
        account: String? = nil,
        host: String? = nil,
        isMe: Bool = false,
        isAway: Bool = false
    ) {
        self.connectionID = connectionID
        self.nick = nick
        self.modePrefix = modePrefix
        self.account = account
        self.host = host
        self.isMe = isMe
        self.isAway = isAway
    }

    var id: String { "\(connectionID.uuidString.lowercased())::\(nick.lowercased())" }
    var plainTextDescription: String { nick }

    private enum CodingKeys: String, CodingKey {
        case connectionID, nick, modePrefix, account, host, isMe, isAway
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.connectionID = try c.decode(UUID.self, forKey: .connectionID)
        self.nick = try c.decode(String.self, forKey: .nick)
        if let s = try c.decodeIfPresent(String.self, forKey: .modePrefix) {
            guard s.count == 1 else {
                throw DecodingError.dataCorruptedError(
                    forKey: .modePrefix, in: c,
                    debugDescription: "modePrefix must be a single character")
            }
            self.modePrefix = s.first
        } else {
            self.modePrefix = nil
        }
        self.account = try c.decodeIfPresent(String.self, forKey: .account)
        self.host = try c.decodeIfPresent(String.self, forKey: .host)
        self.isMe = try c.decode(Bool.self, forKey: .isMe)
        self.isAway = try c.decode(Bool.self, forKey: .isAway)
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(connectionID, forKey: .connectionID)
        try c.encode(nick, forKey: .nick)
        try c.encodeIfPresent(modePrefix.map(String.init), forKey: .modePrefix)
        try c.encodeIfPresent(account, forKey: .account)
        try c.encodeIfPresent(host, forKey: .host)
        try c.encode(isMe, forKey: .isMe)
        try c.encode(isAway, forKey: .isAway)
    }

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
        ProxyRepresentation(exporting: \.plainTextDescription)
    }
}

struct ServerEndpoint: Codable, Hashable {
    var host: String
    var port: UInt16
    var useTLS: Bool
}

struct SASLConfig: Codable, Hashable {
    var mechanism: String
    var username: String
    // TODO(phase-6+): plaintext on disk. Move to Keychain before shipping.
    var password: String
}

struct ConversationKey: Codable, Hashable {
    let networkID: UUID
    let channel: String
    // Pre-lowered cache: `==` and `hash(into:)` are called on every dictionary lookup
    // along the per-message-append path, so we avoid allocating a fresh lowercased
    // String each time. Foundation case-folding, not RFC 1459 IRC casemapping —
    // server-advertised CASEMAPPING is a future-phase concern.
    private let channelLowercased: String

    init(networkID: UUID, channel: String) {
        self.networkID = networkID
        self.channel = channel
        self.channelLowercased = channel.lowercased()
    }

    private enum CodingKeys: String, CodingKey {
        case networkID, channel
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let net = try c.decode(UUID.self, forKey: .networkID)
        let ch = try c.decode(String.self, forKey: .channel)
        self.init(networkID: net, channel: ch)
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(networkID, forKey: .networkID)
        try c.encode(channel, forKey: .channel)
    }

    static func == (lhs: ConversationKey, rhs: ConversationKey) -> Bool {
        lhs.networkID == rhs.networkID && lhs.channelLowercased == rhs.channelLowercased
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(networkID)
        hasher.combine(channelLowercased)
    }
}

enum AppStateDecodingError: Error {
    case unsupportedSchemaVersion(Int)
}

struct AppState: Codable, Hashable {
    static let currentSchemaVersion = 1

    var schemaVersion: Int
    var networks: [Network]
    var conversations: [ConversationState]
    var lastFocusedKey: ConversationKey?
    var commandHistory: [String]

    init(
        schemaVersion: Int = AppState.currentSchemaVersion,
        networks: [Network] = [],
        conversations: [ConversationState] = [],
        lastFocusedKey: ConversationKey? = nil,
        commandHistory: [String] = []
    ) {
        self.schemaVersion = schemaVersion
        self.networks = networks
        self.conversations = conversations
        self.lastFocusedKey = lastFocusedKey
        self.commandHistory = commandHistory
    }

    private enum CodingKeys: String, CodingKey {
        case schemaVersion, networks, conversations, lastFocusedKey, commandHistory
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let version = try c.decode(Int.self, forKey: .schemaVersion)
        guard version == AppState.currentSchemaVersion else {
            throw AppStateDecodingError.unsupportedSchemaVersion(version)
        }
        self.schemaVersion = version
        self.networks = try c.decode([Network].self, forKey: .networks)
        self.conversations = try c.decode([ConversationState].self, forKey: .conversations)
        self.lastFocusedKey = try c.decodeIfPresent(ConversationKey.self, forKey: .lastFocusedKey)
        self.commandHistory = try c.decode([String].self, forKey: .commandHistory)
    }
}

struct ConversationState: Codable, Hashable {
    var key: ConversationKey
    var draft: String
    var unread: Int
    var lastReadAt: Date?

    init(
        key: ConversationKey,
        draft: String = "",
        unread: Int = 0,
        lastReadAt: Date? = nil
    ) {
        self.key = key
        self.draft = draft
        self.unread = unread
        self.lastReadAt = lastReadAt
    }
}

struct Network: Identifiable, Codable, Hashable {
    let id: UUID
    var displayName: String
    var servers: [ServerEndpoint]
    var nicks: [String]
    var sasl: SASLConfig?
    var autoConnect: Bool
    var autoJoin: [String]
    var onConnectCommands: [String]

    init(
        id: UUID,
        displayName: String,
        servers: [ServerEndpoint] = [],
        nicks: [String] = [],
        sasl: SASLConfig? = nil,
        autoConnect: Bool = false,
        autoJoin: [String] = [],
        onConnectCommands: [String] = []
    ) {
        self.id = id
        self.displayName = displayName
        self.servers = servers
        self.nicks = nicks
        self.sasl = sasl
        self.autoConnect = autoConnect
        self.autoJoin = autoJoin
        self.onConnectCommands = onConnectCommands
    }
}

extension Network: Transferable {
    var plainTextDescription: String { displayName }

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
        ProxyRepresentation(exporting: \.plainTextDescription)
    }
}

struct Connection: Identifiable, Hashable, Codable, Transferable {
    let id: UUID
    let networkID: UUID
    var serverName: String
    var selfNick: String?
    /// Snapshot of `server::have_chathistory`. Set on every event the
    /// connection is observed in; flips on CAP NEW/DEL or full reconnect. The
    /// Phase 7.5 `loadOlder` path gates the chathistory bridge dispatch on
    /// this flag.
    var haveChathistory: Bool

    init(
        id: UUID,
        networkID: UUID,
        serverName: String,
        selfNick: String? = nil,
        haveChathistory: Bool = false
    ) {
        self.id = id
        self.networkID = networkID
        self.serverName = serverName
        self.selfNick = selfNick
        self.haveChathistory = haveChathistory
    }

    private enum CodingKeys: String, CodingKey {
        case id, networkID, serverName, selfNick, haveChathistory
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        self.id = try c.decode(UUID.self, forKey: .id)
        self.networkID = try c.decode(UUID.self, forKey: .networkID)
        self.serverName = try c.decode(String.self, forKey: .serverName)
        self.selfNick = try c.decodeIfPresent(String.self, forKey: .selfNick)
        // Missing key decodes to false: an unknown/missing capability is the
        // safe-default value. This matches the property's = false initializer.
        self.haveChathistory = try c.decodeIfPresent(Bool.self, forKey: .haveChathistory) ?? false
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(id, forKey: .id)
        try c.encode(networkID, forKey: .networkID)
        try c.encode(serverName, forKey: .serverName)
        try c.encodeIfPresent(selfNick, forKey: .selfNick)
        try c.encode(haveChathistory, forKey: .haveChathistory)
    }

    var plainTextDescription: String { serverName }

    static var transferRepresentation: some TransferRepresentation {
        CodableRepresentation(contentType: .json)
        ProxyRepresentation(exporting: \.plainTextDescription)
    }
}

/// Result of `EngineController.loadOlder(forConversation:limit:)`. Phase 7.5
/// replaces the Phase 7 `Int` return so callers can distinguish "no local rows
/// AND no remote fetch in flight" (UI: stop paginating) from "no local rows
/// but a server request was dispatched" (UI: keep the affordance, replays will
/// arrive asynchronously).
struct LoadOlderResult: Equatable {
    let localCount: Int
    let requestedRemote: Bool
}

/// Outbound seam for `CHATHISTORY BEFORE` requests. The C-runtime
/// implementation lives in `CRuntimeChathistoryBridge`; tests inject a
/// recording fake. `requestBefore` is fire-and-forget — replays arrive over
/// the existing event channel, and `loadOlder` returns synchronously
/// regardless of bridge state.
protocol ChathistoryBridge: Sendable {
    func requestBefore(connectionID: UInt64, channel: String, beforeMsec: Int64, limit: Int)
}

/// Production bridge — calls `hc_apple_runtime_request_chathistory_before`
/// in `src/fe-apple/apple-runtime.c`. The C function is thread-safe (it
/// strdups the channel and dispatches via `g_main_context_invoke` onto the
/// engine thread), so calling from the @MainActor `loadOlder` path needs no
/// extra serialisation. Failures inside the engine-thread callback drop
/// silently — replays come over the existing event channel.
struct CRuntimeChathistoryBridge: ChathistoryBridge {
    func requestBefore(connectionID: UInt64, channel: String, beforeMsec: Int64, limit: Int) {
        _ = hc_apple_runtime_request_chathistory_before(
            connectionID, channel, beforeMsec, Int32(limit))
    }
}

struct User: Identifiable, Hashable {
    let id: UUID
    let connectionID: UUID
    var nick: String
    var account: String?
    var hostmask: String?
    var isMe: Bool
    var isAway: Bool

    static func == (lhs: User, rhs: User) -> Bool { lhs.id == rhs.id }
    func hash(into hasher: inout Hasher) { hasher.combine(id) }
}

struct ChannelMembership: Identifiable, Hashable {
    let sessionID: UUID
    let userID: UUID
    var modePrefix: Character?

    var id: String { "\(sessionID.uuidString)::\(userID.uuidString)" }

    static func == (lhs: ChannelMembership, rhs: ChannelMembership) -> Bool {
        lhs.sessionID == rhs.sessionID && lhs.userID == rhs.userID
    }
    func hash(into hasher: inout Hasher) {
        hasher.combine(sessionID)
        hasher.combine(userID)
    }
}

struct UserKey: Hashable {
    let connectionID: UUID
    let nickKey: String

    init(connectionID: UUID, nick: String) {
        self.connectionID = connectionID
        self.nickKey = nick.lowercased()
    }
}

/// Classifies free-text `LOG_LINE` strings (apple-runtime synthetic lines, IRC
/// PRIVMSG/NOTICE bodies) into a `ChatMessageKind`. JOIN/PART/QUIT/KICK/NICK/MODE
/// are no longer matched here as of Phase 5 — typed-event producers
/// (`fe_text_event` → `HC_APPLE_EVENT_*_CHANGE`) are the source of truth for
/// those events. Anything that doesn't match a recognized prefix becomes
/// `.message(body:)`.
enum ChatMessageClassifier {
    static func classify(raw: String) -> ChatMessageKind {
        let isLifecycle =
            raw.hasPrefix("[STARTING]") || raw.hasPrefix("[READY]")
            || raw.hasPrefix("[STOPPING]") || raw.hasPrefix("[STOPPED]")
        if isLifecycle {
            // Body is the raw line minus the leading `[PHASE] ` token.
            let phase = raw.prefix(while: { $0 != "]" }).dropFirst()  // "[STARTING" → "STARTING"
            let body = raw.drop(while: { $0 != "]" }).dropFirst().drop(while: { $0 == " " })
            return .lifecycle(phase: String(phase), body: String(body))
        }
        if raw.hasPrefix("!") { return .error(body: String(raw.dropFirst().drop(while: { $0 == " " }))) }
        if raw.hasPrefix(">") { return .command(body: String(raw.dropFirst().drop(while: { $0 == " " }))) }
        if raw.hasPrefix("-") { return .notice(body: String(raw.dropFirst().drop(while: { $0 == " " }))) }
        return .message(body: raw)
    }
}

enum SessionLocator: Hashable {
    case composed(connectionID: UUID, channel: String)
    case runtime(id: UInt64)

    var composedKey: String {
        switch self {
        case .composed(let connectionID, let channel):
            return "\(connectionID.uuidString.lowercased())::\(channel.lowercased())"
        case .runtime(let id):
            return "sess:\(id)"
        }
    }

    static func == (lhs: SessionLocator, rhs: SessionLocator) -> Bool {
        switch (lhs, rhs) {
        case (.composed(let a, let ac), .composed(let b, let bc)):
            return a == b && ac.caseInsensitiveCompare(bc) == .orderedSame
        case (.runtime(let a), .runtime(let b)):
            return a == b
        default:
            return false
        }
    }

    func hash(into hasher: inout Hasher) {
        switch self {
        case .composed(let connectionID, let channel):
            hasher.combine(0)
            hasher.combine(connectionID)
            hasher.combine(channel.lowercased())
        case .runtime(let id):
            hasher.combine(1)
            hasher.combine(id)
        }
    }
}

@MainActor
@Observable
final class EngineController {
    static let commandHistoryCap = 1000
    static let messagesGlobalCap = 5000
    static let messageRingPerConversation = 200

    var isRunning = false
    var messages: [ChatMessage] = []
    private var messageRing: [ConversationKey: [ChatMessage]] = [:]
    var sessions: [ChatSession] = []

    var conversations: [ConversationKey: ConversationState] = [:] {
        didSet { coordinator?.markDirty() }
    }

    func markRead(forSession sessionID: UUID) {
        markReadInternal(forSession: sessionID)
    }

    @discardableResult
    private func markReadInternal(forSession sessionID: UUID) -> Bool {
        guard let key = conversationKey(for: sessionID) else { return false }
        var state = conversations[key] ?? ConversationState(key: key)
        state.unread = 0
        state.lastReadAt = Date()
        conversations[key] = state
        return true
    }

    // MARK: - Focus tracking

    /// The most-recently focused session UUID across all windows. Cold-launch hint:
    /// `AppMain` seeds the primary `WindowSession.focusedSessionID` from this value
    /// when no `@SceneStorage` state exists. **Survives `LIFECYCLE_STOPPED`** — it is
    /// not runtime state.
    private(set) var lastFocusedSessionID: UUID?

    /// Number of `WindowSession`s currently focused on each session UUID. A session
    /// in this map (with non-zero count) suppresses unread incrementing in
    /// `recordActivity`. **Cleared on `LIFECYCLE_STOPPED`** because all sessions
    /// are torn down; live windows re-register on next focus change.
    private(set) var focusRefcount: [UUID: Int] = [:]

    /// Set during `apply(_:)` if the persisted snapshot named a `lastFocusedKey`
    /// whose session has not yet been re-emitted by the C core. Resolved inside
    /// `upsertSession` on first matching emit. **Survives `LIFECYCLE_STOPPED`** so
    /// reconnect-after-stop still honours the hint.
    private var pendingLastFocusedKey: ConversationKey?

    /// Single mutation entry point for focus state. `WindowSession.focusedSessionID
    /// didSet` calls this with `from: oldValue, to: newValue`. `WindowSession.deinit`
    /// calls this with `from: focusedSessionID, to: nil` (synchronously, via
    /// `MainActor.assumeIsolated`).
    func recordFocusTransition(from old: UUID?, to new: UUID?) {
        guard old != new else { return }
        if let old, let count = focusRefcount[old] {
            if count <= 1 {
                focusRefcount.removeValue(forKey: old)
            } else {
                focusRefcount[old] = count - 1
            }
        }
        if let new {
            focusRefcount[new, default: 0] += 1
            lastFocusedSessionID = new
            markReadInternal(forSession: new)
        }
    }

    // MARK: - Phase 10: per-window unread registry

    /// Weak pointer wrapper. The controller holds non-owning references so window
    /// lifetime is governed by SwiftUI scene teardown — the controller never
    /// keeps a window alive past its scene.
    private final class WeakWindowBox {
        weak var window: WindowSession?
        init(window: WindowSession) { self.window = window }
    }

    /// Registered `WindowSession`s, keyed by `ObjectIdentifier`. Mutated only on
    /// `@MainActor`. Used by `recordActivity(on:)` to broadcast unread bumps and
    /// by the REMOVE session-event branch to scrub stale UUID keys from each
    /// window's `unread` map.
    private var weakWindows: [ObjectIdentifier: WeakWindowBox] = [:]

    /// Register a `WindowSession` with the controller. Called from
    /// `WindowSession.init`. Idempotent on the same identity.
    func registerWindow(_ window: WindowSession) {
        weakWindows[ObjectIdentifier(window)] = WeakWindowBox(window: window)
    }

    /// Unregister a `WindowSession`. Called from `WindowSession.deinit` inside
    /// `MainActor.assumeIsolated` so registry mutation is serialised with
    /// `recordFocusTransition`.
    func unregisterWindow(_ window: WindowSession) {
        weakWindows.removeValue(forKey: ObjectIdentifier(window))
    }

    /// Iterate live registered windows. Lazily prunes any boxes whose weak ref
    /// has been dropped (defensive — `unregisterWindow` should already have
    /// fired).
    private func iterateRegisteredWindows(_ body: (WindowSession) -> Void) {
        for (key, box) in weakWindows {
            if let window = box.window {
                body(window)
            } else {
                // Safe to mutate during iteration: Dictionary is CoW, so removeValue
                // allocates a new buffer while the iterator continues traversing the
                // old one. Boxes already visited are not revisited.
                weakWindows.removeValue(forKey: key)
            }
        }
    }

    /// Test-only. Returns the count of live registered windows, pruning any
    /// boxes whose weak ref has been dropped as a side effect (because
    /// `iterateRegisteredWindows` does the prune).
    var registeredWindowCountForTest: Int {
        var count = 0
        iterateRegisteredWindows { _ in count += 1 }
        return count
    }

    var activeSessionID: UUID?

    private(set) var sessionByLocator: [SessionLocator: UUID] = [:]

    var networks: [UUID: Network] = [:]
    var connections: [UUID: Connection] = [:]
    private(set) var networksByName: [String: UUID] = [:]
    private(set) var connectionsByServerID: [UInt64: UUID] = [:]

    private(set) var users: [UUID: User] = [:]
    private(set) var usersByConnectionAndNick: [UserKey: UUID] = [:]
    private(set) var membershipsBySession: [UUID: [ChannelMembership]] = [:]

    var usersBySession: [UUID: [ChatUser]] {
        var out: [UUID: [ChatUser]] = [:]
        for (sessionID, memberships) in membershipsBySession {
            var roster: [ChatUser] = []
            roster.reserveCapacity(memberships.count)
            for m in memberships {
                guard let user = users[m.userID] else {
                    assertionFailure("membershipsBySession/users drift: userID \(m.userID) missing")
                    continue
                }
                roster.append(
                    ChatUser(
                        connectionID: user.connectionID,
                        nick: user.nick, modePrefix: m.modePrefix,
                        account: user.account, host: user.hostmask,
                        isMe: user.isMe, isAway: user.isAway))
            }
            roster.sort(by: userSort)
            out[sessionID] = roster
        }
        return out
    }

    @discardableResult
    private func upsertUser(
        connectionID: UUID, nick: String,
        account: String?, hostmask: String?,
        isMe: Bool, isAway: Bool
    ) -> UUID {
        let key = UserKey(connectionID: connectionID, nick: nick)
        if let existing = usersByConnectionAndNick[key] {
            // Direct assignment (no `??` merge): `account = nil` means the account was
            // explicitly dropped (e.g. services logout / account-notify clear). Phase 3
            // ChatUser behaves this way; see testUserlistUpdateClearsAccountToNil.
            // Refresh stored nick to match the incoming event's casing. True renames
            // (different UserKey) fall through to the create branch; Phase 5 owns them.
            users[existing]?.nick = nick
            users[existing]?.account = account
            users[existing]?.hostmask = hostmask
            users[existing]?.isMe = isMe
            users[existing]?.isAway = isAway
            return existing
        }
        let new = User(
            id: UUID(), connectionID: connectionID, nick: nick,
            account: account, hostmask: hostmask, isMe: isMe, isAway: isAway)
        users[new.id] = new
        usersByConnectionAndNick[key] = new.id
        return new.id
    }

    private func setMembership(sessionID: UUID, userID: UUID, modePrefix: Character?) {
        var roster = membershipsBySession[sessionID, default: []]
        if let idx = roster.firstIndex(where: { $0.userID == userID }) {
            roster[idx].modePrefix = modePrefix
        } else {
            roster.append(ChannelMembership(sessionID: sessionID, userID: userID, modePrefix: modePrefix))
        }
        membershipsBySession[sessionID] = roster
    }

    private func removeMembership(sessionID: UUID, userID: UUID) {
        membershipsBySession[sessionID]?.removeAll { $0.userID == userID }
    }

    /// Builds a `MessageAuthor` for `(connectionID, nick)`, resolving `userID`
    /// via the Phase 4 `usersByConnectionAndNick` index. Returns a `nil` userID
    /// when the nick is not yet tracked on this connection (e.g. a typed JOIN
    /// that arrives before its USERLIST_INSERT companion). Nick casing is passed
    /// through unchanged; `UserKey` lowercases internally.
    func resolveAuthor(connectionID: UUID, nick: String) -> MessageAuthor {
        let userID = usersByConnectionAndNick[UserKey(connectionID: connectionID, nick: nick)]
        return MessageAuthor(nick: nick, userID: userID)
    }

    // Test helpers, parallel to the other applyForTest/upsertForTest methods.
    func upsertUserForTest(
        connectionID: UUID, nick: String,
        account: String?, hostmask: String?,
        isMe: Bool, isAway: Bool
    ) -> UUID {
        upsertUser(
            connectionID: connectionID, nick: nick,
            account: account, hostmask: hostmask, isMe: isMe, isAway: isAway)
    }

    func setMembershipForTest(sessionID: UUID, userID: UUID, modePrefix: Character?) {
        setMembership(sessionID: sessionID, userID: userID, modePrefix: modePrefix)
    }

    func removeMembershipForTest(sessionID: UUID, userID: UUID) {
        removeMembership(sessionID: sessionID, userID: userID)
    }

    enum SystemSession {
        static let networkName = "network"
        static let channel = "server"
    }

    @ObservationIgnored
    private var systemSessionUUIDStorage: UUID?
    @ObservationIgnored
    private var systemConnectionUUIDStorage: UUID?

    private func systemConnectionUUID() -> UUID {
        if let cached = systemConnectionUUIDStorage { return cached }
        let networkID = upsertNetwork(name: SystemSession.networkName)
        let new = Connection(
            id: UUID(), networkID: networkID,
            serverName: SystemSession.networkName, selfNick: nil)
        connections[new.id] = new
        // Intentionally NOT in connectionsByServerID: server_id == 0 is reserved for "no real server."
        systemConnectionUUIDStorage = new.id
        return new.id
    }

    private func systemSessionUUID() -> UUID {
        let connectionID = systemConnectionUUID()
        let locator = SessionLocator.composed(connectionID: connectionID, channel: SystemSession.channel)
        if let existing = sessionByLocator[locator] {
            systemSessionUUIDStorage = existing
            return existing
        }
        if let cached = systemSessionUUIDStorage { return cached }
        let placeholder = ChatSession(
            connectionID: connectionID,
            channel: SystemSession.channel,
            isActive: false,
            locator: locator)
        sessions.append(placeholder)
        sessions = sessions.sorted(by: sessionSort)
        systemSessionUUIDStorage = placeholder.id
        sessionByLocator[locator] = placeholder.id
        return placeholder.id
    }

    func sessionUUID(for locator: SessionLocator) -> UUID? {
        sessionByLocator[locator]
    }

    var commandHistory: [String] = [] {
        didSet {
            if commandHistory.count > Self.commandHistoryCap {
                commandHistory.removeFirst(commandHistory.count - Self.commandHistoryCap)
                return  // recursive didSet from the trim hits markDirty itself
            }
            coordinator?.markDirty()
        }
    }
    private var historyCursorBySession: [UUID: Int] = [:]
    private var historyDraftBySession: [UUID: String] = [:]

    @ObservationIgnored
    private var coordinator: PersistenceCoordinator?

    @ObservationIgnored
    private let messageStore: MessageStore
    @ObservationIgnored
    private let chathistoryBridge: ChathistoryBridge
    @ObservationIgnored
    nonisolated private static let messageStoreLog = Logger(
        subsystem: "net.afternet.hexchat", category: "messageStore")

    /// Convenience for tests, previews, and any caller that doesn't want to
    /// write to the user's real `~/Library/Application Support`. Production
    /// must explicitly construct `EngineController(persistence:messageStore:)`
    /// with the on-disk stores.
    convenience init() {
        self.init(
            persistence: InMemoryPersistenceStore(),
            messageStore: InMemoryMessageStore(),
            debounceInterval: .milliseconds(500))
    }

    init(
        persistence: PersistenceStore,
        messageStore: MessageStore = InMemoryMessageStore(),
        debounceInterval: Duration = .milliseconds(500),
        chathistory: ChathistoryBridge = CRuntimeChathistoryBridge()
    ) {
        self.messageStore = messageStore
        self.chathistoryBridge = chathistory
        // Coordinator stays nil during apply() so didSet observers don't schedule
        // saves while we're rehydrating from disk. Wired up after apply() completes.
        self.coordinator = nil
        if let loaded = try? persistence.load() {
            apply(loaded)
        }
        self.coordinator = PersistenceCoordinator(
            store: persistence,
            debounceInterval: debounceInterval,
            snapshot: { [weak self] in self?.currentAppState() ?? AppState() }
        )
    }

    /// Resolves a runtime session UUID to the durable persistence key.
    /// Returns nil for the system session or when the network mapping is missing.
    func conversationKey(for sessionID: UUID) -> ConversationKey? {
        guard
            let session = sessions.first(where: { $0.id == sessionID }),
            let connection = connections[session.connectionID]
        else { return nil }
        return ConversationKey(networkID: connection.networkID, channel: session.channel)
    }

    private func currentAppState() -> AppState {
        AppState(
            networks: networks.values.sorted {
                let primary = $0.displayName.localizedStandardCompare($1.displayName)
                if primary != .orderedSame { return primary == .orderedAscending }
                return $0.id.uuidString < $1.id.uuidString
            },
            conversations: conversations.values.sorted {
                if $0.key.networkID != $1.key.networkID {
                    return $0.key.networkID.uuidString < $1.key.networkID.uuidString
                }
                return $0.key.channel.lowercased() < $1.key.channel.lowercased()
            },
            lastFocusedKey: lastFocusedSessionID.flatMap(conversationKey(for:)),
            commandHistory: commandHistory
        )
    }

    /// Apply state read from the persistence store. Caller is responsible for
    /// ensuring the coordinator is nil while this runs — every assignment below
    /// fires a didSet that would otherwise schedule a save of the just-loaded
    /// state. Today's only caller is init(persistence:debounceInterval:).
    private func apply(_ state: AppState) {
        networks = Dictionary(
            state.networks.map { ($0.id, $0) }, uniquingKeysWith: { _, last in last })
        networksByName = Dictionary(
            state.networks.map { ($0.displayName.lowercased(), $0.id) },
            uniquingKeysWith: { _, last in last })
        conversations = Dictionary(
            state.conversations.map { ($0.key, $0) },
            uniquingKeysWith: { _, last in last })
        commandHistory = state.commandHistory
        pendingLastFocusedKey = state.lastFocusedKey
    }

    func recordCommand(_ cmd: String) {
        commandHistory.append(cmd)
    }

    func applyForTest(_ state: AppState) { apply(state) }

    func snapshotForPersistence() -> AppState { currentAppState() }

    func upsertNetworkForTest(id: UUID, name: String) {
        upsertNetwork(id: id, name: name)
    }

    @discardableResult
    func upsertNetworkForName(_ name: String) -> UUID {
        if let existing = networksByName[name.lowercased()] {
            return existing
        }
        return upsertNetwork(id: UUID(), name: name)
    }

    @discardableResult
    private func upsertNetwork(id: UUID, name: String) -> UUID {
        if var existing = networks[id] {
            existing.displayName = name
            networks[id] = existing
        } else {
            networks[id] = Network(id: id, displayName: name)
        }
        networksByName[name.lowercased()] = id
        coordinator?.markDirty()
        return id
    }

    func setConversationStateForTest(_ state: ConversationState) {
        conversations[state.key] = state
    }

    private var callbackUserdata: UnsafeMutableRawPointer?

    func visibleMessages(for sessionID: UUID?) -> [ChatMessage] {
        guard let id = sessionID else { return [] }
        return messages.filter { $0.sessionID == id }
    }

    func visibleUsers(for sessionID: UUID?) -> [ChatUser] {
        guard let id = sessionID else { return [] }
        return usersBySession[id] ?? []
    }

    func visibleSessionTitle(for sessionID: UUID?) -> String {
        guard let id = sessionID,
              let session = sessions.first(where: { $0.id == id }),
              let name = networkDisplayName(for: session.connectionID)
        else {
            return "No Session"
        }
        return "\(name) • \(session.channel)"
    }

    func draftBinding(for sessionID: UUID?) -> Binding<String> {
        Binding(
            get: { [weak self] in
                guard let self, let id = sessionID,
                      let key = self.conversationKey(for: id)
                else { return "" }
                return self.conversations[key]?.draft ?? ""
            },
            set: { [weak self] newValue in
                guard let self, let id = sessionID,
                      let key = self.conversationKey(for: id)
                else { return }
                var state = self.conversations[key] ?? ConversationState(key: key)
                state.draft = newValue
                self.conversations[key] = state
            }
        )
    }

    var networkSections: [NetworkSection] {
        // Dropped orphans: sessions whose Connection has been cleared (LIFECYCLE_STOPPED window).
        // Inner sort is unneeded — `sessions` is kept sorted by `sessionSort` on every mutation.
        let grouped = Dictionary(grouping: sessions) { session -> UUID? in
            connections[session.connectionID]?.networkID
        }
        let ordered = grouped.keys
            .compactMap { $0 }
            .compactMap { networks[$0] }
            .sorted { $0.displayName.localizedStandardCompare($1.displayName) == .orderedAscending }
        return ordered.map { network in
            NetworkSection(
                id: network.id.uuidString,
                name: network.displayName,
                sessions: grouped[network.id] ?? [])
        }
    }

    private func networkDisplayName(for connectionID: UUID) -> String? {
        connections[connectionID].flatMap { networks[$0.networkID]?.displayName }
    }

    func start() {
        guard !isRunning else {
            return
        }
        let config = hc_apple_runtime_config(config_dir: nil, no_auto: 1, skip_plugins: 1)
        callbackUserdata = Unmanaged.passUnretained(self).toOpaque()
        withUnsafePointer(to: config) { configPtr in
            let started = hc_apple_runtime_start(configPtr, engineEventCallback, callbackUserdata)
            if started == 0 {
                appendMessage(raw: "! runtime start failed", kind: .error(body: "runtime start failed"))
            }
        }
    }

    func stop() {
        hc_apple_runtime_stop()
    }

    func send(_ command: String, forSession sessionID: UUID?, trackHistory: Bool = true) {
        let trimmed = command.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            return
        }

        if trackHistory {
            if commandHistory.last != trimmed {
                recordCommand(trimmed)
            }
            if let id = sessionID {
                historyCursorBySession[id] = commandHistory.count
                historyDraftBySession[id] = ""
            }
        }

        let targetSessionID: UInt64 = sessionID.map(numericRuntimeSessionID(forSelection:)) ?? 0
        appendMessage(raw: "> \(trimmed)", kind: .command(body: trimmed))
        trimmed.withCString { cString in
            let code: Int32
            if targetSessionID > 0 {
                code = hc_apple_runtime_post_command_for_session(cString, targetSessionID)
            } else {
                code = hc_apple_runtime_post_command(cString)
            }
            if code == 0 {
                appendMessage(raw: "! failed to post command", kind: .error(body: "failed to post command"))
            }
        }
    }

    func prefillPrivateMessage(to nick: String, forSession sessionID: UUID?) {
        draftBinding(for: sessionID).wrappedValue = "/msg \(nick) "
    }

    func browseHistory(delta: Int, forSession sessionID: UUID?) {
        guard let sessionID, !commandHistory.isEmpty else {
            return
        }

        let draft = draftBinding(for: sessionID)
        var cursor = historyCursorBySession[sessionID] ?? commandHistory.count

        if delta < 0 {
            if cursor == commandHistory.count {
                historyDraftBySession[sessionID] = draft.wrappedValue
            }
            cursor = max(0, cursor - 1)
            draft.wrappedValue = commandHistory[cursor]
            historyCursorBySession[sessionID] = cursor
            return
        }

        cursor = min(commandHistory.count, cursor + 1)
        if cursor == commandHistory.count {
            draft.wrappedValue = historyDraftBySession[sessionID] ?? ""
        } else {
            draft.wrappedValue = commandHistory[cursor]
        }
        historyCursorBySession[sessionID] = cursor
    }

    func applyUserlistForTest(
        action: hc_apple_userlist_action,
        network: String,
        channel: String,
        nick: String?,
        modePrefix: Character? = nil,
        account: String? = nil,
        host: String? = nil,
        isMe: Bool = false,
        isAway: Bool = false,
        sessionID: UInt64 = 0,
        connectionID: UInt64 = 0,
        selfNick: String? = nil
    ) {
        let event = RuntimeEvent(
            kind: HC_APPLE_EVENT_USERLIST,
            text: nil,
            phase: HC_APPLE_LIFECYCLE_STARTING,
            code: Int32(action.rawValue),
            sessionID: sessionID,
            network: network,
            channel: channel,
            nick: nick,
            modePrefix: modePrefix,
            account: account,
            host: host,
            isMe: isMe,
            isAway: isAway,
            connectionID: connectionID,
            selfNick: selfNick,
            membershipAction: HC_APPLE_MEMBERSHIP_JOIN,  // ignored when kind != MEMBERSHIP_CHANGE
            targetNick: nil,
            reason: nil,
            modes: nil,
            modesArgs: nil,
            timestampSeconds: 0,
            serverMsgID: nil,
            connectionHaveChathistory: false
        )
        handleUserlistEvent(event)
    }

    func applyUserlistRawForTest(
        action: hc_apple_userlist_action,
        network: String?,
        channel: String?,
        nick: String?,
        connectionID: UInt64 = 0,
        selfNick: String? = nil
    ) {
        let event = RuntimeEvent(
            kind: HC_APPLE_EVENT_USERLIST,
            text: nil,
            phase: HC_APPLE_LIFECYCLE_STARTING,
            code: Int32(action.rawValue),
            sessionID: 0,
            network: network,
            channel: channel,
            nick: nick,
            modePrefix: nil,
            account: nil,
            host: nil,
            isMe: false,
            isAway: false,
            connectionID: connectionID,
            selfNick: selfNick,
            membershipAction: HC_APPLE_MEMBERSHIP_JOIN,  // ignored when kind != MEMBERSHIP_CHANGE
            targetNick: nil,
            reason: nil,
            modes: nil,
            modesArgs: nil,
            timestampSeconds: 0,
            serverMsgID: nil,
            connectionHaveChathistory: false
        )
        handleUserlistEvent(event)
    }

    func systemSessionUUIDForTest() -> UUID {
        systemSessionUUID()
    }

    func applySessionForTest(
        action: hc_apple_session_action,
        network: String,
        channel: String,
        sessionID: UInt64 = 0,
        connectionID: UInt64 = 0,
        selfNick: String? = nil,
        connectionHaveChathistory: Bool = false
    ) {
        let event = RuntimeEvent(
            kind: HC_APPLE_EVENT_SESSION,
            text: nil,
            phase: HC_APPLE_LIFECYCLE_STARTING,
            code: Int32(action.rawValue),
            sessionID: sessionID,
            network: network,
            channel: channel,
            nick: nil,
            modePrefix: nil,
            account: nil,
            host: nil,
            isMe: false,
            isAway: false,
            connectionID: connectionID,
            selfNick: selfNick,
            membershipAction: HC_APPLE_MEMBERSHIP_JOIN,  // ignored when kind != MEMBERSHIP_CHANGE
            targetNick: nil,
            reason: nil,
            modes: nil,
            modesArgs: nil,
            timestampSeconds: 0,
            serverMsgID: nil,
            connectionHaveChathistory: connectionHaveChathistory
        )
        handleSessionEvent(event)
    }

    func applyRenameForTest(network: String, fromChannel: String, toChannel: String) {
        // Legacy helper — looks up the first registered connection for this network name,
        // falling back to the system connection. The old implementation relied on the
        // composed locator's network string; with connection-UUID keying, we resolve
        // via networksByName.
        let networkID = networksByName[network.lowercased()]
        let connectionID = connections.values
            .first(where: { $0.networkID == networkID })?.id
            ?? systemConnectionUUID()
        upsertSession(
            locator: .composed(connectionID: connectionID, channel: fromChannel),
            connectionID: connectionID,
            channel: toChannel
        )
    }

    func systemConnectionUUIDForTest() -> UUID { systemConnectionUUID() }

    func applyLifecycleForTest(phase: hc_apple_lifecycle_phase) {
        let event = RuntimeEvent(
            kind: HC_APPLE_EVENT_LIFECYCLE, text: nil, phase: phase,
            code: 0, sessionID: 0, network: nil, channel: nil, nick: nil,
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            connectionID: 0, selfNick: nil,
            membershipAction: HC_APPLE_MEMBERSHIP_JOIN, targetNick: nil,
            reason: nil, modes: nil, modesArgs: nil, timestampSeconds: 0,
            serverMsgID: nil, connectionHaveChathistory: false)
        handleRuntimeEvent(event)
    }

    func applyMembershipForTest(
        action: hc_apple_membership_action,
        network: String,
        channel: String,
        nick: String,
        targetNick: String? = nil,
        reason: String? = nil,
        sessionID: UInt64 = 0,
        connectionID: UInt64 = 0,
        selfNick: String? = nil,
        timestampSeconds: Int64 = 0,
        connectionHaveChathistory: Bool = false
    ) {
        let event = RuntimeEvent(
            kind: HC_APPLE_EVENT_MEMBERSHIP_CHANGE,
            text: nil,
            phase: HC_APPLE_LIFECYCLE_STARTING,
            code: 0,
            sessionID: sessionID,
            network: network,
            channel: channel,
            nick: nick,
            modePrefix: nil,
            account: nil,
            host: nil,
            isMe: false,
            isAway: false,
            connectionID: connectionID,
            selfNick: selfNick,
            membershipAction: action,
            targetNick: targetNick,
            reason: reason,
            modes: nil,
            modesArgs: nil,
            timestampSeconds: timestampSeconds,
            serverMsgID: nil,
            connectionHaveChathistory: connectionHaveChathistory
        )
        handleRuntimeEvent(event)
    }

    func applyNickChangeForTest(
        network: String, channel: String,
        oldNick: String, newNick: String,
        sessionID: UInt64 = 0, connectionID: UInt64 = 0, selfNick: String? = nil,
        timestampSeconds: Int64 = 0
    ) {
        let event = RuntimeEvent(
            kind: HC_APPLE_EVENT_NICK_CHANGE,
            text: nil, phase: HC_APPLE_LIFECYCLE_STARTING, code: 0,
            sessionID: sessionID, network: network, channel: channel,
            nick: oldNick, modePrefix: nil, account: nil, host: nil,
            isMe: false, isAway: false,
            connectionID: connectionID, selfNick: selfNick,
            membershipAction: HC_APPLE_MEMBERSHIP_JOIN,
            targetNick: newNick, reason: nil, modes: nil, modesArgs: nil,
            timestampSeconds: timestampSeconds,
            serverMsgID: nil, connectionHaveChathistory: false)
        handleRuntimeEvent(event)
    }

    func applyModeChangeForTest(
        network: String, channel: String, actor: String,
        modes: String, args: String?,
        sessionID: UInt64 = 0, connectionID: UInt64 = 0, selfNick: String? = nil,
        timestampSeconds: Int64 = 0
    ) {
        let event = RuntimeEvent(
            kind: HC_APPLE_EVENT_MODE_CHANGE,
            text: nil, phase: HC_APPLE_LIFECYCLE_STARTING, code: 0,
            sessionID: sessionID, network: network, channel: channel,
            nick: actor, modePrefix: nil, account: nil, host: nil,
            isMe: false, isAway: false,
            connectionID: connectionID, selfNick: selfNick,
            membershipAction: HC_APPLE_MEMBERSHIP_JOIN,
            targetNick: nil, reason: nil, modes: modes, modesArgs: args,
            timestampSeconds: timestampSeconds,
            serverMsgID: nil, connectionHaveChathistory: false)
        handleRuntimeEvent(event)
    }

    func applyLogLineForTest(
        network: String? = nil,
        channel: String? = nil,
        text: String,
        sessionID: UInt64 = 0,
        connectionID: UInt64 = 0,
        selfNick: String? = nil,
        serverMsgID: String? = nil,
        connectionHaveChathistory: Bool = false,
        timestampSeconds: Int64 = 0
    ) {
        let event = RuntimeEvent(
            kind: HC_APPLE_EVENT_LOG_LINE,
            text: text,
            phase: HC_APPLE_LIFECYCLE_STARTING,
            code: 0,
            sessionID: sessionID,
            network: network,
            channel: channel,
            nick: nil,
            modePrefix: nil,
            account: nil,
            host: nil,
            isMe: false,
            isAway: false,
            connectionID: connectionID,
            selfNick: selfNick,
            membershipAction: HC_APPLE_MEMBERSHIP_JOIN,  // ignored when kind != MEMBERSHIP_CHANGE
            targetNick: nil,
            reason: nil,
            modes: nil,
            modesArgs: nil,
            timestampSeconds: timestampSeconds,
            serverMsgID: serverMsgID,
            connectionHaveChathistory: connectionHaveChathistory
        )
        handleRuntimeEvent(event)
    }

    // Test helpers, parallel to the other applyForTest methods.
    func upsertNetworkForTest(name: String) -> UUID { upsertNetwork(name: name) }

    func upsertConnectionForTest(
        serverID: UInt64, networkID: UUID, serverName: String, selfNick: String?
    ) -> UUID {
        upsertConnection(
            serverID: serverID, networkID: networkID,
            serverName: serverName, selfNick: selfNick)
    }

    fileprivate func handleRuntimeEvent(_ event: RuntimeEvent) {
        switch event.kind {
        case HC_APPLE_EVENT_LOG_LINE:
            if let text = event.text {
                appendMessage(raw: text, kind: ChatMessageClassifier.classify(raw: text), event: event)
            }
        case HC_APPLE_EVENT_LIFECYCLE:
            if let text = event.text {
                appendMessage(raw: "[\(event.phase.name)] \(text)", kind: .lifecycle(phase: event.phase.name, body: text))
            }
            if event.phase == HC_APPLE_LIFECYCLE_READY {
                isRunning = true
            } else if event.phase == HC_APPLE_LIFECYCLE_STOPPED {
                // Persist the final live state before the runtime teardown nukes it.
                // Done before any clearing so the on-disk snapshot reflects what the
                // user actually had when the engine stopped.
                coordinator?.flushNow()
                isRunning = false
                // Clear runtime-only state. Persistable identity (`networks`,
                // `networksByName`, `conversations`, `commandHistory`,
                // `lastFocusedSessionID`, `pendingLastFocusedKey`) survives:
                // network UUIDs must remain stable across STOPPED so reconnect
                // (whether intra-process or cold-launch) produces the same
                // ConversationKey, which is what `pendingLastFocusedKey`
                // resolution relies on.
                sessionByLocator = [:]
                focusRefcount = [:]
                connections = [:]
                connectionsByServerID = [:]
                membershipsBySession = [:]
                users = [:]
                usersByConnectionAndNick = [:]
                sessions.removeAll()
                systemSessionUUIDStorage = nil
                systemConnectionUUIDStorage = nil
            }
        case HC_APPLE_EVENT_COMMAND:
            if event.code != 0 {
                let commandText = event.text ?? ""
                let errorBody = "command rejected (\(event.code)): \(commandText)"
                appendMessage(raw: "! \(errorBody)", kind: .error(body: errorBody))
            }
        case HC_APPLE_EVENT_USERLIST:
            handleUserlistEvent(event)
        case HC_APPLE_EVENT_SESSION:
            handleSessionEvent(event)
        case HC_APPLE_EVENT_MEMBERSHIP_CHANGE:
            handleMembershipChangeEvent(event)
        case HC_APPLE_EVENT_NICK_CHANGE:
            handleNickChangeEvent(event)
        case HC_APPLE_EVENT_MODE_CHANGE:
            handleModeChangeEvent(event)
        default:
            break
        }
    }

    private func appendMessage(raw: String, kind: ChatMessageKind, event: RuntimeEvent? = nil) {
        let targetUUID = resolveMessageSessionID(event: event)
        // Phase 7.5: only LOG_LINE events carry a meaningful serverMsgID — typed
        // events deliberately leave it nil because sess->current_msgid is not
        // safe to read on those code paths. Apply at this seam so callers don't
        // have to think about it.
        let serverMsgID: String? = {
            guard let event, event.kind == HC_APPLE_EVENT_LOG_LINE else { return nil }
            guard let raw = event.serverMsgID, !raw.isEmpty,
                !raw.hasPrefix("pending:")
            else { return nil }
            return raw
        }()
        // Honor producer-side timestamp for chathistory replays. Falls back to
        // Date() when the event is local or has no timestamp.
        let timestamp: Date = {
            if let event, event.timestampSeconds > 0 {
                return Date(timeIntervalSince1970: TimeInterval(event.timestampSeconds))
            }
            return Date()
        }()
        append(
            ChatMessage(
                sessionID: targetUUID, raw: raw, kind: kind, timestamp: timestamp,
                serverMsgID: serverMsgID),
            attributed: event != nil)
    }

    private func append(_ message: ChatMessage, attributed: Bool = true) {
        // Phase 7.5: storage decides whether this is a new row. The ring and
        // global messages array follow only when the store reports the insert
        // succeeded — that's the single dedup invariant for chathistory replays.
        // System-pseudo-session messages bypass storage (they're console output)
        // and always insert.
        let storeInserted = writeThroughToMessageStore(message)
        guard storeInserted else { return }

        messages.append(message)
        if messages.count > Self.messagesGlobalCap {
            messages.removeFirst(messages.count - Self.messagesGlobalCap)
        }
        if let key = conversationKey(for: message.sessionID) {
            var bucket = messageRing[key] ?? []
            // Insertion-sort by timestamp so out-of-order arrivals (chathistory
            // replays carrying server-time tags from the past) land at the right
            // slot. The fast path keeps live-message appends O(1).
            if let last = bucket.last, message.timestamp >= last.timestamp {
                bucket.append(message)
            } else {
                let insertAt = bucket.firstIndex { $0.timestamp > message.timestamp }
                    ?? bucket.endIndex
                bucket.insert(message, at: insertAt)
            }
            if bucket.count > Self.messageRingPerConversation {
                bucket.removeFirst(bucket.count - Self.messageRingPerConversation)
            }
            messageRing[key] = bucket
        }
        // Local console output (command echoes, internal errors, lifecycle banners)
        // arrives via appendMessage(event: nil) and must not bump unread on the
        // active conversation. Server-driven messages always carry an event.
        if attributed {
            recordActivity(on: message.sessionID)
        }
    }

    func messageRingForTest(conversation: ConversationKey) -> [ChatMessage] {
        messageRing[conversation] ?? []
    }

    /// Page older history for a conversation. Phase 7.5 makes this two-tier:
    /// SQLite first, then `CHATHISTORY BEFORE` over the C bridge if local rows
    /// fall short of `limit` AND the connection advertises `draft/chathistory`.
    /// Replays arrive asynchronously over the existing event channel; the
    /// caller doesn't wait. `LoadOlderResult.requestedRemote` tells the UI
    /// whether to keep the "load older" affordance enabled past `localCount == 0`.
    ///
    /// Synchronous on @MainActor: SQLite reads are fast at this scale and
    /// FULLMUTEX makes the connection safe alongside the (now sync) write
    /// path. The bridge dispatch is itself fire-and-forget.
    @discardableResult
    func loadOlder(forConversation key: ConversationKey, limit: Int) throws -> LoadOlderResult {
        guard limit > 0 else { return LoadOlderResult(localCount: 0, requestedRemote: false) }
        let preFetchOldest = messageRing[key]?.first?.timestamp
        let page = try messageStore.page(
            conversation: key, before: preFetchOldest, limit: limit)
        if !page.isEmpty {
            var bucket = messageRing[key] ?? []
            bucket.insert(contentsOf: page, at: 0)
            // Allow loadOlder to push the ring up to 2x the per-conversation cap so
            // the UI has room to scroll without an immediate trim discarding the
            // newly-prepended rows.
            let upperBound = Self.messageRingPerConversation * 2
            if bucket.count > upperBound {
                bucket.removeFirst(bucket.count - upperBound)
            }
            messageRing[key] = bucket
        }

        let localCount = page.count
        var requestedRemote = false
        if localCount < limit,
            let connection = liveConnection(forNetwork: key.networkID),
            connection.haveChathistory,
            let runtimeServerID = runtimeServerID(forConnection: connection.id)
        {
            // Anchor on the post-fetch ring oldest, not the pre-fetch one — codex
            // finding #1 from the plan review. Falls back to "now" only when the
            // ring is still completely empty after the local fetch.
            let anchor =
                page.first?.timestamp
                ?? preFetchOldest
                ?? Date()
            chathistoryBridge.requestBefore(
                connectionID: runtimeServerID,
                channel: key.channel,
                beforeMsec: Int64(anchor.timeIntervalSince1970 * 1000),
                limit: limit - localCount)
            requestedRemote = true
        }
        return LoadOlderResult(localCount: localCount, requestedRemote: requestedRemote)
    }

    /// Resolves a durable `networkID` to the most recent live `Connection`. If
    /// the network has no Connection in flight (offline), returns nil.
    private func liveConnection(forNetwork networkID: UUID) -> Connection? {
        connections.values.first(where: { $0.networkID == networkID })
    }

    /// Inverse of `connectionsByServerID`: durable connection UUID → runtime
    /// server id (UInt64). The bridge needs the runtime id to resolve back to
    /// the C-side `server*`.
    private func runtimeServerID(forConnection connectionID: UUID) -> UInt64? {
        connectionsByServerID.first(where: { $0.value == connectionID })?.key
    }

    /// Returns whether the in-memory ring should mutate. System-pseudo-session
    /// messages (no `ConversationKey`) bypass storage and return `true` so they
    /// always insert into the ring. Real conversation messages return whatever
    /// the store says; if storage throws (a broken / read-only file system) we
    /// log and return `false` so the ring stays consistent with what's durable.
    private func writeThroughToMessageStore(_ message: ChatMessage) -> Bool {
        guard message.sessionID != systemSessionUUIDStorage,
            let key = conversationKey(for: message.sessionID)
        else { return true }
        do {
            return try messageStore.append(message, conversation: key)
        } catch {
            Self.messageStoreLog.error(
                "messageStore.append failed: \(String(describing: error))")
            return false
        }
    }

    private func recordActivity(on sessionID: UUID) {
        // Skip the system pseudo-session — its messages are local console output,
        // not unread-bearing conversation activity.
        // Suppress unread when any window currently focuses this session.
        // markRead fires from WindowSession.focusedSessionID didSet, but only
        // on focus change — sequential messages in a focused session need the
        // refcount check to keep unread at 0.
        guard sessionID != systemSessionUUIDStorage,
            focusRefcount[sessionID, default: 0] == 0,
            let key = conversationKey(for: sessionID)
        else { return }
        var state = conversations[key] ?? ConversationState(key: key)
        state.unread += 1
        conversations[key] = state
    }

    func appendMessageForTest(_ message: ChatMessage) {
        append(message)
    }

    func appendUnattributedForTest(raw: String, kind: ChatMessageKind) {
        appendMessage(raw: raw, kind: kind, event: nil)
    }

    // Safety: `activeSessionID` always references a session currently in `sessions`.
    // Each engine event is dispatched as its own `Task { @MainActor }` block, so a
    // LOG_LINE cannot interleave with `HC_APPLE_SESSION_REMOVE` mid-handler. The REMOVE
    // path updates `activeSessionID` before `sessions.removeAll` runs, so by the next
    // event tick the invariant is re-established.
    private func resolveMessageSessionID(event: RuntimeEvent?) -> UUID {
        if let event, let resolved = resolveEventSessionID(event) { return resolved }
        if let act = activeSessionID { return act }
        if let first = sessions.first?.id { return first }
        return systemSessionUUID()
    }

    @discardableResult
    private func registerConnection(from event: RuntimeEvent) -> UUID? {
        guard event.connectionID != 0, let networkName = event.network else { return nil }
        let networkID = upsertNetwork(name: networkName)
        return upsertConnection(
            serverID: event.connectionID,
            networkID: networkID,
            serverName: networkName,
            selfNick: event.selfNick,
            haveChathistory: event.connectionHaveChathistory)
    }

    private func resolveEventSessionID(_ event: RuntimeEvent) -> UUID? {
        guard let connectionID = registerConnection(from: event) else { return nil }
        guard let channel = event.channel else { return nil }
        let locator: SessionLocator = event.sessionID > 0
            ? .runtime(id: event.sessionID)
            : .composed(connectionID: connectionID, channel: channel)
        return upsertSession(locator: locator, connectionID: connectionID, channel: channel)
    }

    private func handleSessionEvent(_ event: RuntimeEvent) {
        let channel = event.channel ?? SystemSession.channel
        let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
        let locator: SessionLocator = event.sessionID > 0
            ? .runtime(id: event.sessionID)
            : .composed(connectionID: connectionID, channel: channel)

        switch event.sessionAction {
        case HC_APPLE_SESSION_UPSERT:
            upsertSession(locator: locator, connectionID: connectionID, channel: channel)
        case HC_APPLE_SESSION_REMOVE:
            if let uuid = sessionByLocator[locator],
               let removed = sessions.first(where: { $0.id == uuid }) {
                membershipsBySession[uuid] = nil
                sessionByLocator[removed.locator] = nil
                focusRefcount.removeValue(forKey: uuid)
                historyCursorBySession.removeValue(forKey: uuid)
                historyDraftBySession.removeValue(forKey: uuid)
                if lastFocusedSessionID == uuid { lastFocusedSessionID = nil }
                // Evaluated against the still-intact `sessions` array, before the removeAll call below.
                if activeSessionID == uuid { activeSessionID = sessions.first(where: { $0.id != uuid })?.id }
                sessions.removeAll { $0.id == uuid }
            }
        case HC_APPLE_SESSION_ACTIVATE:
            let uuid = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
            activeSessionID = uuid
        default:
            break
        }

        // sessionSort ignores isActive, so no re-sort is needed after flipping flags.
        for idx in sessions.indices {
            sessions[idx].isActive = (sessions[idx].id == activeSessionID)
        }
    }

    @discardableResult
    private func upsertSession(
        locator: SessionLocator, connectionID: UUID, channel: String
    ) -> UUID {
        let targetLocator: SessionLocator
        switch locator {
        case .runtime:
            targetLocator = locator
        case .composed:
            targetLocator = .composed(connectionID: connectionID, channel: channel)
        }

        let resultID: UUID
        if let existing = sessionByLocator[locator],
           let idx = sessions.firstIndex(where: { $0.id == existing }) {
            let oldLocator = sessions[idx].locator
            let oldConnectionID = sessions[idx].connectionID
            let oldChannel = sessions[idx].channel
            sessions[idx].connectionID = connectionID
            sessions[idx].channel = channel
            sessions[idx].locator = targetLocator
            if oldLocator != targetLocator {
                sessionByLocator[oldLocator] = nil
            }
            sessionByLocator[targetLocator] = existing
            if oldConnectionID != connectionID || oldChannel != channel {
                sessions = sessions.sorted(by: sessionSort)
            }
            resultID = existing
        } else {
            let new = ChatSession(
                connectionID: connectionID,
                channel: channel,
                isActive: false,
                locator: targetLocator)
            sessions.append(new)
            sessionByLocator[targetLocator] = new.id
            sessions = sessions.sorted(by: sessionSort)
            resultID = new.id
        }
        resolvePendingLastFocusedIfMatches(uuid: resultID)
        return resultID
    }

    private func resolvePendingLastFocusedIfMatches(uuid: UUID) {
        guard let pending = pendingLastFocusedKey,
              let key = conversationKey(for: uuid),
              key == pending else { return }
        lastFocusedSessionID = uuid
        pendingLastFocusedKey = nil
    }

    @discardableResult
    private func upsertNetwork(name: String) -> UUID {
        upsertNetworkForName(name)
    }

    @discardableResult
    private func upsertConnection(
        serverID: UInt64, networkID: UUID, serverName: String, selfNick: String?,
        haveChathistory: Bool = false
    ) -> UUID {
        if let existing = connectionsByServerID[serverID] {
            if connections[existing]?.serverName != serverName {
                connections[existing]?.serverName = serverName
            }
            if let nick = selfNick, connections[existing]?.selfNick != nick {
                connections[existing]?.selfNick = nick
            }
            // Phase 7.5: cap bit can flip mid-session on CAP NEW/DEL or full
            // reconnect. Always rewrite from the freshest event so loadOlder's
            // gate decision uses current state.
            if connections[existing]?.haveChathistory != haveChathistory {
                connections[existing]?.haveChathistory = haveChathistory
            }
            return existing
        }
        let new = Connection(
            id: UUID(), networkID: networkID,
            serverName: serverName, selfNick: selfNick,
            haveChathistory: haveChathistory)
        connections[new.id] = new
        connectionsByServerID[serverID] = new.id
        return new.id
    }

    private func handleUserlistEvent(_ event: RuntimeEvent) {
        let channel = event.channel ?? SystemSession.channel
        let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
        let locator: SessionLocator = event.sessionID > 0
            ? .runtime(id: event.sessionID)
            : .composed(connectionID: connectionID, channel: channel)
        let sessionID = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
        let nick = event.nick ?? ""

        switch event.userlistAction {
        case HC_APPLE_USERLIST_INSERT, HC_APPLE_USERLIST_UPDATE:
            guard !nick.isEmpty else { return }
            let userID = upsertUser(
                connectionID: connectionID, nick: nick,
                account: event.account, hostmask: event.host,
                isMe: event.isMe, isAway: event.isAway)
            setMembership(sessionID: sessionID, userID: userID, modePrefix: event.modePrefix)
        case HC_APPLE_USERLIST_REMOVE:
            guard !nick.isEmpty,
                  let userID = usersByConnectionAndNick[UserKey(connectionID: connectionID, nick: nick)]
            else { return }
            removeMembership(sessionID: sessionID, userID: userID)
        case HC_APPLE_USERLIST_CLEAR:
            membershipsBySession[sessionID] = []
        default:
            break
        }
    }

    private func handleMembershipChangeEvent(_ event: RuntimeEvent) {
        let channel = event.channel ?? SystemSession.channel
        let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
        let locator: SessionLocator = event.sessionID > 0
            ? .runtime(id: event.sessionID)
            : .composed(connectionID: connectionID, channel: channel)
        let sessionID = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
        let nick = event.nick ?? ""
        guard !nick.isEmpty else { return }
        let author = resolveAuthor(connectionID: connectionID, nick: nick)
        let kind: ChatMessageKind
        switch event.membershipAction {
        case HC_APPLE_MEMBERSHIP_JOIN:
            kind = .join
        case HC_APPLE_MEMBERSHIP_PART:
            kind = .part(reason: event.reason)
        case HC_APPLE_MEMBERSHIP_QUIT:
            kind = .quit(reason: event.reason)
        case HC_APPLE_MEMBERSHIP_KICK:
            kind = .kick(target: event.targetNick ?? "", reason: event.reason)
        default:
            return
        }
        // Synthesize a back-compat raw string so legacy consumers reading `.raw` still
        // see something readable. Matches the format the classifier used to assign.
        let raw: String
        switch kind {
        case .join: raw = "* \(nick) has joined \(channel)"
        case .part(let r):
            raw = r.map { "* \(nick) has left \(channel) (\($0))" } ?? "* \(nick) has left \(channel)"
        case .quit(let r):
            raw = r.map { "* \(nick) has quit (\($0))" } ?? "* \(nick) has quit"
        case .kick(let target, let r):
            raw = r.map { "* \(nick) has kicked \(target) (\($0))" } ?? "* \(nick) has kicked \(target)"
        default: raw = ""
        }
        let timestamp = event.timestampSeconds == 0
            ? Date()
            : Date(timeIntervalSince1970: TimeInterval(event.timestampSeconds))
        append(ChatMessage(
            sessionID: sessionID, raw: raw, kind: kind,
            author: author, timestamp: timestamp))
    }

    private func handleNickChangeEvent(_ event: RuntimeEvent) {
        let channel = event.channel ?? SystemSession.channel
        let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
        let locator: SessionLocator = event.sessionID > 0
            ? .runtime(id: event.sessionID)
            : .composed(connectionID: connectionID, channel: channel)
        let sessionID = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
        let oldNick = event.nick ?? ""
        let newNick = event.targetNick ?? ""
        guard !oldNick.isEmpty, !newNick.isEmpty else { return }
        // Author is intentionally resolved from `oldNick` here. Phase 5 does NOT
        // remap `usersByConnectionAndNick[oldNick] → newNick`; the next
        // USERLIST_UPDATE for this user reconciles the index. A follow-up
        // (deferred per Phase 4 session summary) can correlate the typed NICK
        // event to update the User record in place instead of waiting.
        let author = resolveAuthor(connectionID: connectionID, nick: oldNick)
        let timestamp = event.timestampSeconds == 0
            ? Date()
            : Date(timeIntervalSince1970: TimeInterval(event.timestampSeconds))
        append(ChatMessage(
            sessionID: sessionID,
            raw: "* \(oldNick) is now known as \(newNick)",
            kind: .nickChange(from: oldNick, to: newNick),
            author: author,
            timestamp: timestamp))
    }

    private func handleModeChangeEvent(_ event: RuntimeEvent) {
        let channel = event.channel ?? SystemSession.channel
        let connectionID = registerConnection(from: event) ?? systemConnectionUUID()
        let locator: SessionLocator = event.sessionID > 0
            ? .runtime(id: event.sessionID)
            : .composed(connectionID: connectionID, channel: channel)
        let sessionID = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
        let actor = event.nick ?? ""
        let modes = event.modes ?? ""
        guard !modes.isEmpty else { return }
        let author = actor.isEmpty
            ? nil
            : resolveAuthor(connectionID: connectionID, nick: actor)
        let timestamp = event.timestampSeconds == 0
            ? Date()
            : Date(timeIntervalSince1970: TimeInterval(event.timestampSeconds))
        append(ChatMessage(
            sessionID: sessionID,
            raw: "* \(actor) sets mode \(modes)\(event.modesArgs.map { " " + $0 } ?? "")",
            kind: .modeChange(modes: modes, args: event.modesArgs),
            author: author,
            timestamp: timestamp))
    }

    private func sessionSort(_ lhs: ChatSession, _ rhs: ChatSession) -> Bool {
        let lhsName = networkDisplayName(for: lhs.connectionID) ?? ""
        let rhsName = networkDisplayName(for: rhs.connectionID) ?? ""
        if lhsName != rhsName {
            return lhsName.localizedStandardCompare(rhsName) == .orderedAscending
        }
        if lhs.isChannel != rhs.isChannel {
            return lhs.isChannel && !rhs.isChannel
        }
        return lhs.channel.localizedStandardCompare(rhs.channel) == .orderedAscending
    }

    private func userSort(_ lhs: ChatUser, _ rhs: ChatUser) -> Bool {
        let lhsRank = NickPrefix.rank(lhs.modePrefix)
        let rhsRank = NickPrefix.rank(rhs.modePrefix)
        if lhsRank != rhsRank {
            return lhsRank < rhsRank
        }
        return lhs.nick.localizedStandardCompare(rhs.nick) == .orderedAscending
    }

    internal func numericRuntimeSessionID(forSelection uuid: UUID) -> UInt64 {
        guard let session = sessions.first(where: { $0.id == uuid }),
              case .runtime(let n) = session.locator
        else { return 0 }
        return n
    }
}

enum NickPrefix {
    static func rank(_ prefix: Character?) -> Int {
        switch prefix {
        case "~": return 0
        case "&": return 1
        case "@": return 2
        case "%": return 3
        case "+": return 4
        default: return 99
        }
    }
}

fileprivate struct RuntimeEvent {
    let kind: hc_apple_event_kind
    let text: String?
    let phase: hc_apple_lifecycle_phase
    let code: Int32
    let sessionID: UInt64
    let network: String?
    let channel: String?
    let nick: String?
    let modePrefix: Character?
    let account: String?
    let host: String?
    let isMe: Bool
    let isAway: Bool
    let connectionID: UInt64
    let selfNick: String?
    // Phase 5 typed events
    let membershipAction: hc_apple_membership_action
    let targetNick: String?
    let reason: String?
    let modes: String?
    let modesArgs: String?
    /// Producer-side `time_t` widened to int64. 0 means "no producer timestamp;
    /// the consumer uses Date() at handle time."
    let timestampSeconds: Int64
    // Phase 7.5: chathistory bridge fields.
    /// IRCv3 msgid tag for log-line emits where `sess->current_msgid` was set.
    /// `nil` for typed events and untagged content.
    let serverMsgID: String?
    /// Snapshot of `sess->server->have_chathistory` at emit time.
    let connectionHaveChathistory: Bool

    var userlistAction: hc_apple_userlist_action {
        hc_apple_userlist_action(rawValue: UInt32(code))
    }

    var sessionAction: hc_apple_session_action {
        hc_apple_session_action(rawValue: UInt32(code))
    }
}

private extension hc_apple_lifecycle_phase {
    var name: String {
        switch self {
        case HC_APPLE_LIFECYCLE_STARTING:
            return "STARTING"
        case HC_APPLE_LIFECYCLE_READY:
            return "READY"
        case HC_APPLE_LIFECYCLE_STOPPING:
            return "STOPPING"
        case HC_APPLE_LIFECYCLE_STOPPED:
            return "STOPPED"
        default:
            return "UNKNOWN"
        }
    }
}

private func makeRuntimeEvent(from pointer: UnsafePointer<hc_apple_event>) -> RuntimeEvent {
    let raw = pointer.pointee

    let copiedText = raw.text.map { String(cString: $0) }
    let copiedNetwork = raw.network.map { String(cString: $0) }
    let copiedChannel = raw.channel.map { String(cString: $0) }
    let copiedNick = raw.nick.map { String(cString: $0) }
    let copiedAccount = raw.account.map { String(cString: $0) }
    let copiedHost = raw.host.map { String(cString: $0) }
    let copiedSelfNick = raw.self_nick.map { String(cString: $0) }
    let copiedTarget = raw.target_nick.map { String(cString: $0) }
    let copiedReason = raw.reason.map { String(cString: $0) }
    let copiedModes = raw.modes.map { String(cString: $0) }
    let copiedModesArgs = raw.modes_args.map { String(cString: $0) }
    let copiedServerMsgID = raw.server_msgid.map { String(cString: $0) }
    let prefix: Character? = raw.mode_prefix == 0 ? nil : Character(UnicodeScalar(raw.mode_prefix))

    return RuntimeEvent(
        kind: raw.kind,
        text: copiedText,
        phase: raw.lifecycle_phase,
        code: Int32(raw.code),
        sessionID: raw.session_id,
        network: copiedNetwork,
        channel: copiedChannel,
        nick: copiedNick,
        modePrefix: prefix,
        account: copiedAccount,
        host: copiedHost,
        isMe: raw.is_me != 0,
        isAway: raw.is_away != 0,
        connectionID: raw.connection_id,
        selfNick: copiedSelfNick,
        membershipAction: raw.membership_action,
        targetNick: copiedTarget,
        reason: copiedReason,
        modes: copiedModes,
        modesArgs: copiedModesArgs,
        timestampSeconds: raw.timestamp_seconds,
        serverMsgID: copiedServerMsgID,
        connectionHaveChathistory: raw.connection_have_chathistory != 0
    )
}

private let engineEventCallback: hc_apple_event_cb = { eventPtr, userData in
    guard let eventPtr, let userData else {
        return
    }

    let event = makeRuntimeEvent(from: eventPtr)
    let controller = Unmanaged<EngineController>.fromOpaque(userData).takeUnretainedValue()
    Task { @MainActor in
        controller.handleRuntimeEvent(event)
    }
}
