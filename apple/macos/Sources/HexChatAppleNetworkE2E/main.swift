import Foundation
import AppleAdapterBridge

private let targetHost = "irc.afternet.org"
private let targetChannels = ["#cybercafe", "#cannabis"]

private func normalized(_ channel: String) -> String {
    channel.lowercased()
}

private final class NetworkE2EState {
    private let lock = NSLock()
    private let rootToken: String
    private let channelTokens: [String: String]

    private(set) var sawReady = false
    private(set) var sawStopped = false
    private(set) var serverSessionID: UInt64 = 0

    private(set) var sawJoinCommands: Set<String> = []
    private(set) var sawSayCommands: Set<String> = []

    private(set) var channelSessionIDs: [String: UInt64] = [:]
    private(set) var channelUsers: [String: Set<String>] = [:]
    private(set) var channelInboundCount: [String: Int] = [:]

    private(set) var commandEvents: [String] = []
    private(set) var logLines: [String] = []
    private(set) var sessionEvents: [String] = []

    let readySignal = DispatchSemaphore(value: 0)
    let serverSessionSignal = DispatchSemaphore(value: 0)
    let channelSessionSignal = DispatchSemaphore(value: 0)
    let channelUserlistSignal = DispatchSemaphore(value: 0)
    let channelInboundSignal = DispatchSemaphore(value: 0)
    let stoppedSignal = DispatchSemaphore(value: 0)

    init(rootToken: String) {
        self.rootToken = rootToken
        var tokens: [String: String] = [:]
        for channel in targetChannels {
            let key = normalized(channel)
            let suffix = key.replacingOccurrences(of: "#", with: "")
            tokens[key] = "\(rootToken)-\(suffix)"
        }
        self.channelTokens = tokens
    }

    func token(for channel: String) -> String {
        lock.lock()
        defer { lock.unlock() }
        return channelTokens[normalized(channel)] ?? rootToken
    }

    func recordLifecycle(_ phase: hc_apple_lifecycle_phase) {
        lock.lock()
        defer { lock.unlock() }

        if phase == HC_APPLE_LIFECYCLE_READY {
            sawReady = true
            readySignal.signal()
        } else if phase == HC_APPLE_LIFECYCLE_STOPPED {
            sawStopped = true
            stoppedSignal.signal()
        }
    }

    func recordCommand(_ command: String, code: Int32) {
        lock.lock()
        defer { lock.unlock() }

        commandEvents.append("\(code):\(command)")
        guard code == 0 else {
            return
        }

        for channel in targetChannels {
            let key = normalized(channel)
            if command.localizedCaseInsensitiveContains("/join \(channel)") {
                sawJoinCommands.insert(key)
            }
            if let token = channelTokens[key], command.contains(token) {
                sawSayCommands.insert(key)
            }
        }
    }

    func recordSession(channel: String?, sessionID: UInt64, actionCode: Int32) {
        guard actionCode == Int32(HC_APPLE_SESSION_UPSERT.rawValue) || actionCode == Int32(HC_APPLE_SESSION_ACTIVATE.rawValue) else {
            return
        }
        guard let channel else {
            return
        }

        lock.lock()
        defer { lock.unlock() }

        sessionEvents.append("\(actionCode):\(channel):\(sessionID)")
        if sessionEvents.count > 80 {
            sessionEvents.removeFirst(sessionEvents.count - 80)
        }

        let key = normalized(channel)
        if targetChannels.map(normalized).contains(key), sessionID != 0 {
            channelSessionIDs[key] = sessionID
            channelSessionSignal.signal()
        }

        if sessionID != 0 && serverSessionID == 0 {
            serverSessionID = sessionID
            serverSessionSignal.signal()
        }
    }

    private func targetChannelKey(channel: String?, sessionID: UInt64) -> String? {
        if let channel {
            let key = normalized(channel)
            if targetChannels.map(normalized).contains(key) {
                return key
            }
        }
        if sessionID != 0 {
            for (key, sid) in channelSessionIDs where sid == sessionID {
                return key
            }
        }
        return nil
    }

    func recordLogLine(_ text: String, channel: String?, sessionID: UInt64) {
        lock.lock()
        defer { lock.unlock() }

        logLines.append(text)
        if logLines.count > 120 {
            logLines.removeFirst(logLines.count - 120)
        }

        guard let key = targetChannelKey(channel: channel, sessionID: sessionID) else {
            return
        }

        channelInboundCount[key, default: 0] += 1
        channelInboundSignal.signal()
    }

