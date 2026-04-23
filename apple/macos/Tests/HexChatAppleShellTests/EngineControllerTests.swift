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
        XCTAssertEqual(ChatMessageClassifier.classify(raw: "! failed"), .error)
        XCTAssertEqual(ChatMessageClassifier.classify(raw: "> /join #a"), .command)
        XCTAssertEqual(ChatMessageClassifier.classify(raw: "[READY] ready"), .lifecycle)
        XCTAssertEqual(ChatMessageClassifier.classify(raw: "-notice-"), .notice)
        XCTAssertEqual(ChatMessageClassifier.classify(raw: "nick has joined #a"), .join)
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
        controller.appendUnattributedForTest(raw: "! system error", kind: .error)
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
        controller.appendUnattributedForTest(raw: "! system error", kind: .error)
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
        controller.appendUnattributedForTest(raw: "! system error", kind: .error)

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
        controller.appendUnattributedForTest(raw: "! system error", kind: .error)
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
        controller.appendUnattributedForTest(raw: "! system error", kind: .error)
        XCTAssertFalse(controller.connections.isEmpty, "system connection must be created")
        XCTAssertTrue(
            controller.connectionsByServerID.isEmpty,
            "system connection has no server-id slot — server_id == 0 is reserved")
    }

    func testSystemSessionHasSystemConnectionAndNetwork() {
        let controller = EngineController()
        controller.appendUnattributedForTest(raw: "! system error", kind: .error)

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
        // Legacy storage still populated (dual-write).
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
