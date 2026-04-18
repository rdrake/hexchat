import Foundation
import Observation
import AppleAdapterBridge

struct ChatSession: Identifiable, Hashable {
    let id: String
    var network: String
    var channel: String
    var isActive: Bool

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
    let sessionID: String
    let raw: String
    let kind: ChatMessageKind
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

@Observable
final class EngineController {
    private static let nickModePrefixes: Set<Character> = ["~", "&", "!", "@", "%", "+"]
    private let maxStoredMessages = 5_000
    private let messageTrimTarget = 4_500

    var isRunning = false
    var messages: [ChatMessage] = []
    var sessions: [ChatSession] = []
    var usersBySession: [String: [String]] = [:]
    var input = ""

    var selectedSessionID: String?
    var activeSessionID: String?

    private(set) var commandHistory: [String] = []
    private var historyCursor = 0
    private var historyDraft = ""

    private var callbackUserdata: UnsafeMutableRawPointer?

    static func sessionID(network: String, channel: String) -> String {
        "\(network.lowercased())::\(channel.lowercased())"
    }

    static func runtimeSessionID(_ sessionID: UInt64) -> String {
        "sess:\(sessionID)"
    }

    var visibleSessionID: String {
        if let selectedSessionID {
            return selectedSessionID
        }
        if let activeSessionID {
            return activeSessionID
        }
        let fallback = Self.sessionID(network: "network", channel: "server")
        if sessions.contains(where: { $0.id == fallback }) {
            return fallback
        }
        return sessions.first?.id ?? fallback
    }

    var visibleMessages: [ChatMessage] {
        messages.filter { $0.sessionID == visibleSessionID }
    }

    var visibleUsers: [String] {
        usersBySession[visibleSessionID] ?? []
    }

    var visibleSessionTitle: String {
        if let session = sessions.first(where: { $0.id == visibleSessionID }) {
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

        let targetSessionID = selectedRuntimeSessionNumericID()
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
        let target = stripModePrefix(nick)
        input = "/msg \(target) "
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

    func applyUserlistForTest(action: hc_apple_userlist_action, network: String, channel: String, nick: String?, sessionID: UInt64 = 0) {
        let event = RuntimeEvent(
            kind: HC_APPLE_EVENT_USERLIST,
            text: nil,
            phase: HC_APPLE_LIFECYCLE_STARTING,
            code: Int32(action.rawValue),
            sessionID: sessionID,
            network: network,
            channel: channel,
            nick: nick
        )
        handleUserlistEvent(event)
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
            nick: nil
        )
        handleSessionEvent(event)
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
            nick: nil
        )
        handleRuntimeEvent(event)
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
        let targetSessionID = resolveMessageSessionID(event: event)
        messages.append(ChatMessage(sessionID: targetSessionID, raw: raw, kind: kind))
        if messages.count > maxStoredMessages {
            messages.removeFirst(messages.count - messageTrimTarget)
        }
    }

    private func resolveMessageSessionID(event: RuntimeEvent?) -> String {
        if let event {
            if let resolved = resolveEventSessionID(event) {
                return resolved
            }
        }
        return selectedSessionID ?? activeSessionID ?? visibleSessionID
    }

    private func resolveEventSessionID(_ event: RuntimeEvent) -> String? {
        guard let network = event.network, let channel = event.channel else {
            return nil
        }
        let id = event.sessionID > 0
            ? Self.runtimeSessionID(event.sessionID)
            : Self.sessionID(network: network, channel: channel)
        upsertSession(id: id, network: network, channel: channel)
        return id
    }

    private func handleSessionEvent(_ event: RuntimeEvent) {
        let network = event.network ?? "network"
        let channel = event.channel ?? "server"
        let id = event.sessionID > 0
            ? Self.runtimeSessionID(event.sessionID)
            : Self.sessionID(network: network, channel: channel)

        switch event.sessionAction {
        case HC_APPLE_SESSION_UPSERT:
            upsertSession(id: id, network: network, channel: channel)
        case HC_APPLE_SESSION_REMOVE:
            sessions.removeAll { $0.id == id }
            usersBySession[id] = nil
            if selectedSessionID == id {
                selectedSessionID = nil
            }
            if activeSessionID == id {
                activeSessionID = sessions.first?.id
            }
        case HC_APPLE_SESSION_ACTIVATE:
            upsertSession(id: id, network: network, channel: channel)
            activeSessionID = id
            if selectedSessionID == nil {
                selectedSessionID = id
            }
        default:
            break
        }

        sessions = sessions.map { session in
            var mutable = session
            mutable.isActive = (session.id == activeSessionID)
            return mutable
        }.sorted(by: sessionSort)
    }

    private func upsertSession(id: String, network: String, channel: String) {
        if let index = sessions.firstIndex(where: { $0.id == id }) {
            sessions[index].network = network
            sessions[index].channel = channel
            sessions = sessions.sorted(by: sessionSort)
            return
        }

        sessions.append(ChatSession(id: id, network: network, channel: channel, isActive: false))
        sessions = sessions.sorted(by: sessionSort)
        if selectedSessionID == nil {
            selectedSessionID = id
        }
    }