    func recordUserlist(actionCode: Int32, nick: String?, channel: String?, sessionID: UInt64) {
        lock.lock()
        defer { lock.unlock() }

        guard let key = targetChannelKey(channel: channel, sessionID: sessionID) else {
            return
        }

        let action = hc_apple_userlist_action(rawValue: UInt32(actionCode))
        let normalizedNick = nick?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()

        switch action {
        case HC_APPLE_USERLIST_INSERT, HC_APPLE_USERLIST_UPDATE:
            if let normalizedNick, !normalizedNick.isEmpty {
                channelUsers[key, default: []].insert(normalizedNick)
                channelUserlistSignal.signal()
            }
        case HC_APPLE_USERLIST_REMOVE:
            if let normalizedNick, !normalizedNick.isEmpty {
                channelUsers[key, default: []].remove(normalizedNick)
            }
        case HC_APPLE_USERLIST_CLEAR:
            channelUsers[key] = []
        default:
            break
        }
    }

    func sessionID(for channel: String) -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        return channelSessionIDs[normalized(channel)] ?? 0
    }

    func inboundCount(for channel: String) -> Int {
        lock.lock()
        defer { lock.unlock() }
        return channelInboundCount[normalized(channel)] ?? 0
    }

    func userCount(for channel: String) -> Int {
        lock.lock()
        defer { lock.unlock() }
        return channelUsers[normalized(channel), default: []].count
    }

    func allTargetChannelsHaveJoinCommand() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        let target = Set(targetChannels.map(normalized))
        return target.isSubset(of: sawJoinCommands)
    }

    func allTargetChannelsHaveSayCommand() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        let target = Set(targetChannels.map(normalized))
        return target.isSubset(of: sawSayCommands)
    }

    func summary() -> String {
        lock.lock()
        defer { lock.unlock() }

        let commands = commandEvents.joined(separator: "\n")
        let sessions = sessionEvents.joined(separator: "\n")
        let lastLogs = logLines.suffix(20).joined(separator: "\n")

        var perChannel: [String] = []
        for channel in targetChannels {
            let key = normalized(channel)
            perChannel.append("\(channel): session=\(channelSessionIDs[key, default: 0]) users=\(channelUsers[key, default: []].count) inbound=\(channelInboundCount[key, default: 0])")
        }

        return """
        sawReady=\(sawReady) sawStopped=\(sawStopped) serverSessionID=\(serverSessionID)
        sawJoinCommands=\(sawJoinCommands) sawSayCommands=\(sawSayCommands)
        channels:\n\(perChannel.joined(separator: "\n"))

        commands:
        \(commands)

        session-events:
        \(sessions)

        recent-log-lines:
        \(lastLogs)
        """
    }
}

private enum ExitCode: Int32 {
    case startFailed = 2
    case readyTimeout = 3
    case joinTimeout = 4
    case tokenTimeout = 5
    case stoppedTimeout = 6
    case directionCheckFailed = 7
}

private func waitForChannelSession(_ channel: String, state: NetworkE2EState, timeoutSeconds: Int) -> UInt64 {
    let deadline = Date().addingTimeInterval(TimeInterval(timeoutSeconds))
    while Date() < deadline {
        let sid = state.sessionID(for: channel)
        if sid != 0 {
            return sid
        }
        _ = state.channelSessionSignal.wait(timeout: .now() + 1)
    }
    return state.sessionID(for: channel)
}

private func waitForChannelUsers(_ channel: String, state: NetworkE2EState, timeoutSeconds: Int) -> Bool {
    let deadline = Date().addingTimeInterval(TimeInterval(timeoutSeconds))
    while Date() < deadline {
        if state.userCount(for: channel) > 0 {
            return true
        }
        _ = state.channelUserlistSignal.wait(timeout: .now() + 1)
    }
    return state.userCount(for: channel) > 0
}

private let callback: hc_apple_event_cb = { eventPtr, userData in
    guard let eventPtr, let userData else {
        return
    }

    let state = Unmanaged<NetworkE2EState>.fromOpaque(userData).takeUnretainedValue()
    let event = eventPtr.pointee

    switch event.kind {
    case HC_APPLE_EVENT_LIFECYCLE:
        state.recordLifecycle(event.lifecycle_phase)

    case HC_APPLE_EVENT_COMMAND:
        let text = event.text.map(String.init(cString:)) ?? ""
        state.recordCommand(text, code: event.code)

    case HC_APPLE_EVENT_SESSION:
        let channel = event.channel.map(String.init(cString:))
        state.recordSession(channel: channel, sessionID: event.session_id, actionCode: event.code)

    case HC_APPLE_EVENT_LOG_LINE:
        guard let cText = event.text else {
            return
        }
        let text = String(cString: cText)
        let channel = event.channel.map(String.init(cString:))
        state.recordLogLine(text, channel: channel, sessionID: event.session_id)

    case HC_APPLE_EVENT_USERLIST:
        let channel = event.channel.map(String.init(cString:))
        let nick = event.nick.map(String.init(cString:))
        state.recordUserlist(actionCode: event.code, nick: nick, channel: channel, sessionID: event.session_id)

    default:
        return
    }
}

