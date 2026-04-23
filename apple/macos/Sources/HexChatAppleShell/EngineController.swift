import Foundation
import Observation
import AppleAdapterBridge

struct ChatSession: Identifiable, Hashable {
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
struct MessageAuthor: Hashable {
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
enum ChatMessageKind: Hashable {
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
}

struct ChatMessage: Identifiable {
    let id = UUID()
    let sessionID: UUID
    let raw: String
    let kind: ChatMessageKind
    let author: MessageAuthor?
    let timestamp: Date

    init(
        sessionID: UUID,
        raw: String,
        kind: ChatMessageKind,
        author: MessageAuthor? = nil,
        timestamp: Date = Date()
    ) {
        self.sessionID = sessionID
        self.raw = raw
        self.kind = kind
        self.author = author
        self.timestamp = timestamp
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

/// View-facing DTO assembled on demand from `User` + `ChannelMembership` via the
/// computed `usersBySession` projection. `id` is the lowercased `nick` and is valid
/// only within one channel's roster (for SwiftUI `ForEach` diffing). Stable cross-
/// channel identity lives on `User.id` (UUID), looked up through
/// `usersByConnectionAndNick`.
struct ChatUser: Identifiable, Hashable {
    var nick: String
    var modePrefix: Character?
    var account: String?
    var host: String?
    var isMe: Bool
    var isAway: Bool

    init(
        nick: String,
        modePrefix: Character? = nil,
        account: String? = nil,
        host: String? = nil,
        isMe: Bool = false,
        isAway: Bool = false
    ) {
        self.nick = nick
        self.modePrefix = modePrefix
        self.account = account
        self.host = host
        self.isMe = isMe
        self.isAway = isAway
    }

    var id: String { nick.lowercased() }
}

struct Network: Identifiable, Hashable {
    let id: UUID
    var displayName: String
}

struct Connection: Identifiable, Hashable {
    let id: UUID
    let networkID: UUID
    var serverName: String
    var selfNick: String?
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

enum ChatMessageClassifier {
    static func classify(raw: String, fallback: ChatMessageKind = .message(body: "")) -> ChatMessageKind {
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
        let lower = raw.lowercased()
        if lower.contains(" has joined") || lower.contains(" joined ") { return .join }
        if lower.contains(" has left") || lower.contains(" left ") { return .part(reason: nil) }
        if lower.contains(" quit") { return .quit(reason: nil) }
        if raw.hasPrefix("-") { return .notice(body: String(raw.dropFirst().drop(while: { $0 == " " }))) }
        // Default body for the message case is the raw text.
        switch fallback {
        case .message:
            return .message(body: raw)
        default:
            return fallback
        }
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

@Observable
final class EngineController {
    var isRunning = false
    var messages: [ChatMessage] = []
    var sessions: [ChatSession] = []
    var input = ""

    var selectedSessionID: UUID?
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

    private(set) var commandHistory: [String] = []
    private var historyCursor = 0
    private var historyDraft = ""

    private var callbackUserdata: UnsafeMutableRawPointer?

    var visibleSessionUUID: UUID? {
        if let selectedSessionID { return selectedSessionID }
        if let activeSessionID { return activeSessionID }
        return sessions.first?.id
    }

    var visibleSessionID: String {
        guard let uuid = visibleSessionUUID,
              let session = sessions.first(where: { $0.id == uuid })
        else {
            let connID = systemConnectionUUID()
            return SessionLocator.composed(connectionID: connID, channel: SystemSession.channel).composedKey
        }
        return session.composedKey
    }

    var visibleMessages: [ChatMessage] {
        guard let uuid = visibleSessionUUID else { return [] }
        return messages.filter { $0.sessionID == uuid }
    }

    var visibleUsers: [ChatUser] {
        guard let uuid = visibleSessionUUID else { return [] }
        return usersBySession[uuid] ?? []
    }

    var visibleSessionTitle: String {
        guard let uuid = visibleSessionUUID,
              let session = sessions.first(where: { $0.id == uuid }),
              let name = networkDisplayName(for: session.connectionID)
        else {
            return "No Session"
        }
        return "\(name) • \(session.channel)"
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

    func send(_ command: String, trackHistory: Bool = true) {
        let trimmed = command.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            return
        }

        if trackHistory {
            if commandHistory.last != trimmed {
                commandHistory.append(trimmed)
            }
            historyCursor = commandHistory.count
            historyDraft = ""
        }

        let targetSessionID: UInt64 = {
            guard let uuid = selectedSessionID else { return 0 }
            return numericRuntimeSessionID(forSelection: uuid)
        }()
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

    func prefillPrivateMessage(to nick: String) {
        input = "/msg \(nick) "
    }

    func browseHistory(delta: Int) {
        guard !commandHistory.isEmpty else {
            return
        }

        if delta < 0 {
            if historyCursor == commandHistory.count {
                historyDraft = input
            }
            historyCursor = max(0, historyCursor - 1)
            input = commandHistory[historyCursor]
            return
        }

        historyCursor = min(commandHistory.count, historyCursor + 1)
        if historyCursor == commandHistory.count {
            input = historyDraft
        } else {
            input = commandHistory[historyCursor]
        }
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
            selfNick: selfNick
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
            selfNick: selfNick
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
        selfNick: String? = nil
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
            selfNick: selfNick
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
            connectionID: 0, selfNick: nil)
        handleRuntimeEvent(event)
    }

    func applyLogLineForTest(
        network: String? = nil,
        channel: String? = nil,
        text: String,
        sessionID: UInt64 = 0,
        connectionID: UInt64 = 0,
        selfNick: String? = nil
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
            selfNick: selfNick
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
                isRunning = false
                sessionByLocator = [:]
                networks = [:]
                connections = [:]
                networksByName = [:]
                connectionsByServerID = [:]
                membershipsBySession = [:]
                users = [:]
                usersByConnectionAndNick = [:]
                if let old = systemSessionUUIDStorage {
                    sessions.removeAll { $0.id == old }
                }
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
        default:
            break
        }
    }

    private func appendMessage(raw: String, kind: ChatMessageKind, event: RuntimeEvent? = nil) {
        let targetUUID = resolveMessageSessionID(event: event)
        messages.append(ChatMessage(sessionID: targetUUID, raw: raw, kind: kind))
    }

    func appendUnattributedForTest(raw: String, kind: ChatMessageKind) {
        appendMessage(raw: raw, kind: kind, event: nil)
    }

    // Safety: `activeSessionID`/`selectedSessionID` always reference a session currently
    // in `sessions`. Each engine event is dispatched as its own `Task { @MainActor }` block,
    // so a LOG_LINE cannot interleave with `HC_APPLE_SESSION_REMOVE` mid-handler. The REMOVE
    // path updates these UUIDs before `sessions.removeAll` runs, so by the next event tick
    // both invariants are re-established.
    private func resolveMessageSessionID(event: RuntimeEvent?) -> UUID {
        if let event, let resolved = resolveEventSessionID(event) { return resolved }
        if let act = activeSessionID { return act }
        if let sel = selectedSessionID { return sel }
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
            selfNick: event.selfNick)
    }

    private func resolveEventSessionID(_ event: RuntimeEvent) -> UUID? {
        guard let connectionID = registerConnection(from: event),
              let channel = event.channel else { return nil }
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
                if selectedSessionID == uuid { selectedSessionID = nil }
                // Evaluated against the still-intact `sessions` array, before the removeAll call below.
                if activeSessionID == uuid { activeSessionID = sessions.first(where: { $0.id != uuid })?.id }
                sessions.removeAll { $0.id == uuid }
            }
        case HC_APPLE_SESSION_ACTIVATE:
            let uuid = upsertSession(locator: locator, connectionID: connectionID, channel: channel)
            activeSessionID = uuid
            if selectedSessionID == nil { selectedSessionID = uuid }
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
            return existing
        }
        let new = ChatSession(
            connectionID: connectionID,
            channel: channel,
            isActive: false,
            locator: targetLocator)
        sessions.append(new)
        sessionByLocator[targetLocator] = new.id
        sessions = sessions.sorted(by: sessionSort)
        if selectedSessionID == nil { selectedSessionID = new.id }
        return new.id
    }

    @discardableResult
    private func upsertNetwork(name: String) -> UUID {
        let key = name.lowercased()
        if let existing = networksByName[key] { return existing }
        let new = Network(id: UUID(), displayName: name)
        networks[new.id] = new
        networksByName[key] = new.id
        return new.id
    }

    @discardableResult
    private func upsertConnection(
        serverID: UInt64, networkID: UUID, serverName: String, selfNick: String?
    ) -> UUID {
        if let existing = connectionsByServerID[serverID] {
            if connections[existing]?.serverName != serverName {
                connections[existing]?.serverName = serverName
            }
            if let nick = selfNick, connections[existing]?.selfNick != nick {
                connections[existing]?.selfNick = nick
            }
            return existing
        }
        let new = Connection(
            id: UUID(), networkID: networkID,
            serverName: serverName, selfNick: selfNick)
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
        selfNick: copiedSelfNick
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
