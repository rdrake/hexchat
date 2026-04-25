#if canImport(XCTest)
import XCTest
@testable import HexChatAppleShell
import AppleAdapterBridge

final class EngineControllerTests: XCTestCase {
    func testUserlistInsertUpdateRemoveAndClear() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#hexchat",
            connectionID: 1, selfNick: "me")

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#hexchat",
            nick: "bob", connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#hexchat",
            nick: "alice", modePrefix: "@", connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#hexchat",
            nick: "bob", modePrefix: "+", connectionID: 1)

        XCTAssertEqual(controller.visibleUsers.map(\.nick), ["alice", "bob"])
        XCTAssertEqual(controller.visibleUsers.map(\.modePrefix), ["@", "+"])

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_REMOVE, network: "Libera", channel: "#hexchat",
            nick: "bob", connectionID: 1)
        XCTAssertEqual(controller.visibleUsers.map(\.nick), ["alice"])

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_CLEAR, network: "Libera", channel: "#hexchat",
            nick: nil, connectionID: 1)
        XCTAssertTrue(controller.visibleUsers.isEmpty)
    }

    func testChannelScopedUserlistsDoNotBleed() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b",
            connectionID: 1, selfNick: "me")

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#b",
            nick: "bob", connectionID: 1)

        let conn = controller.connectionsByServerID[1]!
        controller.selectedSessionID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        XCTAssertEqual(controller.visibleUsers.map(\.nick), ["alice"])

        controller.selectedSessionID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#b"))
        XCTAssertEqual(controller.visibleUsers.map(\.nick), ["bob"])
    }

    func testHistoryBrowseUpDownRestoresDraft() {
        let controller = EngineController()
        controller.send("/join #hexchat")
        controller.send("/msg alice hi")
        controller.input = "/nick newname"

        controller.browseHistory(delta: -1)
        XCTAssertEqual(controller.input, "/msg alice hi")

        controller.browseHistory(delta: -1)
        XCTAssertEqual(controller.input, "/join #hexchat")

        controller.browseHistory(delta: 1)
        XCTAssertEqual(controller.input, "/msg alice hi")

        controller.browseHistory(delta: 1)
        XCTAssertEqual(controller.input, "/nick newname")
    }

    func testMessageClassifier() {
        XCTAssertEqual(ChatMessageClassifier.classify(raw: "! failed"), .error(body: "failed"))
        XCTAssertEqual(ChatMessageClassifier.classify(raw: "> /join #a"), .command(body: "/join #a"))
        XCTAssertEqual(ChatMessageClassifier.classify(raw: "[READY] ready"), .lifecycle(phase: "READY", body: "ready"))
        XCTAssertEqual(ChatMessageClassifier.classify(raw: "-notice-"), .notice(body: "notice-"))
    }

    func testClassifierNoLongerMatchesJoinPartQuitText() {
        // The text path used to classify these as typed events. Phase 5 retires
        // the heuristic — typed-event producers are the source of truth now.
        if case .message = ChatMessageClassifier.classify(raw: "* alice has joined #a") {} else {
            XCTFail("\" has joined\" must classify as .message in Phase 5+, not .join")
        }
        if case .message = ChatMessageClassifier.classify(raw: "* alice has left #a") {} else {
            XCTFail("\" has left\" must classify as .message in Phase 5+, not .part")
        }
        if case .message = ChatMessageClassifier.classify(raw: "* alice has quit (bye)") {} else {
            XCTFail("\" quit\" must classify as .message in Phase 5+, not .quit")
        }
    }

    func testLogAttributionUsesEventSessionNotSelectedSession() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b",
            connectionID: 1, selfNick: "me")

        let conn = controller.connectionsByServerID[1]!
        controller.selectedSessionID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))

        controller.applyLogLineForTest(
            network: "Libera", channel: "#b", text: "message for b",
            connectionID: 1, selfNick: "me")

        let sessionA = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        let sessionB = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#b"))!
        XCTAssertFalse(
            controller.messages.contains(where: { $0.sessionID == sessionA && $0.raw == "message for b" }))
        XCTAssertTrue(
            controller.messages.contains(where: { $0.sessionID == sessionB && $0.raw == "message for b" }))
    }

    func testRuntimeSessionIDSeparatesSameNetworkChannelLabel() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#same",
            sessionID: 101, connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#same",
            sessionID: 202, connectionID: 1, selfNick: "me")

        controller.applyLogLineForTest(
            network: "Libera", channel: "#same", text: "message for 202",
            sessionID: 202, connectionID: 1, selfNick: "me")

        let sessionB202 = controller.sessionUUID(for: .runtime(id: 202))!
        let sessionB101 = controller.sessionUUID(for: .runtime(id: 101))!
        XCTAssertTrue(
            controller.messages.contains(where: { $0.sessionID == sessionB202 && $0.raw == "message for 202" }))
        XCTAssertFalse(
            controller.messages.contains(where: { $0.sessionID == sessionB101 && $0.raw == "message for 202" }))
    }

    func testServerAndChannelSessionsAreDistinctForUILists() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "server",
            sessionID: 1, connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#cybercafe",
            sessionID: 2, connectionID: 1, selfNick: "me")

        let serverUUID = controller.sessionUUID(for: .runtime(id: 1))
        let channelUUID = controller.sessionUUID(for: .runtime(id: 2))
        XCTAssertTrue(controller.sessions.contains(where: { $0.id == serverUUID && $0.channel == "server" }))
        XCTAssertTrue(
            controller.sessions.contains(where: { $0.id == channelUUID && $0.channel == "#cybercafe" }))
        XCTAssertEqual(controller.networkSections.first(where: { $0.name == "AfterNET" })?.sessions.count, 2)
    }

    func testChannelUserlistDoesNotPopulateServerSessionUsers() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "server",
            sessionID: 1, connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "AfterNET", channel: "#cybercafe",
            sessionID: 2, connectionID: 1, selfNick: "me")

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "AfterNET", channel: "#cybercafe",
            nick: "alice", sessionID: 2, connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "AfterNET", channel: "#cybercafe",
            nick: "bob", modePrefix: "@", sessionID: 2, connectionID: 1)

        controller.selectedSessionID = controller.sessionUUID(for: .runtime(id: 1))
        XCTAssertTrue(controller.visibleUsers.isEmpty)

        controller.selectedSessionID = controller.sessionUUID(for: .runtime(id: 2))
        XCTAssertEqual(controller.visibleUsers.map(\.nick), ["bob", "alice"])
        XCTAssertEqual(controller.visibleUsers.map(\.modePrefix), ["@", nil])
    }

    func testServerAndChannelMessagesRemainRoutedToOwnSessions() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "server",
            sessionID: 1, connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#cybercafe",
            sessionID: 2, connectionID: 1, selfNick: "me")

        controller.applyLogLineForTest(
            network: "AfterNET", channel: "server", text: "-server notice-",
            sessionID: 1, connectionID: 1, selfNick: "me")
        controller.applyLogLineForTest(
            network: "AfterNET", channel: "#cybercafe", text: "<alice> hi",
            sessionID: 2, connectionID: 1, selfNick: "me")

        let serverSession = controller.sessionUUID(for: .runtime(id: 1))!
        let channelSession = controller.sessionUUID(for: .runtime(id: 2))!
        XCTAssertTrue(
            controller.messages.contains(where: { $0.sessionID == serverSession && $0.raw == "-server notice-" }))
        XCTAssertTrue(
            controller.messages.contains(where: { $0.sessionID == channelSession && $0.raw == "<alice> hi" }))
        XCTAssertFalse(
            controller.messages.contains(where: { $0.sessionID == serverSession && $0.raw == "<alice> hi" }))
        XCTAssertFalse(
            controller.messages.contains(where: { $0.sessionID == channelSession && $0.raw == "-server notice-" }))
    }

    func testSessionLocatorRoundTripsComposedAndRuntimeKeys() {
        let conn1 = UUID()
        let conn2 = UUID()
        let a = SessionLocator.composed(connectionID: conn1, channel: "#a")
        let b = SessionLocator.composed(connectionID: conn1, channel: "#A")
        XCTAssertEqual(a, b, "channel comparison stays case-insensitive")

        let c = SessionLocator.composed(connectionID: conn2, channel: "#a")
        XCTAssertNotEqual(a, c, "distinct connectionIDs imply distinct locators")

        let runtime = SessionLocator.runtime(id: 42)
        XCTAssertNotEqual(runtime, SessionLocator.runtime(id: 43))
        XCTAssertNotEqual(runtime, a)

        var seen: Set<SessionLocator> = []
        seen.insert(a)
        XCTAssertTrue(seen.contains(.composed(connectionID: conn1, channel: "#A")))
    }

    func testVisibleSessionIDFallbackWhenNoSessions() {
        let controller = EngineController()
        XCTAssertTrue(controller.sessions.isEmpty)
        // With no sessions, visibleSessionID must return the system session's composed key
        // (UUID-based since the flip). Verify it is non-empty and consistent.
        let id1 = controller.visibleSessionID
        let id2 = controller.visibleSessionID
        XCTAssertEqual(id1, id2, "visibleSessionID must be stable across calls")
        XCTAssertFalse(id1.isEmpty, "visibleSessionID must not be empty")
        XCTAssertTrue(id1.hasSuffix("::server"), "visibleSessionID must end with ::server for system session")
        // And visibleMessages must simply be empty, not crash.
        XCTAssertTrue(controller.visibleMessages.isEmpty)
        XCTAssertTrue(controller.visibleUsers.isEmpty)
    }

    func testVisibleSessionIDPrefersSelectedOverActiveOverFirst() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "AfterNET", channel: "#b",
            connectionID: 1, selfNick: "me")

        let conn = controller.connectionsByServerID[1]!
        let aUUID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        controller.selectedSessionID = aUUID
        XCTAssertEqual(
            controller.visibleSessionID,
            SessionLocator.composed(connectionID: conn, channel: "#a").composedKey,
            "selected takes precedence over active")
        controller.selectedSessionID = nil
        XCTAssertEqual(
            controller.visibleSessionID,
            SessionLocator.composed(connectionID: conn, channel: "#b").composedKey,
            "active chosen when selected is nil")
        controller.activeSessionID = nil
        // both selected and active are now nil — should fall back to sessions.first
        // sessions are sorted so #a comes first alphabetically.
        XCTAssertEqual(
            controller.visibleSessionID,
            SessionLocator.composed(connectionID: conn, channel: "#a").composedKey,
            "first session used when both selected and active are nil")
    }

    func testChatSessionCarriesStableUUIDAcrossMutations() {
        var session = ChatSession(connectionID: UUID(), channel: "#a", isActive: false)
        let originalID = session.id
        session.channel = "#renamed"
        XCTAssertEqual(session.id, originalID, "id is stable across mutations")
    }

    func testSessionByLocatorIndexPopulatesAndRemoves() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let locator = SessionLocator.composed(connectionID: conn, channel: "#a")
        XCTAssertNotNil(controller.sessionUUID(for: locator))

        controller.applySessionForTest(
            action: HC_APPLE_SESSION_REMOVE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        XCTAssertNil(controller.sessionUUID(for: locator))
    }

    func testSessionByLocatorIndexHandlesRuntimeID() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#same",
            sessionID: 42, connectionID: 1, selfNick: "me")
        XCTAssertNotNil(controller.sessionUUID(for: .runtime(id: 42)))
    }

    func testSessionByLocatorPurgesStaleCompositesOnRename() {
        // If a session's channel changes, stale composed locators must not linger.
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#old",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let oldLocator = SessionLocator.composed(connectionID: conn, channel: "#old")
        let uuid = controller.sessionUUID(for: oldLocator)!

        // Mutate the session in place. applyRenameForTest simulates a rename.
        controller.applyRenameForTest(network: "Libera", fromChannel: "#old", toChannel: "#new")
        XCTAssertNil(controller.sessionUUID(for: oldLocator), "old composed locator must purge")
        XCTAssertEqual(
            controller.sessionUUID(for: .composed(connectionID: conn, channel: "#new")), uuid)
    }

    func testLifecycleStoppedClearsLocatorIndex() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)
        XCTAssertNil(controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a")))
    }

    func testSessionRemovePurgesRuntimeLocator() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#r",
            sessionID: 77, connectionID: 1, selfNick: "me")
        XCTAssertNotNil(controller.sessionUUID(for: .runtime(id: 77)))

        controller.applySessionForTest(
            action: HC_APPLE_SESSION_REMOVE, network: "AfterNET", channel: "#r",
            sessionID: 77, connectionID: 1, selfNick: "me")
        XCTAssertNil(controller.sessionUUID(for: .runtime(id: 77)))
    }

    func testUsersBySessionIsKeyedByUUID() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let uuid = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        XCTAssertEqual(controller.usersBySession[uuid]?.map(\.nick), ["alice"])
    }

    func testChatMessageSessionIDIsUUID() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyLogLineForTest(
            network: "Libera", channel: "#a", text: "hello",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let uuid = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        XCTAssertEqual(controller.messages.last?.sessionID, uuid)
    }

    func testUnattributableMessageStillReceivesSessionUUID() {
        // Before any session exists, route a manually-crafted unattributable message through appendMessage.
        // The message must still land with *some* non-nil UUID (the synthetic system session).
        let controller = EngineController()
        controller.appendUnattributedForTest(raw: "! system error", kind: .error(body: "system error"))
        XCTAssertFalse(controller.messages.isEmpty)
        XCTAssertFalse(controller.sessions.isEmpty)
        XCTAssertEqual(controller.messages.last?.sessionID, controller.sessions.first?.id)
    }

    func testSessionRemoveReselectsActiveAndClearsSelectedWhenMatching() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "AfterNET", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#b",
            connectionID: 1, selfNick: "me")

        let conn = controller.connectionsByServerID[1]!
        let aUUID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        let bUUID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#b"))!
        controller.selectedSessionID = aUUID
        XCTAssertEqual(controller.activeSessionID, aUUID)

        // Put users in #a so we can assert the cleanup.
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "AfterNET", channel: "#a",
            nick: "alice", connectionID: 1)
        XCTAssertFalse(controller.usersBySession[aUUID, default: []].isEmpty)

        controller.applySessionForTest(
            action: HC_APPLE_SESSION_REMOVE, network: "AfterNET", channel: "#a",
            connectionID: 1, selfNick: "me")

        XCTAssertFalse(controller.sessions.contains(where: { $0.id == aUUID }), "#a must be gone")
        XCTAssertNil(controller.selectedSessionID, "selected must clear when its session is removed")
        XCTAssertEqual(controller.activeSessionID, bUUID, "active must reassign to a remaining session")
        XCTAssertNil(controller.usersBySession[aUUID], "usersBySession entry must be cleaned up")
        // Exactly one session should have isActive == true, and it should be #b.
        let actives = controller.sessions.filter { $0.isActive }
        XCTAssertEqual(actives.map(\.id), [bUUID])
    }

    func testSelectedSessionIDIsUUIDAndRoutesRuntimeCommands() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#same",
            sessionID: 7, connectionID: 1, selfNick: "me")
        let uuid = controller.sessionUUID(for: .runtime(id: 7))!
        controller.selectedSessionID = uuid
        XCTAssertEqual(controller.numericRuntimeSessionID(forSelection: uuid), 7)
    }

    func testActiveSessionIDIsUUID() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let uuid = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        XCTAssertEqual(controller.activeSessionID, uuid)
    }

    func testChatSessionIDIsUUID() {
        let session = ChatSession(connectionID: UUID(), channel: "#a", isActive: false)
        let _: UUID = session.id  // compile-time type assertion
        let another = ChatSession(connectionID: UUID(), channel: "#b", isActive: false)
        XCTAssertNotEqual(session.id, another.id, "default UUIDs must be distinct")
    }

    func testUnattributedMessageRegistersSystemSessionLocator() {
        let controller = EngineController()
        controller.appendUnattributedForTest(raw: "! system error", kind: .error(body: "system error"))
        let systemConn = controller.systemConnectionUUIDForTest()
        XCTAssertNotNil(
            controller.sessionUUID(for: .composed(connectionID: systemConn, channel: EngineController.SystemSession.channel)),
            "system session must be registered in sessionByLocator so real events fold into it")
    }

    func testChatUserIdentityIsCaseInsensitiveNick() {
        let a = ChatUser(
            nick: "Alice", modePrefix: "@", account: "alice_acct",
            host: "host.example", isMe: false, isAway: false)
        let b = ChatUser(
            nick: "alice", modePrefix: nil, account: nil, host: nil,
            isMe: true, isAway: true)
        XCTAssertEqual(a.id, b.id, "ChatUser identity must be case-insensitive on nick")
        XCTAssertNotEqual(a, b, "Equality is field-by-field; identity is nick alone")
    }

    func testChatUserDefaultsAreSafe() {
        let user = ChatUser(nick: "bob")
        XCTAssertNil(user.modePrefix)
        XCTAssertNil(user.account)
        XCTAssertNil(user.host)
        XCTAssertFalse(user.isMe)
        XCTAssertFalse(user.isAway)
    }

    func testVisibleUsersReturnStructuredChatUsers() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", modePrefix: "@", account: "alice_acct", isMe: false, isAway: false,
            connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "bob", modePrefix: "+", isMe: true, isAway: false, connectionID: 1)

        let users = controller.visibleUsers
        XCTAssertEqual(users.map(\.nick), ["alice", "bob"], "ops then voiced; identical-rank sorted by name")
        XCTAssertEqual(users.first?.modePrefix, "@")
        XCTAssertEqual(users.first?.account, "alice_acct")
        XCTAssertTrue(users[1].isMe)
    }

    func testApplyUserlistForTestPropagatesMetadataToRuntimeEvent() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera",
            channel: "#a",
            nick: "alice",
            modePrefix: "@",
            account: "alice_acct",
            host: "alice.example",
            isMe: false,
            isAway: true,
            connectionID: 1
        )
        XCTAssertFalse(controller.usersBySession.isEmpty)
    }

    func testApplyForTestPropagatesConnectionIdentity() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE,
            network: "Libera", channel: "#a",
            sessionID: 0,
            connectionID: 99, selfNick: "alice"
        )
        let connectionID = controller.connectionsByServerID[99]
        XCTAssertNotNil(connectionID, "connectionID must register in connectionsByServerID")
        XCTAssertEqual(controller.connections[connectionID!]?.selfNick, "alice")
        XCTAssertEqual(controller.connections[connectionID!]?.serverName, "Libera")
    }

    func testUserlistUpdateOverwritesAwayFlag() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", modePrefix: "@", isAway: false, connectionID: 1)
        XCTAssertEqual(controller.visibleUsers.first?.isAway, false)

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
            nick: "alice", modePrefix: "@", isAway: true, connectionID: 1)
        XCTAssertEqual(controller.visibleUsers.count, 1, "UPDATE must not duplicate the user")
        XCTAssertEqual(
            controller.visibleUsers.first?.isAway, true,
            "UPDATE overwrites the prior record with fresh state")
    }

    func testUserlistUpdatePopulatesAccountAndHost() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", connectionID: 1)
        XCTAssertNil(controller.visibleUsers.first?.account)
        XCTAssertNil(controller.visibleUsers.first?.host)

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
            nick: "alice", account: "alice_acct", host: "alice.example", connectionID: 1)
        XCTAssertEqual(controller.visibleUsers.first?.account, "alice_acct")
        XCTAssertEqual(controller.visibleUsers.first?.host, "alice.example")
    }

    func testUserlistUpdateClearsAccountToNil() {
        // The crux of overwrite-vs-merge: a logout (account=nil from the C side)
        // must clear the previously-non-nil account. A merge would silently retain
        // the stale value.
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", account: "alice_acct", connectionID: 1)
        XCTAssertEqual(controller.visibleUsers.first?.account, "alice_acct")

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
            nick: "alice", account: nil, connectionID: 1)
        XCTAssertNil(
            controller.visibleUsers.first?.account, "logout / account-clear must overwrite, not merge")
    }

    func testUserlistInsertCarriesIsMe() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "me", isMe: true, connectionID: 1)
        XCTAssertTrue(controller.visibleUsers.first?.isMe ?? false)
    }

    func testUserlistRemoveByNickIsCaseInsensitive() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "Alice", modePrefix: "@", connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_REMOVE, network: "Libera", channel: "#a",
            nick: "ALICE", connectionID: 1)
        XCTAssertTrue(controller.visibleUsers.isEmpty)
    }

    func testUserlistEmptyNickIsIgnored() {
        // The C side should never emit an empty nick on INSERT/UPDATE/REMOVE, but
        // a defensive guard keeps a malformed event from corrupting the roster.
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "", connectionID: 1)
        XCTAssertTrue(controller.visibleUsers.isEmpty)

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_REMOVE, network: "Libera", channel: "#a",
            nick: "", connectionID: 1)
        XCTAssertEqual(
            controller.visibleUsers.map(\.nick), ["alice"], "empty REMOVE must not delete real users")
    }

    func testUserlistFallsBackToSystemSessionWhenNetworkOrChannelMissing() {
        // C events with NULL network/channel land in the synthetic system session
        // (same fallback path as unattributable log lines).
        let controller = EngineController()
        controller.applyUserlistRawForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: nil, channel: nil, nick: "alice")
        let systemConn = controller.systemConnectionUUIDForTest()
        let systemUUID = controller.sessionUUID(for: .composed(connectionID: systemConn, channel: EngineController.SystemSession.channel))
        XCTAssertNotNil(systemUUID, "system session must be registered as the fallback target")
        XCTAssertEqual(controller.usersBySession[systemUUID!]?.map(\.nick), ["alice"])
    }

    func testUnattributedMessageAndUserlistShareSystemSession() {
        // Phase-1 latent bug regression: appendMessage(without event) and
        // userlist-with-NULL-network must converge on the SAME system session.
        // Order: userlist first, then message.
        let controller = EngineController()
        controller.applyUserlistRawForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: nil, channel: nil, nick: "alice")
        controller.appendUnattributedForTest(raw: "! system error", kind: .error(body: "system error"))

        let systemConn = controller.systemConnectionUUIDForTest()
        let systemSessions = controller.sessions.filter {
            $0.locator == .composed(connectionID: systemConn, channel: EngineController.SystemSession.channel)
        }
        XCTAssertEqual(systemSessions.count, 1, "must converge on a single system session, not duplicate")
        let systemUUID = systemSessions[0].id
        XCTAssertEqual(controller.messages.last?.sessionID, systemUUID)
        XCTAssertEqual(controller.usersBySession[systemUUID]?.map(\.nick), ["alice"])
    }

    func testUnattributedMessageBeforeUserlistAlsoConverges() {
        // Reverse-order variant: message first creates the system session via
        // systemSessionUUID(); a subsequent userlist-with-NULL-network must
        // reuse that same session, not create a second one.
        let controller = EngineController()
        controller.appendUnattributedForTest(raw: "! system error", kind: .error(body: "system error"))
        controller.applyUserlistRawForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: nil, channel: nil, nick: "alice")

        let systemConn = controller.systemConnectionUUIDForTest()
        let systemSessions = controller.sessions.filter {
            $0.locator == .composed(connectionID: systemConn, channel: EngineController.SystemSession.channel)
        }
        XCTAssertEqual(systemSessions.count, 1, "reverse order must also converge")
        let systemUUID = systemSessions[0].id
        XCTAssertEqual(controller.messages.last?.sessionID, systemUUID)
        XCTAssertEqual(controller.usersBySession[systemUUID]?.map(\.nick), ["alice"])
    }

    func testSystemSessionUUIDReusesExistingLocatorRegistration() {
        // Direct mechanism test: after a userlist event creates the system-locator
        // session via upsertSession (without touching systemSessionUUIDStorage),
        // a direct call to systemSessionUUID() must return the SAME UUID, not create a duplicate.
        let controller = EngineController()
        controller.applyUserlistRawForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: nil, channel: nil, nick: "alice")
        let systemConn = controller.systemConnectionUUIDForTest()
        let upsertedUUID = controller.sessionUUID(for:
            .composed(connectionID: systemConn, channel: EngineController.SystemSession.channel))!
        XCTAssertEqual(
            controller.systemSessionUUIDForTest(), upsertedUUID,
            "systemSessionUUID() must reuse the existing sessionByLocator entry")
        XCTAssertEqual(
            controller.sessions.filter {
                $0.locator == .composed(connectionID: systemConn, channel: EngineController.SystemSession.channel)
            }.count,
            1,
            "no duplicate system session was created"
        )
    }

    func testUserlistUpdateFlipsIsMe() {
        // Theoretical: should never happen in practice (isMe is a stable property of
        // the local connection's own User), but the model must not trap on the flip.
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", isMe: false, connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
            nick: "alice", isMe: true, connectionID: 1)
        XCTAssertEqual(controller.visibleUsers.count, 1)
        XCTAssertTrue(controller.visibleUsers.first?.isMe ?? false)

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
            nick: "alice", isMe: false, connectionID: 1)
        XCTAssertFalse(controller.visibleUsers.first?.isMe ?? true)
    }

    func testNetworkCarriesStableIDAndDisplayName() {
        var network = Network(id: UUID(), displayName: "AfterNET")
        let originalID = network.id
        network.displayName = "renamed"
        XCTAssertEqual(network.id, originalID, "Network.id is stable across displayName mutations")
    }

    func testConnectionCarriesNetworkFKAndSelfNick() {
        let networkID = UUID()
        var connection = Connection(
            id: UUID(), networkID: networkID,
            serverName: "irc.afternet.org", selfNick: "alice")
        XCTAssertEqual(connection.networkID, networkID)
        XCTAssertEqual(connection.selfNick, "alice")
        connection.selfNick = "alice_"
        XCTAssertEqual(connection.selfNick, "alice_", "selfNick is mutable")
    }

    func testUpsertNetworkIsCaseInsensitive() {
        let controller = EngineController()
        let a = controller.upsertNetworkForTest(name: "AfterNET")
        let b = controller.upsertNetworkForTest(name: "afternet")
        XCTAssertEqual(a, b, "AfterNET and afternet resolve to the same Network.id")
        XCTAssertEqual(
            controller.networks[a]?.displayName, "AfterNET",
            "displayName keeps the first-seen casing")
    }

    func testUpsertConnectionRegistersByServerID() {
        let controller = EngineController()
        let networkID = controller.upsertNetworkForTest(name: "Libera")
        let connectionID = controller.upsertConnectionForTest(
            serverID: 42, networkID: networkID, serverName: "irc.libera.chat", selfNick: "alice")
        XCTAssertEqual(controller.connectionsByServerID[42], connectionID)
        XCTAssertEqual(controller.connections[connectionID]?.networkID, networkID)
        XCTAssertEqual(controller.connections[connectionID]?.selfNick, "alice")
    }

    func testUpsertConnectionRefreshesSelfNickAndServerName() {
        let controller = EngineController()
        let networkID = controller.upsertNetworkForTest(name: "Libera")
        let first = controller.upsertConnectionForTest(
            serverID: 42, networkID: networkID, serverName: "irc.libera.chat", selfNick: "alice")
        let second = controller.upsertConnectionForTest(
            serverID: 42, networkID: networkID, serverName: "sol.libera.chat", selfNick: "alice_")
        XCTAssertEqual(first, second, "same serverID must resolve to same Connection UUID")
        XCTAssertEqual(controller.connections[first]?.selfNick, "alice_")
        XCTAssertEqual(controller.connections[first]?.serverName, "sol.libera.chat")
    }

    // MARK: - Multi-connection isolation

    func testTwoConnectionsToSameNetworkAreDistinct() {
        let controller = EngineController()
        // Two struct server* pointers, same configured network display name.
        // Use sessionID: 0 so composed locator (connectionID + channel) is used for storage.
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#c",
            sessionID: 0, connectionID: 1, selfNick: "alice")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#c",
            sessionID: 0, connectionID: 2, selfNick: "alice2")

        let conn1 = controller.connectionsByServerID[1]!
        let conn2 = controller.connectionsByServerID[2]!
        XCTAssertNotEqual(conn1, conn2,
            "Two connections with distinct server IDs must produce distinct Connection UUIDs")

        XCTAssertEqual(controller.networks.count, 1, "Same network name = one Network record")
        XCTAssertEqual(controller.connections.count, 2)
        XCTAssertEqual(controller.sessions.count, 2)
        XCTAssertEqual(controller.connections[conn1]?.networkID,
                       controller.connections[conn2]?.networkID)

        let section = controller.networkSections.first { $0.name == "AfterNET" }
        XCTAssertEqual(section?.sessions.count, 2)

        let sessionA = controller.sessionUUID(for: .composed(connectionID: conn1, channel: "#c"))
        let sessionB = controller.sessionUUID(for: .composed(connectionID: conn2, channel: "#c"))
        XCTAssertNotNil(sessionA)
        XCTAssertNotNil(sessionB)
        XCTAssertNotEqual(sessionA, sessionB)
    }

    func testNetworkSectionsGroupByNetworkIdentity() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 10, selfNick: "alice")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#b",
            sessionID: 2, connectionID: 20, selfNick: "alice")

        let names = controller.networkSections.map(\.name)
        XCTAssertEqual(names.sorted(), ["AfterNET", "Libera"])
    }

    // MARK: - Lifecycle teardown & system-session invariants

    func testLifecycleStoppedClearsNetworksAndConnections() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 1, selfNick: "me")
        XCTAssertFalse(controller.networks.isEmpty)
        XCTAssertFalse(controller.connections.isEmpty)
        XCTAssertFalse(controller.networksByName.isEmpty)
        XCTAssertFalse(controller.connectionsByServerID.isEmpty)

        controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)

        XCTAssertTrue(controller.networks.isEmpty)
        XCTAssertTrue(controller.connections.isEmpty)
        XCTAssertTrue(controller.networksByName.isEmpty)
        XCTAssertTrue(controller.connectionsByServerID.isEmpty)
        XCTAssertTrue(controller.sessionByLocator.isEmpty)
    }

    func testSelfNickRefreshesOnSubsequentEvent() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "alice")
        let connID = controller.connectionsByServerID[1]!
        XCTAssertEqual(controller.connections[connID]?.selfNick, "alice")

        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b",
            connectionID: 1, selfNick: "alice_")
        XCTAssertEqual(
            controller.connections[connID]?.selfNick, "alice_",
            "subsequent events refresh selfNick on the already-registered Connection")
    }

    func testSystemConnectionIsNotRegisteredByServerID() {
        let controller = EngineController()
        // Trigger system-session creation.
        controller.appendUnattributedForTest(raw: "! system error", kind: .error(body: "system error"))
        XCTAssertFalse(controller.connections.isEmpty, "system connection must be created")
        XCTAssertTrue(
            controller.connectionsByServerID.isEmpty,
            "system connection has no server-id slot — server_id == 0 is reserved")
    }

    func testSystemSessionHasSystemConnectionAndNetwork() {
        let controller = EngineController()
        controller.appendUnattributedForTest(raw: "! system error", kind: .error(body: "system error"))

        let systemNetworkID = controller.networksByName[EngineController.SystemSession.networkName]
        XCTAssertNotNil(systemNetworkID)
        let systemConnectionID = controller.systemConnectionUUIDForTest()
        XCTAssertEqual(controller.connections[systemConnectionID]?.networkID, systemNetworkID)
        XCTAssertEqual(
            controller.networks.values.filter {
                $0.displayName == EngineController.SystemSession.networkName
            }.count,
            1,
            "exactly one system Network")
    }

    func testConnectionIDZeroRoutesToSystemConnection() {
        // Events with connection_id == 0 (no struct server) reuse the system Connection
        // rather than spawning a fresh per-call Connection.
        let controller = EngineController()
        controller.applyLogLineForTest(
            network: nil, channel: nil,
            text: "first unattributed", sessionID: 0,
            connectionID: 0, selfNick: nil)
        controller.applyLogLineForTest(
            network: nil, channel: nil,
            text: "second unattributed", sessionID: 0,
            connectionID: 0, selfNick: nil)
        XCTAssertEqual(controller.connections.count, 1, "both events reuse the system connection")
    }

    func testNetworkDisplayNamePrefersFirstSeenCasing() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#c",
            connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "afternet", channel: "#d",
            connectionID: 1, selfNick: "me")
        // Same Network.id, first-seen display casing preserved.
        XCTAssertEqual(controller.networks.count, 1)
        XCTAssertEqual(controller.networks.values.first?.displayName, "AfterNET")
    }

    func testConnectionIDZeroIsReservedSentinelAndDistinctFromID1() {
        let controller = EngineController()
        // connectionID 0 must route to the system connection (not create a real one).
        controller.applyLogLineForTest(
            network: nil, channel: nil,
            text: "sentinel", sessionID: 0,
            connectionID: 0, selfNick: nil)
        XCTAssertTrue(
            controller.connectionsByServerID.isEmpty,
            "connectionID 0 must not populate connectionsByServerID")
        let systemConn = controller.systemConnectionUUIDForTest()

        // A real event with connectionID 1 must create a DISTINCT Connection.
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 0, connectionID: 1, selfNick: "me")
        let realConn = controller.connectionsByServerID[1]!
        XCTAssertNotEqual(
            realConn, systemConn,
            "Real server connections must be distinct from the system sentinel")
        XCTAssertEqual(
            controller.connections.count, 2,
            "One system connection + one real connection")
    }

    func testUserCarriesStableIDAndConnectionScopedIdentity() {
        let connID = UUID()
        var user = User(
            id: UUID(), connectionID: connID, nick: "alice",
            account: nil, hostmask: nil, isMe: false, isAway: false)
        let originalID = user.id
        user.nick = "alice_"
        XCTAssertEqual(user.id, originalID, "User.id is stable across nick changes")
        XCTAssertEqual(user.connectionID, connID, "User.connectionID is the identity scope")
    }

    func testChannelMembershipCarriesJunctionFields() {
        let sessionID = UUID()
        let userID = UUID()
        var membership = ChannelMembership(sessionID: sessionID, userID: userID, modePrefix: "@")
        XCTAssertEqual(membership.sessionID, sessionID)
        XCTAssertEqual(membership.userID, userID)
        XCTAssertEqual(membership.modePrefix, "@")
        membership.modePrefix = "+"
        XCTAssertEqual(membership.modePrefix, "+", "modePrefix is mutable for mode changes")
    }

    func testUserKeyIsCaseInsensitiveOnNick() {
        let connID = UUID()
        let a = UserKey(connectionID: connID, nick: "Alice")
        let b = UserKey(connectionID: connID, nick: "ALICE")
        XCTAssertEqual(a, b, "UserKey equality must be case-insensitive on nick")
        let seen: Set<UserKey> = [a]
        XCTAssertTrue(seen.contains(b), "UserKey hash parity")
        let otherConn = UserKey(connectionID: UUID(), nick: "alice")
        XCTAssertNotEqual(a, otherConn, "different connections must yield distinct keys")
    }

    func testUpsertUserRegistersByConnectionAndNick() {
        let controller = EngineController()
        let connID = UUID()
        let userID = controller.upsertUserForTest(
            connectionID: connID, nick: "alice",
            account: nil, hostmask: nil, isMe: false, isAway: false)
        XCTAssertEqual(controller.users[userID]?.nick, "alice")
        XCTAssertEqual(
            controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "ALICE")],
            userID, "lookup must be case-insensitive")
    }

    func testUpsertUserRefreshesMetadataWithoutCreatingDuplicate() {
        let controller = EngineController()
        let connID = UUID()
        let first = controller.upsertUserForTest(
            connectionID: connID, nick: "alice",
            account: nil, hostmask: nil, isMe: false, isAway: false)
        let second = controller.upsertUserForTest(
            connectionID: connID, nick: "alice",
            account: "alice!account", hostmask: "alice@host",
            isMe: false, isAway: true)
        XCTAssertEqual(first, second, "same (connection, nick) must resolve to same User UUID")
        XCTAssertEqual(controller.users[first]?.account, "alice!account")
        XCTAssertEqual(controller.users[first]?.hostmask, "alice@host")
        XCTAssertTrue(controller.users[first]?.isAway == true)
        XCTAssertEqual(controller.users.count, 1, "upsert must update in place, not append a second entry")
    }

    func testUpsertUserClearsAccountToNilOnSubsequentCall() {
        // Parallels Phase 2's testUserlistUpdateClearsAccountToNil: a later event with
        // account: nil must drop the previously-set account, not merge-preserve it.
        let controller = EngineController()
        let connID = UUID()
        let userID = controller.upsertUserForTest(
            connectionID: connID, nick: "alice",
            account: "alice!acct", hostmask: "alice@host",
            isMe: false, isAway: false)
        _ = controller.upsertUserForTest(
            connectionID: connID, nick: "alice",
            account: nil, hostmask: nil,
            isMe: false, isAway: false)
        XCTAssertNil(
            controller.users[userID]?.account,
            "account=nil on update must clear, not preserve")
        XCTAssertNil(
            controller.users[userID]?.hostmask,
            "hostmask=nil on update must clear, not preserve")
    }

    func testUpsertUserOnDifferentConnectionsAreDistinct() {
        let controller = EngineController()
        let a = controller.upsertUserForTest(
            connectionID: UUID(), nick: "alice",
            account: nil, hostmask: nil, isMe: false, isAway: false)
        let b = controller.upsertUserForTest(
            connectionID: UUID(), nick: "alice",
            account: nil, hostmask: nil, isMe: false, isAway: false)
        XCTAssertNotEqual(a, b, "same nick across connections must yield distinct User UUIDs")
    }

    func testSetMembershipAddsAndUpdatesModePrefix() {
        let controller = EngineController()
        let connID = UUID()
        let sessionID = UUID()
        let userID = controller.upsertUserForTest(
            connectionID: connID, nick: "alice",
            account: nil, hostmask: nil, isMe: false, isAway: false)
        controller.setMembershipForTest(sessionID: sessionID, userID: userID, modePrefix: "@")
        XCTAssertEqual(controller.membershipsBySession[sessionID]?.count, 1)
        XCTAssertEqual(controller.membershipsBySession[sessionID]?.first?.modePrefix, "@")
        // Second call updates in place.
        controller.setMembershipForTest(sessionID: sessionID, userID: userID, modePrefix: "+")
        XCTAssertEqual(controller.membershipsBySession[sessionID]?.count, 1, "no duplicate membership")
        XCTAssertEqual(controller.membershipsBySession[sessionID]?.first?.modePrefix, "+")
    }

    func testRemoveMembershipDropsByUserIDAndIsNoOpWhenAbsent() {
        let controller = EngineController()
        let connID = UUID()
        let sessionID = UUID()
        let userID = controller.upsertUserForTest(
            connectionID: connID, nick: "alice",
            account: nil, hostmask: nil, isMe: false, isAway: false)
        controller.setMembershipForTest(sessionID: sessionID, userID: userID, modePrefix: nil)
        XCTAssertEqual(controller.membershipsBySession[sessionID]?.count, 1)

        // removeMembership is private; exercise it via a new test helper (add below).
        controller.removeMembershipForTest(sessionID: sessionID, userID: userID)
        XCTAssertTrue(controller.membershipsBySession[sessionID, default: []].isEmpty)

        // Second call (no entry) is a silent no-op.
        controller.removeMembershipForTest(sessionID: sessionID, userID: userID)
        XCTAssertTrue(controller.membershipsBySession[sessionID, default: []].isEmpty)

        // Removing from a never-seen session is also a silent no-op.
        controller.removeMembershipForTest(sessionID: UUID(), userID: userID)
        // no crash, no assertion needed beyond reaching this line
    }

    func testUserlistInsertPopulatesMembershipAndUser() {
        let controller = EngineController()
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: "#a", nick: "alice",
            modePrefix: "@",
            account: "alice!acct",
            host: "alice@host",
            sessionID: 1, connectionID: 1, selfNick: "me")
        // New storage populated.
        let connUUID = controller.connectionsByServerID[1]!
        let userUUID = controller.usersByConnectionAndNick[UserKey(connectionID: connUUID, nick: "alice")]
        XCTAssertNotNil(userUUID, "USERLIST_INSERT must create User")
        XCTAssertEqual(controller.users[userUUID!]?.account, "alice!acct")
        XCTAssertEqual(controller.users[userUUID!]?.hostmask, "alice@host")
        let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
        XCTAssertEqual(controller.membershipsBySession[sessionUUID]?.count, 1)
        XCTAssertEqual(controller.membershipsBySession[sessionUUID]?.first?.modePrefix, "@")
        // Computed projection confirms read path works end-to-end.
        XCTAssertEqual(controller.usersBySession[sessionUUID]?.map(\.nick), ["alice"])
    }

    func testUserlistRemoveDropsMembershipButLeavesUser() {
        let controller = EngineController()
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: "#a", nick: "alice",
            sessionID: 1, connectionID: 1, selfNick: "me")
        let connUUID = controller.connectionsByServerID[1]!
        let userUUID = controller.usersByConnectionAndNick[UserKey(connectionID: connUUID, nick: "alice")]!

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_REMOVE,
            network: "Libera", channel: "#a", nick: "alice",
            sessionID: 1, connectionID: 1, selfNick: "me")

        let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
        XCTAssertTrue(
            controller.membershipsBySession[sessionUUID, default: []].isEmpty,
            "membership removed")
        XCTAssertNotNil(
            controller.users[userUUID],
            "User record remains for potential re-join / other channel memberships")
    }

    func testUserlistClearDropsAllMembershipsForSession() {
        let controller = EngineController()
        for nick in ["alice", "bob"] {
            controller.applyUserlistForTest(
                action: HC_APPLE_USERLIST_INSERT,
                network: "Libera", channel: "#a", nick: nick,
                sessionID: 1, connectionID: 1, selfNick: "me")
        }
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_CLEAR,
            network: "Libera", channel: "#a", nick: "",
            sessionID: 1, connectionID: 1, selfNick: "me")
        let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
        XCTAssertTrue(controller.membershipsBySession[sessionUUID, default: []].isEmpty)
    }

    func testUsersBySessionProjectionPreservesUserSortOrder() {
        let controller = EngineController()
        for (nick, prefix) in [("bob", "+" as Character?), ("alice", "@"), ("carol", nil)] {
            controller.applyUserlistForTest(
                action: HC_APPLE_USERLIST_INSERT,
                network: "Libera", channel: "#a", nick: nick,
                modePrefix: prefix, account: nil, host: nil, isMe: false, isAway: false,
                sessionID: 1, connectionID: 1, selfNick: "me")
        }
        let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
        XCTAssertEqual(
            controller.usersBySession[sessionUUID]?.map(\.nick),
            ["alice", "bob", "carol"],
            "@ op outranks + voice; unprefixed sorted by nick")
    }

    func testSameNickOnTwoConnectionsAreDistinctUsers() {
        let controller = EngineController()
        // Same configured network name, two distinct server-IDs → two Connections.
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "AfterNET", channel: "#a", nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me1")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "AfterNET", channel: "#a", nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 2, selfNick: "me2")

        let conn1 = controller.connectionsByServerID[1]!
        let conn2 = controller.connectionsByServerID[2]!
        let alice1 = controller.usersByConnectionAndNick[UserKey(connectionID: conn1, nick: "alice")]!
        let alice2 = controller.usersByConnectionAndNick[UserKey(connectionID: conn2, nick: "alice")]!
        XCTAssertNotEqual(alice1, alice2,
            "Two connections to same-named network must dedup users separately")
        XCTAssertEqual(controller.users.count, 2)
    }

    func testSessionRemoveDropsMembershipsForThatSession() {
        let controller = EngineController()
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: "#a", nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 1, connectionID: 1, selfNick: "me")
        let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
        XCTAssertEqual(controller.membershipsBySession[sessionUUID]?.count, 1)

        controller.applySessionForTest(
            action: HC_APPLE_SESSION_REMOVE,
            network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 1, selfNick: "me")

        XCTAssertNil(
            controller.membershipsBySession[sessionUUID],
            "memberships entry cleared on session removal")
        XCTAssertFalse(
            controller.users.isEmpty,
            "User record survives session removal — session-remove is not user-remove")
    }

    func testLifecycleStoppedClearsUsersAndMemberships() {
        let controller = EngineController()
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: "#a", nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 1, connectionID: 1, selfNick: "me")
        XCTAssertFalse(controller.users.isEmpty)
        XCTAssertFalse(controller.usersByConnectionAndNick.isEmpty)
        XCTAssertFalse(controller.membershipsBySession.isEmpty)

        controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)

        XCTAssertTrue(controller.users.isEmpty)
        XCTAssertTrue(controller.usersByConnectionAndNick.isEmpty)
        XCTAssertTrue(controller.membershipsBySession.isEmpty)
    }

    // MARK: - Fan-out correctness + non-fan-out isolation

    func testAwayUpdateOnOneChannelFansOutToAllChannelsOfSameUser() {
        let controller = EngineController()
        for channel in ["#a", "#b", "#c"] {
            controller.applyUserlistForTest(
                action: HC_APPLE_USERLIST_INSERT,
                network: "Libera", channel: channel, nick: "alice",
                modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
                sessionID: 0, connectionID: 1, selfNick: "me")
        }
        // Single UPDATE on one channel; fan-out must hit the other two.
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE,
            network: "Libera", channel: "#a", nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: true,
            sessionID: 0, connectionID: 1, selfNick: "me")

        let connID = controller.connectionsByServerID[1]!
        let userID = controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "alice")]!
        XCTAssertEqual(controller.users[userID]?.isAway, true)
        // Projection reflects the same isAway across every channel.
        for channel in ["#a", "#b", "#c"] {
            let sessionUUID = controller.sessionUUID(
                for: .composed(connectionID: connID, channel: channel))!
            XCTAssertEqual(controller.usersBySession[sessionUUID]?.first?.isAway, true,
                           "\(channel) projection must reflect User.isAway = true")
        }
        XCTAssertEqual(controller.users.count, 1, "single User record for fan-out")
    }

    func testAccountUpdateOnOneChannelFansOutToAllChannels() {
        let controller = EngineController()
        for channel in ["#a", "#b"] {
            controller.applyUserlistForTest(
                action: HC_APPLE_USERLIST_INSERT,
                network: "Libera", channel: channel, nick: "alice",
                modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
                sessionID: 0, connectionID: 1, selfNick: "me")
        }
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE,
            network: "Libera", channel: "#a", nick: "alice",
            modePrefix: nil, account: "alice!authname", host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me")

        let connID = controller.connectionsByServerID[1]!
        for channel in ["#a", "#b"] {
            let sessionUUID = controller.sessionUUID(
                for: .composed(connectionID: connID, channel: channel))!
            XCTAssertEqual(controller.usersBySession[sessionUUID]?.first?.account, "alice!authname")
        }
    }

    func testModePrefixIsMembershipLocalAndDoesNotFanOut() {
        // modePrefix lives on ChannelMembership, not User. An op in #a is NOT an op in #b.
        let controller = EngineController()
        for (channel, prefix) in [("#a", "@" as Character?), ("#b", nil)] {
            controller.applyUserlistForTest(
                action: HC_APPLE_USERLIST_INSERT,
                network: "Libera", channel: channel, nick: "alice",
                modePrefix: prefix, account: nil, host: nil, isMe: false, isAway: false,
                sessionID: 0, connectionID: 1, selfNick: "me")
        }
        let connID = controller.connectionsByServerID[1]!
        let aUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
        let bUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!
        XCTAssertEqual(controller.usersBySession[aUUID]?.first?.modePrefix, "@")
        XCTAssertNil(controller.usersBySession[bUUID]?.first?.modePrefix,
                     "mode prefix in #a must not bleed into #b")

        // Flip #a op → voice. #b must remain unprefixed.
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE,
            network: "Libera", channel: "#a", nick: "alice",
            modePrefix: "+", account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me")
        XCTAssertEqual(controller.usersBySession[aUUID]?.first?.modePrefix, "+")
        XCTAssertNil(controller.usersBySession[bUUID]?.first?.modePrefix,
                     "mode change in #a must remain isolated to #a")
    }

    func testRemoveInOneChannelLeavesOtherChannelMembershipIntact() {
        let controller = EngineController()
        for channel in ["#a", "#b"] {
            controller.applyUserlistForTest(
                action: HC_APPLE_USERLIST_INSERT,
                network: "Libera", channel: channel, nick: "alice",
                modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
                sessionID: 0, connectionID: 1, selfNick: "me")
        }
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_REMOVE,
            network: "Libera", channel: "#a", nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me")

        let connID = controller.connectionsByServerID[1]!
        let aUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
        let bUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!
        XCTAssertTrue(controller.membershipsBySession[aUUID, default: []].isEmpty,
                      "#a membership dropped")
        XCTAssertEqual(controller.membershipsBySession[bUUID]?.count, 1,
                       "#b membership must remain")
        // User record itself remains — still in #b.
        let userID = controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "alice")]
        XCTAssertNotNil(userID, "User record survives a single-channel REMOVE")
    }

    func testClearInOneChannelLeavesOtherChannelMembershipsIntact() {
        let controller = EngineController()
        for channel in ["#a", "#b"] {
            for nick in ["alice", "bob"] {
                controller.applyUserlistForTest(
                    action: HC_APPLE_USERLIST_INSERT,
                    network: "Libera", channel: channel, nick: nick,
                    modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
                    sessionID: 0, connectionID: 1, selfNick: "me")
            }
        }
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_CLEAR,
            network: "Libera", channel: "#a", nick: "",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 0, connectionID: 1, selfNick: "me")

        let connID = controller.connectionsByServerID[1]!
        let aUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
        let bUUID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!
        XCTAssertTrue(controller.membershipsBySession[aUUID, default: []].isEmpty,
                      "#a memberships cleared")
        XCTAssertEqual(controller.membershipsBySession[bUUID]?.count, 2,
                       "#b memberships untouched by a CLEAR on #a")
        XCTAssertEqual(controller.users.count, 2, "Users persist across a single-channel CLEAR")
    }

    // MARK: - Phase 5 — message structuring

    func testMessageAuthorCarriesNickAndOptionalUserID() {
        let bare = MessageAuthor(nick: "alice", userID: nil)
        XCTAssertEqual(bare.nick, "alice")
        XCTAssertNil(bare.userID)
        let resolved = MessageAuthor(nick: "alice", userID: UUID())
        XCTAssertNotNil(resolved.userID)
        // ChatMessage.author defaults to nil when the constructor omits it.
        let m = ChatMessage(sessionID: UUID(), raw: "x", kind: .join)
        XCTAssertNil(m.author)
    }

    func testChatMessageKindHoldsTypedPayloads() {
        let join: ChatMessageKind = .join
        let part: ChatMessageKind = .part(reason: "later")
        let kick: ChatMessageKind = .kick(target: "bob", reason: "spam")
        let nick: ChatMessageKind = .nickChange(from: "alice", to: "alice_")
        let mode: ChatMessageKind = .modeChange(modes: "+o", args: "alice")
        let priv: ChatMessageKind = .message(body: "hi")
        XCTAssertEqual(join, .join)
        XCTAssertEqual(part, .part(reason: "later"))
        XCTAssertNotEqual(part, .part(reason: nil))
        XCTAssertEqual(kick, .kick(target: "bob", reason: "spam"))
        XCTAssertEqual(nick, .nickChange(from: "alice", to: "alice_"))
        XCTAssertEqual(mode, .modeChange(modes: "+o", args: "alice"))
        XCTAssertEqual(priv, .message(body: "hi"))
    }

    func testChatMessageBodyComputedFromKindForFreeTextCases() {
        let m = ChatMessage(
            sessionID: UUID(), raw: "hi", kind: .message(body: "hi"),
            author: nil, timestamp: Date())
        XCTAssertEqual(m.body, "hi")
        let j = ChatMessage(
            sessionID: UUID(), raw: "* alice has joined #a", kind: .join,
            author: MessageAuthor(nick: "alice", userID: nil), timestamp: Date())
        XCTAssertNil(j.body, "structured kinds have no free-form body")
        XCTAssertEqual(j.author?.nick, "alice")
        let l = ChatMessage(
            sessionID: UUID(), raw: "[READY] up", kind: .lifecycle(phase: "READY", body: "up"),
            author: nil, timestamp: Date())
        XCTAssertEqual(l.body, "up", "lifecycle body extracts the second associated value")
    }

    func testChatMessageTimestampReflectsConstructorArgument() {
        let t = Date(timeIntervalSince1970: 1_700_000_000)
        let m = ChatMessage(
            sessionID: UUID(), raw: "x", kind: .error(body: "x"),
            author: nil, timestamp: t)
        XCTAssertEqual(m.timestamp, t)
    }

    func testResolveAuthorReturnsNickWithNilUserIDWhenUserAbsent() {
        let controller = EngineController()
        let connID = UUID()
        let author = controller.resolveAuthor(connectionID: connID, nick: "alice")
        XCTAssertEqual(author.nick, "alice")
        XCTAssertNil(author.userID, "no User exists yet → userID is nil")
    }

    func testResolveAuthorFindsUserIDWhenUserPresent() {
        let controller = EngineController()
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: "#a", nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 1, connectionID: 1, selfNick: "me")
        let connID = controller.connectionsByServerID[1]!
        let userID = controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "alice")]!
        let author = controller.resolveAuthor(connectionID: connID, nick: "alice")
        XCTAssertEqual(author.userID, userID)
        // Case-insensitive: lookups go through UserKey which lowercases.
        let upper = controller.resolveAuthor(connectionID: connID, nick: "ALICE")
        XCTAssertEqual(upper.userID, userID)
        // Original nick casing is preserved on the author.
        XCTAssertEqual(upper.nick, "ALICE")
    }

    // MARK: - Task 7 — MEMBERSHIP_CHANGE typed events

    func testMembershipJoinAppendsTypedMessageWithResolvedAuthor() {
        let controller = EngineController()
        // Pre-seed the User so author.userID resolves.
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: "#a", nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 1, connectionID: 1, selfNick: "me")
        let connID = controller.connectionsByServerID[1]!
        let userID = controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "alice")]

        controller.applyMembershipForTest(
            action: HC_APPLE_MEMBERSHIP_JOIN,
            network: "Libera", channel: "#a", nick: "alice",
            sessionID: 1, connectionID: 1, selfNick: "me")

        let sessionUUID = controller.sessionUUID(for: .runtime(id: 1))!
        let last = controller.messages.last!
        XCTAssertEqual(last.sessionID, sessionUUID)
        XCTAssertEqual(last.kind, .join)
        XCTAssertEqual(last.author?.nick, "alice")
        XCTAssertEqual(last.author?.userID, userID)
    }

    func testMembershipPartCarriesReasonOptionally() {
        let controller = EngineController()
        controller.applyMembershipForTest(
            action: HC_APPLE_MEMBERSHIP_PART,
            network: "Libera", channel: "#a", nick: "alice",
            reason: "later",
            sessionID: 1, connectionID: 1, selfNick: "me")
        let part = controller.messages.last!
        XCTAssertEqual(part.kind, .part(reason: "later"))

        controller.applyMembershipForTest(
            action: HC_APPLE_MEMBERSHIP_PART,
            network: "Libera", channel: "#a", nick: "bob",
            reason: nil,
            sessionID: 1, connectionID: 1, selfNick: "me")
        XCTAssertEqual(controller.messages.last?.kind, .part(reason: nil))
    }

    func testMembershipKickCarriesTargetAndReason() {
        let controller = EngineController()
        controller.applyMembershipForTest(
            action: HC_APPLE_MEMBERSHIP_KICK,
            network: "Libera", channel: "#a", nick: "kicker",
            targetNick: "victim", reason: "spam",
            sessionID: 1, connectionID: 1, selfNick: "me")
        XCTAssertEqual(controller.messages.last?.kind,
                       .kick(target: "victim", reason: "spam"))
        XCTAssertEqual(controller.messages.last?.author?.nick, "kicker")
    }

    func testMembershipQuitArrivesPerSessionFromCoreFanout() {
        // Simulate what inbound_quit does: emit one MEMBERSHIP_CHANGE/QUIT per session
        // that contained the user. Assert the consumer simply records each one without
        // re-fanning out across the controller.
        let controller = EngineController()
        for channel in ["#a", "#b"] {
            controller.applyUserlistForTest(
                action: HC_APPLE_USERLIST_INSERT,
                network: "Libera", channel: channel, nick: "alice",
                modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
                sessionID: 0, connectionID: 1, selfNick: "me")
        }
        controller.applyMembershipForTest(
            action: HC_APPLE_MEMBERSHIP_QUIT,
            network: "Libera", channel: "#a", nick: "alice",
            reason: "bye", sessionID: 0, connectionID: 1, selfNick: "me")
        controller.applyMembershipForTest(
            action: HC_APPLE_MEMBERSHIP_QUIT,
            network: "Libera", channel: "#b", nick: "alice",
            reason: "bye", sessionID: 0, connectionID: 1, selfNick: "me")

        let connID = controller.connectionsByServerID[1]!
        let aSess = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
        let bSess = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!
        let quits = controller.messages.filter {
            if case .quit(let r) = $0.kind { return r == "bye" }
            return false
        }
        XCTAssertEqual(quits.count, 2, "one quit per session — no extra fan-out on consumer")
        XCTAssertEqual(Set(quits.map(\.sessionID)), Set([aSess, bSess]))
    }

    func testNickChangeAppendsTypedMessage() {
        let controller = EngineController()
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT,
            network: "Libera", channel: "#a", nick: "alice",
            modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
            sessionID: 1, connectionID: 1, selfNick: "me")
        let connID = controller.connectionsByServerID[1]!
        let userID = controller.usersByConnectionAndNick[UserKey(connectionID: connID, nick: "alice")]

        controller.applyNickChangeForTest(
            network: "Libera", channel: "#a",
            oldNick: "alice", newNick: "alice_",
            sessionID: 1, connectionID: 1, selfNick: "me")

        let last = controller.messages.last!
        XCTAssertEqual(last.kind, .nickChange(from: "alice", to: "alice_"))
        XCTAssertEqual(last.author?.userID, userID, "author resolved from old nick before any User update")
    }

    func testNickChangeArrivesPerSessionFromCoreFanout() {
        let controller = EngineController()
        for channel in ["#a", "#b"] {
            controller.applyUserlistForTest(
                action: HC_APPLE_USERLIST_INSERT,
                network: "Libera", channel: channel, nick: "alice",
                modePrefix: nil, account: nil, host: nil, isMe: false, isAway: false,
                sessionID: 0, connectionID: 1, selfNick: "me")
        }
        for channel in ["#a", "#b"] {
            controller.applyNickChangeForTest(
                network: "Libera", channel: channel,
                oldNick: "alice", newNick: "alice_",
                sessionID: 0, connectionID: 1, selfNick: "me")
        }
        let nickChanges = controller.messages.filter {
            if case .nickChange(let f, let t) = $0.kind { return f == "alice" && t == "alice_" }
            return false
        }
        XCTAssertEqual(nickChanges.count, 2)
    }

    func testModeChangeForwardsModesAndArgs() {
        let controller = EngineController()
        controller.applyModeChangeForTest(
            network: "Libera", channel: "#a", actor: "chanop",
            modes: "+o", args: "alice",
            sessionID: 1, connectionID: 1, selfNick: "me")
        XCTAssertEqual(controller.messages.last?.kind, .modeChange(modes: "+o", args: "alice"))
        XCTAssertEqual(controller.messages.last?.author?.nick, "chanop")

        controller.applyModeChangeForTest(
            network: "Libera", channel: "#a", actor: "chanop",
            modes: "+i", args: nil,
            sessionID: 1, connectionID: 1, selfNick: "me")
        XCTAssertEqual(controller.messages.last?.kind, .modeChange(modes: "+i", args: nil))
    }

    func testProducerTimestampOverridesDateForTypedMessages() {
        let controller = EngineController()
        controller.applyMembershipForTest(
            action: HC_APPLE_MEMBERSHIP_JOIN,
            network: "Libera", channel: "#a", nick: "alice",
            sessionID: 1, connectionID: 1, selfNick: "me",
            timestampSeconds: 1_700_000_000)
        let last = controller.messages.last!
        XCTAssertEqual(last.timestamp.timeIntervalSince1970, 1_700_000_000, accuracy: 0.001,
                       "producer-side time_t must round-trip into ChatMessage.timestamp")

        let before = Date()
        controller.applyMembershipForTest(
            action: HC_APPLE_MEMBERSHIP_JOIN,
            network: "Libera", channel: "#a", nick: "bob",
            sessionID: 1, connectionID: 1, selfNick: "me",
            timestampSeconds: 0)
        let after = Date()
        let synthetic = controller.messages.last!.timestamp
        XCTAssertGreaterThanOrEqual(synthetic, before)
        XCTAssertLessThanOrEqual(synthetic, after)
    }

    func testTestHelperNamedArgOrderMatchesDeclaredOrder() {
        let controller = EngineController()
        controller.applyMembershipForTest(
            action: HC_APPLE_MEMBERSHIP_KICK,
            network: "n", channel: "#c", nick: "kicker",
            targetNick: "victim", reason: "spam",
            sessionID: 1, connectionID: 1, selfNick: "me",
            timestampSeconds: 0)
        controller.applyNickChangeForTest(
            network: "n", channel: "#c",
            oldNick: "old", newNick: "new",
            sessionID: 1, connectionID: 1, selfNick: "me",
            timestampSeconds: 0)
        controller.applyModeChangeForTest(
            network: "n", channel: "#c", actor: "actor",
            modes: "+o", args: "alice",
            sessionID: 1, connectionID: 1, selfNick: "me",
            timestampSeconds: 0)
        XCTAssertGreaterThanOrEqual(controller.messages.count, 3)
    }

    // MARK: - Phase 6 — persistence (Task 1)

    func testServerEndpointConstruction() {
        let ep = ServerEndpoint(host: "irc.example.net", port: 6697, useTLS: true)
        XCTAssertEqual(ep.host, "irc.example.net")
        XCTAssertEqual(ep.port, 6697)
        XCTAssertTrue(ep.useTLS)
    }

    func testSASLConfigShape() {
        let cfg = SASLConfig(mechanism: "PLAIN", username: "alice", password: "hunter2")
        XCTAssertEqual(cfg.mechanism, "PLAIN")
        XCTAssertEqual(cfg.username, "alice")
        XCTAssertEqual(cfg.password, "hunter2")
    }

    func testNetworkBackCompatInitializer() {
        let net = Network(id: UUID(), displayName: "Example")
        XCTAssertTrue(net.servers.isEmpty)
        XCTAssertTrue(net.nicks.isEmpty)
        XCTAssertNil(net.sasl)
        XCTAssertFalse(net.autoConnect)
        XCTAssertTrue(net.autoJoin.isEmpty)
        XCTAssertTrue(net.onConnectCommands.isEmpty)
    }

    func testNetworkFullShape() {
        let net = Network(
            id: UUID(),
            displayName: "Example",
            servers: [ServerEndpoint(host: "irc.example.net", port: 6697, useTLS: true)],
            nicks: ["alice", "alice_"],
            sasl: SASLConfig(mechanism: "PLAIN", username: "alice", password: "pw"),
            autoConnect: true,
            autoJoin: ["#hexchat", "#dev"],
            onConnectCommands: ["/msg NickServ IDENTIFY pw"]
        )
        XCTAssertEqual(net.servers.count, 1)
        XCTAssertEqual(net.nicks, ["alice", "alice_"])
        XCTAssertEqual(net.sasl?.username, "alice")
        XCTAssertTrue(net.autoConnect)
        XCTAssertEqual(net.autoJoin, ["#hexchat", "#dev"])
        XCTAssertEqual(net.onConnectCommands.count, 1)
    }

    // MARK: - Phase 6 — persistence (Task 2)

    func testServerEndpointRoundTrip() throws {
        let ep = ServerEndpoint(host: "irc.example.net", port: 6697, useTLS: true)
        let data = try JSONEncoder().encode(ep)
        let back = try JSONDecoder().decode(ServerEndpoint.self, from: data)
        XCTAssertEqual(ep, back)
    }

    func testSASLConfigRoundTrip() throws {
        let cfg = SASLConfig(mechanism: "PLAIN", username: "alice", password: "pw")
        let data = try JSONEncoder().encode(cfg)
        let back = try JSONDecoder().decode(SASLConfig.self, from: data)
        XCTAssertEqual(cfg, back)
    }

    func testNetworkFullRoundTrip() throws {
        let net = Network(
            id: UUID(), displayName: "Example",
            servers: [
                ServerEndpoint(host: "a", port: 6667, useTLS: false),
                ServerEndpoint(host: "b", port: 6697, useTLS: true),
            ],
            nicks: ["alice"],
            sasl: SASLConfig(mechanism: "PLAIN", username: "alice", password: "pw"),
            autoConnect: true, autoJoin: ["#hexchat"],
            onConnectCommands: ["/msg NickServ IDENTIFY pw"]
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        let data = try encoder.encode(net)
        let back = try JSONDecoder().decode(Network.self, from: data)
        XCTAssertEqual(net, back)
    }

    func testNetworkJSONKeysAreStable() throws {
        let net = Network(
            id: UUID(uuidString: "11111111-1111-1111-1111-111111111111")!,
            displayName: "X")
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        let json = String(data: try encoder.encode(net), encoding: .utf8)!
        XCTAssertTrue(json.contains("\"autoConnect\""))
        XCTAssertTrue(json.contains("\"autoJoin\""))
        XCTAssertTrue(json.contains("\"displayName\""))
        XCTAssertTrue(json.contains("\"id\""))
        XCTAssertTrue(json.contains("\"nicks\""))
        XCTAssertTrue(json.contains("\"onConnectCommands\""))
        XCTAssertTrue(json.contains("\"servers\""))
        // Optional `sasl` is nil here; synthesised Codable should encodeIfPresent.
        XCTAssertFalse(json.contains("\"sasl\""))
    }

    // MARK: - Phase 6 — persistence (Task 3)

    func testConversationKeyCaseInsensitiveChannelEquality() {
        let net = UUID(uuidString: "22222222-2222-2222-2222-222222222222")!
        let a = ConversationKey(networkID: net, channel: "#HexChat")
        let b = ConversationKey(networkID: net, channel: "#hexchat")
        XCTAssertEqual(a, b)
        XCTAssertEqual(a.hashValue, b.hashValue)
    }

    func testConversationKeyRoundTrip() throws {
        let net = UUID(uuidString: "22222222-2222-2222-2222-222222222222")!
        let original = ConversationKey(networkID: net, channel: "#HexChat")
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        let data = try encoder.encode(original)
        let json = String(data: data, encoding: .utf8)!
        XCTAssertTrue(json.contains("\"channel\":\"#HexChat\""))
        XCTAssertTrue(json.contains("\"networkID\":\"22222222-2222-2222-2222-222222222222\""))
        let back = try JSONDecoder().decode(ConversationKey.self, from: data)
        XCTAssertEqual(original, back)
        XCTAssertEqual(back, ConversationKey(networkID: net, channel: "#hexchat"))
    }

    func testConversationKeyDictionaryLookupIsCaseInsensitive() {
        let net = UUID()
        var m: [ConversationKey: Int] = [:]
        m[ConversationKey(networkID: net, channel: "#HexChat")] = 42
        XCTAssertEqual(m[ConversationKey(networkID: net, channel: "#hexchat")], 42)
    }

    func testConversationKeyDistinguishesNetworks() {
        let netA = UUID(uuidString: "33333333-3333-3333-3333-333333333333")!
        let netB = UUID(uuidString: "44444444-4444-4444-4444-444444444444")!
        let a = ConversationKey(networkID: netA, channel: "#hexchat")
        let b = ConversationKey(networkID: netB, channel: "#hexchat")
        XCTAssertNotEqual(a, b)
        var m: [ConversationKey: Int] = [a: 1, b: 2]
        XCTAssertEqual(m[a], 1)
        XCTAssertEqual(m[b], 2)
        m[ConversationKey(networkID: netA, channel: "#HEXCHAT")] = 99
        XCTAssertEqual(m[a], 99)
        XCTAssertEqual(m[b], 2)
    }

    // MARK: - Phase 6 — persistence (Task 4)

    func testConversationStateDefaults() {
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let state = ConversationState(key: key)
        XCTAssertEqual(state.draft, "")
        XCTAssertEqual(state.unread, 0)
        XCTAssertNil(state.lastReadAt)
    }

    func testConversationStateRoundTrip() throws {
        let net = UUID(uuidString: "33333333-3333-3333-3333-333333333333")!
        let original = ConversationState(
            key: ConversationKey(networkID: net, channel: "#a"),
            draft: "typing…",
            unread: 3,
            lastReadAt: Date(timeIntervalSince1970: 1_700_000_000)
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(original)
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let back = try decoder.decode(ConversationState.self, from: data)
        XCTAssertEqual(original, back)
    }

    // MARK: - Phase 6 — persistence (Task 5)

    func testAppStateEmptyDefaults() {
        let state = AppState()
        XCTAssertEqual(state.schemaVersion, 1)
        XCTAssertTrue(state.networks.isEmpty)
        XCTAssertTrue(state.conversations.isEmpty)
        XCTAssertNil(state.selectedKey)
        XCTAssertTrue(state.commandHistory.isEmpty)
    }

    func testAppStateFullRoundTrip() throws {
        let net = Network(
            id: UUID(), displayName: "Example",
            servers: [ServerEndpoint(host: "irc.example.net", port: 6697, useTLS: true)],
            nicks: ["alice"], autoJoin: ["#hexchat"]
        )
        let keyA = ConversationKey(networkID: net.id, channel: "#a")
        let keyB = ConversationKey(networkID: net.id, channel: "#b")
        let original = AppState(
            networks: [net],
            conversations: [
                ConversationState(
                    key: keyA, draft: "hi", unread: 2,
                    lastReadAt: Date(timeIntervalSince1970: 1_700_000_000)),
                ConversationState(key: keyB),
            ],
            selectedKey: keyA,
            commandHistory: ["/join #a", "/msg alice hi"]
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(original)
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let back = try decoder.decode(AppState.self, from: data)
        XCTAssertEqual(original, back)
    }

    func testAppStateRejectsUnsupportedSchemaVersion() {
        let blob = #"""
            {"schemaVersion":2,"networks":[],"conversations":[],"commandHistory":[]}
            """#.data(using: .utf8)!
        XCTAssertThrowsError(try JSONDecoder().decode(AppState.self, from: blob)) { error in
            guard case AppStateDecodingError.unsupportedSchemaVersion(let v) = error else {
                return XCTFail("expected unsupportedSchemaVersion, got \(error)")
            }
            XCTAssertEqual(v, 2)
        }
    }

    func testAppStateJSONIsByteStable() throws {
        let net = Network(id: UUID(), displayName: "X")
        let state = AppState(
            networks: [net],
            conversations: [
                ConversationState(key: ConversationKey(networkID: net.id, channel: "#b")),
                ConversationState(key: ConversationKey(networkID: net.id, channel: "#a")),
            ]
        )
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        let a = try encoder.encode(state)
        let b = try encoder.encode(state)
        XCTAssertEqual(a, b)
    }
}
#else
@testable import HexChatAppleShell
import AppleAdapterBridge

// XCTest is unavailable in this toolchain; keep reducer coverage code compiled.
func _hexchatAppleShellTestsCompileProbe() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#probe")
    controller.applyUserlistForTest(
        action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#probe",
        nick: "alice", modePrefix: "@")
    _ = controller.visibleUsers
}
#endif