private func run() -> Int32 {
    let rootToken = "hc-e2e-\(UUID().uuidString.prefix(8))"
    let nick = "hcE2E\(Int(Date().timeIntervalSince1970) % 100000)"

    let state = NetworkE2EState(rootToken: rootToken)
    let userdata = Unmanaged.passRetained(state).toOpaque()
    defer {
        Unmanaged<NetworkE2EState>.fromOpaque(userdata).release()
    }

    let config = hc_apple_runtime_config(config_dir: nil, no_auto: 0, skip_plugins: 1)
    let started = withUnsafePointer(to: config) { configPtr in
        hc_apple_runtime_start(configPtr, callback, userdata)
    }

    guard started != 0 else {
        fputs("failed to start runtime\n", stderr)
        return ExitCode.startFailed.rawValue
    }

    defer {
        hc_apple_runtime_stop()
        if state.stoppedSignal.wait(timeout: .now() + 20) != .success {
            fputs("timeout waiting for STOPPED lifecycle event\n", stderr)
            fputs(state.summary() + "\n", stderr)
            exit(ExitCode.stoppedTimeout.rawValue)
        }
    }

    guard state.readySignal.wait(timeout: .now() + 20) == .success else {
        fputs("timeout waiting for READY lifecycle event\n", stderr)
        return ExitCode.readyTimeout.rawValue
    }

    _ = "/nick \(nick)".withCString { hc_apple_runtime_post_command($0) }
    _ = "/server -insecure \(targetHost) 6667".withCString { hc_apple_runtime_post_command($0) }

    if state.serverSessionID == 0 {
        _ = state.serverSessionSignal.wait(timeout: .now() + 15)
    }

    var channelSessionIDs: [String: UInt64] = [:]
    for channel in targetChannels {
        for _ in 0..<8 {
            let joinCommand = "/join \(channel)"
            let serverSessionID = state.serverSessionID
            if serverSessionID != 0 {
                _ = joinCommand.withCString { hc_apple_runtime_post_command_for_session($0, serverSessionID) }
            } else {
                _ = joinCommand.withCString { hc_apple_runtime_post_command($0) }
            }

            let sid = waitForChannelSession(channel, state: state, timeoutSeconds: 5)
            if sid != 0 {
                channelSessionIDs[channel] = sid
                break
            }
        }

        if channelSessionIDs[channel] == nil {
            fputs("timeout waiting to join \(channel) on \(targetHost)\n", stderr)
            fputs(state.summary() + "\n", stderr)
            return ExitCode.joinTimeout.rawValue
        }
    }

    for channel in targetChannels {
        guard let sid = channelSessionIDs[channel] else { continue }
        let sayCommand = "/say \(state.token(for: channel))"
        _ = sayCommand.withCString {
            hc_apple_runtime_post_command_for_session($0, sid)
        }
    }

    for channel in targetChannels {
        guard let sid = channelSessionIDs[channel] else { continue }
        let namesCommand = "/names \(channel)"
        _ = namesCommand.withCString {
            hc_apple_runtime_post_command_for_session($0, sid)
        }

        let sawUsers = waitForChannelUsers(channel, state: state, timeoutSeconds: 30)
        if !sawUsers {
            fputs("timeout waiting for channel userlist population after /names on \(channel)\n", stderr)
            fputs(state.summary() + "\n", stderr)
            return ExitCode.tokenTimeout.rawValue
        }
    }

    guard state.allTargetChannelsHaveJoinCommand() && state.allTargetChannelsHaveSayCommand() else {
        fputs("missing command-path signal checks\n", stderr)
        fputs(state.summary() + "\n", stderr)
        return ExitCode.directionCheckFailed.rawValue
    }

    let counts = targetChannels.map { "\($0)=\(state.userCount(for: $0))" }.joined(separator: " ")
    print("PASS network e2e host=\(targetHost) channels=\(targetChannels.joined(separator: ",")) users=\(counts)")
    return 0
}

exit(run())
