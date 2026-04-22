import Foundation
import Observation
import AppleAdapterBridge

struct ChatSession: Identifiable, Hashable {
    let id: UUID
    var network: String
    var channel: String
    var isActive: Bool
    var locator: SessionLocator

    init(
        id: UUID = UUID(),
        network: String,
        channel: String,
        isActive: Bool,
        locator: SessionLocator? = nil
    ) {
        self.id = id
        self.network = network
        self.channel = channel
        self.isActive = isActive
        self.locator = locator ?? .composed(network: network, channel: channel)
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

enum ChatMessageKind: String {
    case message
    case notice
    case join
    case part
    case quit
    case command
    case error
    case lifecycle
}

struct ChatMessage: Identifiable {
    let id = UUID()
    let sessionID: UUID
    let raw: String
    let kind: ChatMessageKind
}

/// A user in a single channel's roster.
///
/// `id` is the lowercased `nick` and is **only valid within one channel's roster**:
/// the same nick across multiple channels yields equal-`id` `ChatUser` records that
/// nonetheless represent independent rows in `usersBySession`. Phase 4 introduces
/// per-`Connection` `User` identity backed by a stable UUID and a `ChannelMembership`
/// junction; until then, do not treat `ChatUser.id` as a cross-channel identifier.
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

enum ChatMessageClassifier {
    static func classify(raw: String, fallback: ChatMessageKind = .message) -> ChatMessageKind {
        if raw.hasPrefix("[STARTING]") || raw.hasPrefix("[READY]") || raw.hasPrefix("[STOPPING]") || raw.hasPrefix("[STOPPED]") {
            return .lifecycle
        }
        if raw.hasPrefix("!") {
            return .error
        }
        if raw.hasPrefix(">") {
            return .command
        }

        let lower = raw.lowercased()
        if lower.contains(" has joined") || lower.contains(" joined ") {
            return .join
        }
        if lower.contains(" has left") || lower.contains(" left ") {
            return .part
        }
        if lower.contains(" quit") {
            return .quit
        }
        if raw.hasPrefix("-") {
            return .notice
        }

        return fallback
    }
}

enum SessionLocator: Hashable {
    case composed(network: String, channel: String)
    case runtime(id: UInt64)

    var composedKey: String {
        switch self {
        case .composed(let network, let channel):
            return "\(network.lowercased())::\(channel.lowercased())"
        case .runtime(let id):
            return "sess:\(id)"
        }
    }

    static func == (lhs: SessionLocator, rhs: SessionLocator) -> Bool {
        switch (lhs, rhs) {
        case (.composed(let an, let ac), .composed(let bn, let bc)):
            return an.caseInsensitiveCompare(bn) == .orderedSame
                && ac.caseInsensitiveCompare(bc) == .orderedSame
        case (.runtime(let a), .runtime(let b)):
            return a == b
        default:
            return false
        }
    }

    func hash(into hasher: inout Hasher) {
        switch self {
        case .composed(let n, let c):
            hasher.combine(0)
            hasher.combine(n.lowercased())
            hasher.combine(c.lowercased())
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
    var usersBySession: [UUID: [ChatUser]] = [:]
    var input = ""

    var selectedSessionID: UUID?
    var activeSessionID: UUID?

    private(set) var sessionByLocator: [SessionLocator: UUID] = [:]

    var networks: [UUID: Network] = [:]
    var connections: [UUID: Connection] = [:]
    private(set) var networksByName: [String: UUID] = [:]
    private(set) var connectionsByServerID: [UInt64: UUID] = [:]

    @ObservationIgnored
    private var systemSessionUUIDStorage: UUID?

    private enum SystemSession {
        static let network = "network"
        static let channel = "server"
        static let locator: SessionLocator = .composed(network: network, channel: channel)
    }

    private func systemSessionUUID() -> UUID {
        if let existing = sessionByLocator[SystemSession.locator] {
            systemSessionUUIDStorage = existing
            return existing
        }
        if let cached = systemSessionUUIDStorage { return cached }
        let placeholder = ChatSession(
            network: SystemSession.network,
            channel: SystemSession.channel,
            isActive: false,
            locator: SystemSession.locator
        )
        sessions.append(placeholder)
        sessions = sessions.sorted(by: sessionSort)
        systemSessionUUIDStorage = placeholder.id
        sessionByLocator[SystemSession.locator] = placeholder.id
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
            return SystemSession.locator.composedKey
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
        if let uuid = visibleSessionUUID, let session = sessions.first(where: { $0.id == uuid }) {
            return "\(session.network) • \(session.channel)"
        }
        return "No Session"
    }

    var networkSections: [NetworkSection] {
        let grouped = Dictionary(grouping: sessions) { $0.network }
        return grouped.keys.sorted(using: KeyPathComparator(\.self, comparator: .localizedStandard)).map { network in
            let rows = grouped[network, default: []].sorted(by: sessionSort)
            return NetworkSection(id: network.lowercased(), name: network, sessions: rows)
        }
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
                appendMessage(raw: "! runtime start failed", kind: .error)
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
        appendMessage(raw: "> \(trimmed)", kind: .command)
        trimmed.withCString { cString in
            let code: Int32
            if targetSessionID > 0 {
                code = hc_apple_runtime_post_command_for_session(cString, targetSessionID)
            } else {
                code = hc_apple_runtime_post_command(cString)
            }
            if code == 0 {
                appendMessage(raw: "! failed to post command", kind: .error)
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
        sessionID: UInt64 = 0
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
            isAway: isAway
        )
        handleUserlistEvent(event)
    }

    func applyUserlistRawForTest(
        action: hc_apple_userlist_action,
        network: String?,
        channel: String?,
        nick: String?
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
            isAway: false
        )
        handleUserlistEvent(event)
    }

    func systemSessionUUIDForTest() -> UUID {
        systemSessionUUID()
    }

    func applySessionForTest(action: hc_apple_session_action, network: String, channel: String, sessionID: UInt64 = 0) {
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
            isAway: false
        )
        handleSessionEvent(event)
    }

    func applyRenameForTest(network: String, fromChannel: String, toChannel: String) {
        upsertSession(
            locator: .composed(network: network, channel: fromChannel),
            network: network,
            channel: toChannel
        )
    }

    func applyLifecycleForTest(phase: hc_apple_lifecycle_phase) {
        let event = RuntimeEvent(
            kind: HC_APPLE_EVENT_LIFECYCLE, text: nil, phase: phase,
            code: 0, sessionID: 0, network: nil, channel: nil, nick: nil,
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false)
        handleRuntimeEvent(event)
    }

    func applyLogLineForTest(network: String, channel: String, text: String, sessionID: UInt64 = 0) {
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
            isAway: false
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
                appendMessage(raw: "[\(event.phase.name)] \(text)", kind: .lifecycle)
            }
            if event.phase == HC_APPLE_LIFECYCLE_READY {
                isRunning = true
            } else if event.phase == HC_APPLE_LIFECYCLE_STOPPED {
                isRunning = false
                usersBySession = [:]
                sessionByLocator = [:]
                if let old = systemSessionUUIDStorage {
                    sessions.removeAll { $0.id == old }
                }
                systemSessionUUIDStorage = nil
            }
        case HC_APPLE_EVENT_COMMAND:
            if event.code != 0 {
                let commandText = event.text ?? ""
                appendMessage(raw: "! command rejected (\(event.code)): \(commandText)", kind: .error)
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

    private func resolveEventSessionID(_ event: RuntimeEvent) -> UUID? {
        guard let network = event.network, let channel = event.channel else { return nil }
        let locator: SessionLocator = event.sessionID > 0
            ? .runtime(id: event.sessionID)
            : .composed(network: network, channel: channel)
        return upsertSession(locator: locator, network: network, channel: channel)
    }

    private func handleSessionEvent(_ event: RuntimeEvent) {
        let network = event.network ?? SystemSession.network
        let channel = event.channel ?? SystemSession.channel
        let locator: SessionLocator = event.sessionID > 0
            ? .runtime(id: event.sessionID)
            : .composed(network: network, channel: channel)

        switch event.sessionAction {
        case HC_APPLE_SESSION_UPSERT:
            upsertSession(locator: locator, network: network, channel: channel)
        case HC_APPLE_SESSION_REMOVE:
            if let uuid = sessionByLocator[locator],
               let removed = sessions.first(where: { $0.id == uuid }) {
                usersBySession[uuid] = nil
                sessionByLocator[removed.locator] = nil
                if selectedSessionID == uuid { selectedSessionID = nil }
                // Evaluated against the still-intact `sessions` array, before the removeAll call below.
                if activeSessionID == uuid { activeSessionID = sessions.first(where: { $0.id != uuid })?.id }
                sessions.removeAll { $0.id == uuid }
            }
        case HC_APPLE_SESSION_ACTIVATE:
            let uuid = upsertSession(locator: locator, network: network, channel: channel)
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
    private func upsertSession(locator: SessionLocator, network: String, channel: String) -> UUID {
        let targetLocator: SessionLocator
        switch locator {
        case .runtime:
            targetLocator = locator
        case .composed:
            targetLocator = .composed(network: network, channel: channel)
        }

        if let existing = sessionByLocator[locator],
           let idx = sessions.firstIndex(where: { $0.id == existing }) {
            let oldLocator = sessions[idx].locator
            sessions[idx].network = network
            sessions[idx].channel = channel
            sessions[idx].locator = targetLocator
            if oldLocator != targetLocator {
                sessionByLocator[oldLocator] = nil
            }
            sessionByLocator[targetLocator] = existing
            sessions = sessions.sorted(by: sessionSort)
            return existing
        }
        let new = ChatSession(
            network: network,
            channel: channel,
            isActive: false,
            locator: targetLocator
        )
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
            connections[existing]?.serverName = serverName
            if let nick = selfNick { connections[existing]?.selfNick = nick }
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
        let network = event.network ?? SystemSession.network
        let channel = event.channel ?? SystemSession.channel
        let locator: SessionLocator = event.sessionID > 0
            ? .runtime(id: event.sessionID)
            : .composed(network: network, channel: channel)
        let uuid = upsertSession(locator: locator, network: network, channel: channel)
        let nick = event.nick ?? ""

        switch event.userlistAction {
        case HC_APPLE_USERLIST_INSERT, HC_APPLE_USERLIST_UPDATE:
            guard !nick.isEmpty else { return }
            upsertChatUser(from: event, nick: nick, inSession: uuid)
        case HC_APPLE_USERLIST_REMOVE:
            guard !nick.isEmpty else { return }
            let key = nick.lowercased()
            usersBySession[uuid, default: []].removeAll { $0.id == key }
        case HC_APPLE_USERLIST_CLEAR:
            usersBySession[uuid] = []
        default:
            break
        }

        usersBySession[uuid, default: []].sort(by: userSort)
    }

    private func upsertChatUser(from event: RuntimeEvent, nick: String, inSession uuid: UUID) {
        let candidate = ChatUser(
            nick: nick,
            modePrefix: event.modePrefix,
            account: event.account,
            host: event.host,
            isMe: event.isMe,
            isAway: event.isAway
        )
        var roster = usersBySession[uuid, default: []]
        if let idx = roster.firstIndex(where: { $0.id == candidate.id }) {
            roster[idx] = candidate
        } else {
            roster.append(candidate)
        }
        usersBySession[uuid] = roster
    }

    private func sessionSort(_ lhs: ChatSession, _ rhs: ChatSession) -> Bool {
        if lhs.network != rhs.network {
            return lhs.network.localizedStandardCompare(rhs.network) == .orderedAscending
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
        isAway: raw.is_away != 0
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