    private func handleUserlistEvent(_ event: RuntimeEvent) {
        let network = event.network ?? "network"
        let channel = event.channel ?? "server"
        let sessionID = event.sessionID > 0
            ? Self.runtimeSessionID(event.sessionID)
            : Self.sessionID(network: network, channel: channel)
        upsertSession(id: sessionID, network: network, channel: channel)
        let nick = event.nick ?? ""

        switch event.userlistAction {
        case HC_APPLE_USERLIST_INSERT, HC_APPLE_USERLIST_UPDATE:
            guard !nick.isEmpty else { return }
            upsertNick(nick, in: sessionID)
        case HC_APPLE_USERLIST_REMOVE:
            guard !nick.isEmpty else { return }
            usersBySession[sessionID, default: []].removeAll {
                stripModePrefix($0).caseInsensitiveCompare(stripModePrefix(nick)) == .orderedSame
            }
        case HC_APPLE_USERLIST_CLEAR:
            usersBySession[sessionID] = []
        default:
            break
        }
    }

    private func upsertNick(_ nick: String, in sessionID: String) {
        let normalized = stripModePrefix(nick)
        var nicks = usersBySession[sessionID, default: []]
        if let idx = nicks.firstIndex(where: { stripModePrefix($0).caseInsensitiveCompare(normalized) == .orderedSame }) {
            nicks.remove(at: idx)
        }
        if let insertIndex = nicks.firstIndex(where: { userSort(nick, $0) }) {
            nicks.insert(nick, at: insertIndex)
        } else {
            nicks.append(nick)
        }
        usersBySession[sessionID] = nicks
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

    private func userSort(_ lhs: String, _ rhs: String) -> Bool {
        let lhsRank = modeRank(lhs)
        let rhsRank = modeRank(rhs)
        if lhsRank != rhsRank {
            return lhsRank < rhsRank
        }
        return stripModePrefix(lhs).localizedStandardCompare(stripModePrefix(rhs)) == .orderedAscending
    }

    private func modeRank(_ nick: String) -> Int {
        guard let prefix = nick.first else {
            return 99
        }
        switch prefix {
        case "~": return 0
        case "&": return 1
        case "!": return 2
        case "@": return 3
        case "%": return 4
        case "+": return 5
        default: return 99
        }
    }

    private func stripModePrefix(_ nick: String) -> String {
        guard let first = nick.first else {
            return nick
        }
        if Self.nickModePrefixes.contains(first) {
            return String(nick.dropFirst())
        }
        return nick
    }

    private func selectedRuntimeSessionNumericID() -> UInt64 {
        guard let selectedSessionID, selectedSessionID.hasPrefix("sess:") else {
            return 0
        }
        let raw = selectedSessionID.dropFirst("sess:".count)
        return UInt64(raw) ?? 0
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

private let runtimeEventQueue = DispatchQueue(label: "HexChatAppleShell.RuntimeEventQueue")
private var pendingRuntimeEvents: [RuntimeEvent] = []
private var runtimeEventDrainScheduled = false

private func makeRuntimeEvent(from pointer: UnsafePointer<hc_apple_event>) -> RuntimeEvent {
    let raw = pointer.pointee

    let copiedText = raw.text.map { String(cString: $0) }
    let copiedNetwork = raw.network.map { String(cString: $0) }
    let copiedChannel = raw.channel.map { String(cString: $0) }
    let copiedNick = raw.nick.map { String(cString: $0) }

    return RuntimeEvent(
        kind: raw.kind,
        text: copiedText,
        phase: raw.lifecycle_phase,
        code: Int32(raw.code),
        sessionID: raw.session_id,
        network: copiedNetwork,
        channel: copiedChannel,
        nick: copiedNick
    )
}

private let engineEventCallback: hc_apple_event_cb = { eventPtr, userData in
    guard let eventPtr, let userData else {
        return
    }

    let event = makeRuntimeEvent(from: eventPtr)
    let controller = Unmanaged<EngineController>.fromOpaque(userData).takeUnretainedValue()
    let shouldScheduleDrain = runtimeEventQueue.sync { () -> Bool in
        pendingRuntimeEvents.append(event)
        if runtimeEventDrainScheduled {
            return false
        }
        runtimeEventDrainScheduled = true
        return true
    }
    guard shouldScheduleDrain else { return }

    Task { @MainActor in
        while true {
            let batch = runtimeEventQueue.sync { () -> [RuntimeEvent] in
                let batch = pendingRuntimeEvents
                pendingRuntimeEvents.removeAll(keepingCapacity: true)
                if batch.isEmpty {
                    runtimeEventDrainScheduled = false
                }
                return batch
            }
            if batch.isEmpty { break }

            for queuedEvent in batch {
                controller.handleRuntimeEvent(queuedEvent)
            }
        }
    }
}
