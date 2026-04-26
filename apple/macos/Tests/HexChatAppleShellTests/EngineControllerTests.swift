#if canImport(XCTest)
import XCTest

@testable import HexChatAppleShell
import AppleAdapterBridge

private final class CountingStore: PersistenceStore {
    private(set) var saveCount = 0
    private var cached: AppState?
    func load() throws -> AppState? { cached }
    func save(_ s: AppState) throws {
        cached = s
        saveCount += 1
    }
}

private final class BrokenStore: PersistenceStore {
    func load() throws -> AppState? { throw CocoaError(.fileReadCorruptFile) }
    func save(_ s: AppState) throws {}
}

@MainActor
final class EngineControllerTests: XCTestCase {
    func testUserlistInsertUpdateRemoveAndClear() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#hexchat",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let sessionID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#hexchat"))

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#hexchat",
            nick: "bob", connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#hexchat",
            nick: "alice", modePrefix: "@", connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#hexchat",
            nick: "bob", modePrefix: "+", connectionID: 1)

        XCTAssertEqual(controller.visibleUsers(for: sessionID).map(\.nick), ["alice", "bob"])
        XCTAssertEqual(controller.visibleUsers(for: sessionID).map(\.modePrefix), ["@", "+"])

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_REMOVE, network: "Libera", channel: "#hexchat",
            nick: "bob", connectionID: 1)
        XCTAssertEqual(controller.visibleUsers(for: sessionID).map(\.nick), ["alice"])

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_CLEAR, network: "Libera", channel: "#hexchat",
            nick: nil, connectionID: 1)
        XCTAssertTrue(controller.visibleUsers(for: sessionID).isEmpty)
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
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        let bID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#b"))
        XCTAssertEqual(controller.visibleUsers(for: aID).map(\.nick), ["alice"])
        XCTAssertEqual(controller.visibleUsers(for: bID).map(\.nick), ["bob"])
    }

    func testHistoryBrowseUpDownRestoresDraft() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#hexchat",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let sessionID = controller.sessionUUID(
            for: .composed(connectionID: conn, channel: "#hexchat"))
        controller.send("/join #hexchat", forSession: sessionID, trackHistory: true)
        controller.send("/msg alice hi", forSession: sessionID, trackHistory: true)
        controller.draftBinding(for: sessionID).wrappedValue = "/nick newname"

        controller.browseHistory(delta: -1, forSession: sessionID)
        XCTAssertEqual(controller.draftBinding(for: sessionID).wrappedValue, "/msg alice hi")

        controller.browseHistory(delta: -1, forSession: sessionID)
        XCTAssertEqual(controller.draftBinding(for: sessionID).wrappedValue, "/join #hexchat")

        controller.browseHistory(delta: 1, forSession: sessionID)
        XCTAssertEqual(controller.draftBinding(for: sessionID).wrappedValue, "/msg alice hi")

        controller.browseHistory(delta: 1, forSession: sessionID)
        XCTAssertEqual(controller.draftBinding(for: sessionID).wrappedValue, "/nick newname")
    }

    func testHistoryBrowseIsScopedPerSession() {
        // Two sessions browsing history independently must not clobber each other's
        // draft or cursor — this is the multi-window-safety contract that replaced
        // the old controller-global historyDraft/historyCursor.
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        let bID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#b"))!

        controller.send("/one", forSession: aID, trackHistory: true)
        controller.send("/two", forSession: aID, trackHistory: true)
        controller.draftBinding(for: aID).wrappedValue = "draft-a"
        controller.draftBinding(for: bID).wrappedValue = "draft-b"

        // Window A walks back into history; window B is untouched.
        controller.browseHistory(delta: -1, forSession: aID)
        XCTAssertEqual(controller.draftBinding(for: aID).wrappedValue, "/two")
        XCTAssertEqual(controller.draftBinding(for: bID).wrappedValue, "draft-b")

        // Window B walks back independently and lands on the same shared command,
        // but A's cursor stays where it was.
        controller.browseHistory(delta: -1, forSession: bID)
        XCTAssertEqual(controller.draftBinding(for: bID).wrappedValue, "/two")
        XCTAssertEqual(controller.draftBinding(for: aID).wrappedValue, "/two")

        // A returns to its own cached draft, B keeps its own cached draft.
        controller.browseHistory(delta: 1, forSession: aID)
        XCTAssertEqual(controller.draftBinding(for: aID).wrappedValue, "draft-a")
        XCTAssertEqual(controller.draftBinding(for: bID).wrappedValue, "/two")

        controller.browseHistory(delta: 1, forSession: bID)
        XCTAssertEqual(controller.draftBinding(for: bID).wrappedValue, "draft-b")
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

        controller.applyLogLineForTest(
            network: "Libera", channel: "#b", text: "message for b",
            connectionID: 1, selfNick: "me")

        let conn = controller.connectionsByServerID[1]!
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

        let serverID = controller.sessionUUID(for: .runtime(id: 1))
        let channelID = controller.sessionUUID(for: .runtime(id: 2))
        XCTAssertTrue(controller.visibleUsers(for: serverID).isEmpty)
        XCTAssertEqual(controller.visibleUsers(for: channelID).map(\.nick), ["bob", "alice"])
        XCTAssertEqual(controller.visibleUsers(for: channelID).map(\.modePrefix), ["@", nil])
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

    func testSessionRemoveDropsFocusRefcountAndLastFocused() {
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

        // A window focused on #a contributes to focusRefcount.
        let win = WindowSession(controller: controller, initial: aUUID)
        XCTAssertEqual(controller.focusRefcount[aUUID], 1)
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
        XCTAssertNil(controller.focusRefcount[aUUID], "focusRefcount entry must be cleared on REMOVE")
        XCTAssertNil(controller.lastFocusedSessionID, "lastFocusedSessionID must clear when its session is removed")
        XCTAssertEqual(controller.activeSessionID, bUUID, "active must reassign to a remaining session")
        XCTAssertNil(controller.usersBySession[aUUID], "usersBySession entry must be cleaned up")
        // Exactly one session should have isActive == true, and it should be #b.
        let actives = controller.sessions.filter { $0.isActive }
        XCTAssertEqual(actives.map(\.id), [bUUID])
        withExtendedLifetime(win) {}
    }

    func testNumericRuntimeSessionIDResolvesFromUUID() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#same",
            sessionID: 7, connectionID: 1, selfNick: "me")
        let uuid = controller.sessionUUID(for: .runtime(id: 7))!
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
        let connID = UUID()
        let a = ChatUser(
            connectionID: connID, nick: "Alice", modePrefix: "@", account: "alice_acct",
            host: "host.example", isMe: false, isAway: false)
        let b = ChatUser(
            connectionID: connID, nick: "alice", modePrefix: nil, account: nil, host: nil,
            isMe: true, isAway: true)
        XCTAssertEqual(a.id, b.id, "ChatUser identity must be case-insensitive on nick")
        XCTAssertNotEqual(a, b, "Equality is field-by-field; identity is nick alone")
    }

    func testChatUserDefaultsAreSafe() {
        let user = ChatUser(connectionID: UUID(), nick: "bob")
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
        let conn = controller.connectionsByServerID[1]!
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", modePrefix: "@", account: "alice_acct", isMe: false, isAway: false,
            connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "bob", modePrefix: "+", isMe: true, isAway: false, connectionID: 1)

        let users = controller.visibleUsers(for: aID)
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
        let conn = controller.connectionsByServerID[1]!
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", modePrefix: "@", isAway: false, connectionID: 1)
        XCTAssertEqual(controller.visibleUsers(for: aID).first?.isAway, false)

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
            nick: "alice", modePrefix: "@", isAway: true, connectionID: 1)
        XCTAssertEqual(controller.visibleUsers(for: aID).count, 1, "UPDATE must not duplicate the user")
        XCTAssertEqual(
            controller.visibleUsers(for: aID).first?.isAway, true,
            "UPDATE overwrites the prior record with fresh state")
    }

    func testUserlistUpdatePopulatesAccountAndHost() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", connectionID: 1)
        XCTAssertNil(controller.visibleUsers(for: aID).first?.account)
        XCTAssertNil(controller.visibleUsers(for: aID).first?.host)

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
            nick: "alice", account: "alice_acct", host: "alice.example", connectionID: 1)
        XCTAssertEqual(controller.visibleUsers(for: aID).first?.account, "alice_acct")
        XCTAssertEqual(controller.visibleUsers(for: aID).first?.host, "alice.example")
    }

    func testUserlistUpdateClearsAccountToNil() {
        // The crux of overwrite-vs-merge: a logout (account=nil from the C side)
        // must clear the previously-non-nil account. A merge would silently retain
        // the stale value.
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", account: "alice_acct", connectionID: 1)
        XCTAssertEqual(controller.visibleUsers(for: aID).first?.account, "alice_acct")

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
            nick: "alice", account: nil, connectionID: 1)
        XCTAssertNil(
            controller.visibleUsers(for: aID).first?.account, "logout / account-clear must overwrite, not merge")
    }

    func testUserlistInsertCarriesIsMe() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "me", isMe: true, connectionID: 1)
        XCTAssertTrue(controller.visibleUsers(for: aID).first?.isMe ?? false)
    }

    func testUserlistRemoveByNickIsCaseInsensitive() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "Alice", modePrefix: "@", connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_REMOVE, network: "Libera", channel: "#a",
            nick: "ALICE", connectionID: 1)
        XCTAssertTrue(controller.visibleUsers(for: aID).isEmpty)
    }

    func testUserlistEmptyNickIsIgnored() {
        // The C side should never emit an empty nick on INSERT/UPDATE/REMOVE, but
        // a defensive guard keeps a malformed event from corrupting the roster.
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "", connectionID: 1)
        XCTAssertTrue(controller.visibleUsers(for: aID).isEmpty)

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_REMOVE, network: "Libera", channel: "#a",
            nick: "", connectionID: 1)
        XCTAssertEqual(
            controller.visibleUsers(for: aID).map(\.nick), ["alice"], "empty REMOVE must not delete real users")
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
        let conn = controller.connectionsByServerID[1]!
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a",
            nick: "alice", isMe: false, connectionID: 1)
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
            nick: "alice", isMe: true, connectionID: 1)
        XCTAssertEqual(controller.visibleUsers(for: aID).count, 1)
        XCTAssertTrue(controller.visibleUsers(for: aID).first?.isMe ?? false)

        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#a",
            nick: "alice", isMe: false, connectionID: 1)
        XCTAssertFalse(controller.visibleUsers(for: aID).first?.isMe ?? true)
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

    func testLifecycleStoppedClearsRuntimeStateButPreservesNetworkIdentity() {
        // STOPPED clears runtime state (connections, sessions, locator/serverID
        // indices) but preserves persistable network identity. This is what
        // lets `pendingLastFocusedKey` resolve after reconnect — its networkID
        // must remain stable across the teardown. See the STOPPED comment in
        // the lifecycle handler.
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 1, selfNick: "me")
        XCTAssertFalse(controller.networks.isEmpty)
        XCTAssertFalse(controller.connections.isEmpty)
        XCTAssertFalse(controller.networksByName.isEmpty)
        XCTAssertFalse(controller.connectionsByServerID.isEmpty)

        controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)

        // Runtime state cleared.
        XCTAssertTrue(controller.connections.isEmpty)
        XCTAssertTrue(controller.connectionsByServerID.isEmpty)
        XCTAssertTrue(controller.sessionByLocator.isEmpty)
        XCTAssertTrue(controller.sessions.isEmpty, "all sessions cleared on STOPPED, including the system session")

        // Persistable identity preserved: networks survive so reconnect re-uses
        // the same UUIDs and `pendingLastFocusedKey` keeps resolving.
        XCTAssertFalse(controller.networks.isEmpty)
        XCTAssertFalse(controller.networksByName.isEmpty)
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
        XCTAssertNil(state.lastFocusedKey)
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
            lastFocusedKey: keyA,
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

    func testAppStateRejectsMissingSchemaVersion() {
        let blob = #"""
            {"networks":[],"conversations":[],"commandHistory":[]}
            """#.data(using: .utf8)!
        XCTAssertThrowsError(try JSONDecoder().decode(AppState.self, from: blob)) { error in
            guard case DecodingError.keyNotFound = error else {
                return XCTFail("expected keyNotFound, got \(error)")
            }
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

    // MARK: - Phase 6 — persistence (Task 6, in-memory store)

    func testInMemoryPersistenceStoreStartsEmpty() throws {
        let store = InMemoryPersistenceStore()
        XCTAssertNil(try store.load())
    }

    func testInMemoryPersistenceStoreRoundTrip() throws {
        let store = InMemoryPersistenceStore()
        let state = AppState(
            networks: [Network(id: UUID(), displayName: "X")],
            commandHistory: ["/a", "/b"]
        )
        try store.save(state)
        let back = try store.load()
        XCTAssertEqual(back, state)
    }

    // MARK: - Phase 6 — persistence (Task 7, EngineController wiring)

    func testEngineControllerLoadsPersistedStateAtInit() {
        let netID = UUID()
        let net = Network(id: netID, displayName: "Example")
        let keyA = ConversationKey(networkID: netID, channel: "#a")
        let seeded = AppState(
            networks: [net],
            conversations: [
                ConversationState(
                    key: keyA, draft: "halfway through a thought",
                    unread: 2, lastReadAt: nil)
            ],
            lastFocusedKey: keyA,
            commandHistory: ["/join #a"]
        )
        let store = InMemoryPersistenceStore(initial: seeded)
        let controller = EngineController(persistence: store, debounceInterval: .zero)
        XCTAssertEqual(controller.networks[netID]?.displayName, "Example")
        XCTAssertEqual(controller.commandHistory, ["/join #a"])
        XCTAssertEqual(controller.conversations[keyA]?.draft, "halfway through a thought")
    }

    func testEngineControllerWritesThroughOnMutation() async throws {
        let store = InMemoryPersistenceStore()
        let controller = EngineController(persistence: store, debounceInterval: .zero)
        let netID = UUID()
        controller.upsertNetworkForTest(id: netID, name: "X")
        await Task.yield()
        try? await Task.sleep(for: .milliseconds(20))
        XCTAssertEqual(try store.load()?.networks.first?.displayName, "X")
    }

    func testEngineControllerDebounceCollapsesBursts() async throws {
        let store = CountingStore()
        let controller = EngineController(persistence: store, debounceInterval: .milliseconds(50))
        for i in 0..<10 { controller.recordCommand("/cmd\(i)") }
        try? await Task.sleep(for: .milliseconds(150))
        XCTAssertEqual(store.saveCount, 1)
        XCTAssertEqual(try store.load()?.commandHistory.count, 10)
    }

    func testEngineControllerCorruptionTolerantInit() {
        let controller = EngineController(persistence: BrokenStore(), debounceInterval: .zero)
        XCTAssertTrue(controller.networks.isEmpty)
        XCTAssertTrue(controller.conversations.isEmpty)
    }

    func testCommandHistoryIsCapped() {
        let store = InMemoryPersistenceStore()
        let controller = EngineController(persistence: store, debounceInterval: .zero)
        for i in 0..<(EngineController.commandHistoryCap + 5) {
            controller.recordCommand("/cmd\(i)")
        }
        XCTAssertEqual(controller.commandHistory.count, EngineController.commandHistoryCap)
        XCTAssertEqual(
            controller.commandHistory.last, "/cmd\(EngineController.commandHistoryCap + 4)")
    }

    func testCommandHistoryCapPersistsThroughStore() async throws {
        // Regression guard for recursive didSet on the cap trim: the trim assignment
        // inside commandHistory.didSet must itself fire didSet again so markDirty()
        // is called and the trimmed array lands on disk.
        let store = InMemoryPersistenceStore()
        let controller = EngineController(persistence: store, debounceInterval: .zero)
        for i in 0..<(EngineController.commandHistoryCap + 5) {
            controller.recordCommand("/cmd\(i)")
        }
        await Task.yield()
        try? await Task.sleep(for: .milliseconds(20))
        XCTAssertEqual(
            try store.load()?.commandHistory.count, EngineController.commandHistoryCap)
        XCTAssertEqual(
            try store.load()?.commandHistory.last,
            "/cmd\(EngineController.commandHistoryCap + 4)")
    }

    func testFocusTransitionSchedulesSave() async throws {
        let store = CountingStore()
        let controller = EngineController(persistence: store, debounceInterval: .zero)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1, selfNick: "me")
        let conn = controller.connectionsByServerID[1]!
        let aID = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        // recordFocusTransition updates lastFocusedSessionID and calls markReadInternal,
        // which mutates conversations (didSet → markDirty → schedules save).
        controller.recordFocusTransition(from: nil, to: aID)
        await Task.yield()
        try? await Task.sleep(for: .milliseconds(20))
        XCTAssertGreaterThanOrEqual(store.saveCount, 1)
    }

    func testUpsertNetworkPreservesFirstSeenCasing() {
        let store = InMemoryPersistenceStore()
        let controller = EngineController(persistence: store, debounceInterval: .zero)
        _ = controller.upsertNetworkForName("AfterNET")
        _ = controller.upsertNetworkForName("afternet")
        XCTAssertEqual(controller.networks.count, 1)
        XCTAssertEqual(controller.networks.values.first?.displayName, "AfterNET")
    }

    // MARK: - Phase 6 — persistence (Task 8, per-conversation drafts)

    func testInputIsPerConversationDraft() {
        let store = InMemoryPersistenceStore()
        let controller = EngineController(persistence: store, debounceInterval: .zero)
        let netID = UUID()
        let connID = UUID()
        controller.networks[netID] = Network(id: netID, displayName: "Net")
        controller.connections[connID] = Connection(
            id: connID, networkID: netID, serverName: "Net", selfNick: nil)
        let locA = SessionLocator.composed(connectionID: connID, channel: "#a")
        let locB = SessionLocator.composed(connectionID: connID, channel: "#b")
        let sessA = ChatSession(connectionID: connID, channel: "#a", isActive: true, locator: locA)
        let sessB = ChatSession(connectionID: connID, channel: "#b", isActive: true, locator: locB)
        controller.sessions = [sessA, sessB]
        let keyA = ConversationKey(networkID: netID, channel: "#a")
        let keyB = ConversationKey(networkID: netID, channel: "#b")

        controller.draftBinding(for: sessA.id).wrappedValue = "typing in A"
        XCTAssertEqual(controller.conversations[keyA]?.draft, "typing in A")

        XCTAssertEqual(controller.draftBinding(for: sessB.id).wrappedValue, "")
        controller.draftBinding(for: sessB.id).wrappedValue = "typing in B"
        XCTAssertEqual(controller.conversations[keyB]?.draft, "typing in B")

        XCTAssertEqual(controller.draftBinding(for: sessA.id).wrappedValue, "typing in A")
    }

    func testInputWithNoSelectedSessionIsNoOp() {
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(), debounceInterval: .zero)
        XCTAssertEqual(controller.draftBinding(for: nil).wrappedValue, "")
        controller.draftBinding(for: nil).wrappedValue = "ignored"
        XCTAssertEqual(controller.draftBinding(for: nil).wrappedValue, "")
        XCTAssertTrue(controller.conversations.isEmpty)
    }

    // MARK: - Phase 6 — persistence (Task 9, unread bookkeeping)

    func testIncomingMessageIncrementsUnreadWhenNotFocused() {
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(), debounceInterval: .zero)
        let netID = UUID()
        let connID = UUID()
        controller.networks[netID] = Network(id: netID, displayName: "Net")
        controller.connections[connID] = Connection(
            id: connID, networkID: netID, serverName: "Net", selfNick: nil)
        let locA = SessionLocator.composed(connectionID: connID, channel: "#a")
        let sessA = ChatSession(connectionID: connID, channel: "#a", isActive: true, locator: locA)
        controller.sessions = [sessA]
        // No window has focusRefcount > 0 for sessA, so incoming messages increment unread.

        controller.appendMessageForTest(
            ChatMessage(sessionID: sessA.id, raw: "hi", kind: .message(body: "hi")))

        let keyA = ConversationKey(networkID: netID, channel: "#a")
        XCTAssertEqual(controller.conversations[keyA]?.unread, 1)
    }

    func testFocusingSessionClearsUnreadAndSetsLastRead() {
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(), debounceInterval: .zero)
        let netID = UUID()
        let connID = UUID()
        controller.networks[netID] = Network(id: netID, displayName: "Net")
        controller.connections[connID] = Connection(
            id: connID, networkID: netID, serverName: "Net", selfNick: nil)
        let locA = SessionLocator.composed(connectionID: connID, channel: "#a")
        let sessA = ChatSession(connectionID: connID, channel: "#a", isActive: true, locator: locA)
        controller.sessions = [sessA]
        let keyA = ConversationKey(networkID: netID, channel: "#a")
        controller.conversations[keyA] = ConversationState(key: keyA, unread: 3)

        let before = Date()
        controller.recordFocusTransition(from: nil, to: sessA.id)
        let after = Date()

        XCTAssertEqual(controller.conversations[keyA]?.unread, 0)
        let lastRead = controller.conversations[keyA]?.lastReadAt
        XCTAssertNotNil(lastRead)
        XCTAssertGreaterThanOrEqual(
            lastRead!.timeIntervalSince1970, before.timeIntervalSince1970 - 0.1)
        XCTAssertLessThanOrEqual(
            lastRead!.timeIntervalSince1970, after.timeIntervalSince1970 + 0.1)
    }

    func testNickChangeEventIncrementsUnreadWhenNotFocused() {
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(), debounceInterval: .zero)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#b",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        // No window is focused on #a, so nick changes there increment unread.

        controller.applyNickChangeForTest(
            network: "Libera", channel: "#a",
            oldNick: "alice", newNick: "alice_", connectionID: 1)

        let keyA = ConversationKey(networkID: netID, channel: "#a")
        XCTAssertEqual(controller.conversations[keyA]?.unread, 1)
    }

    func testSystemSessionMessagesDoNotIncrementUnread() {
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(), debounceInterval: .zero)
        // Force the system session into existence via an unattributed append. The
        // recordActivity guard explicitly skips the system session regardless of focus,
        // so system-session messages must never bump conversation unread counts.
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1)

        controller.appendUnattributedForTest(
            raw: "console line", kind: .notice(body: "console line"))

        XCTAssertTrue(
            controller.conversations.allSatisfy { $0.value.unread == 0 },
            "system-session activity must not bump any conversation's unread count")
    }

    func testFinalFlushOnLifecycleStopped() async throws {
        let store = CountingStore()
        let controller = EngineController(persistence: store, debounceInterval: .seconds(60))
        controller.recordCommand("/late")
        XCTAssertEqual(store.saveCount, 0)  // 60s debounce hasn't fired
        controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)
        XCTAssertEqual(store.saveCount, 1)
        XCTAssertEqual(try store.load()?.commandHistory, ["/late"])

        // Quiescence: the post-flush teardown clears non-persisted fields (networks,
        // connections, sessions, indexes) — none of which have a markDirty didSet —
        // so no second save should fire afterward. Regression guard for the day
        // someone adds a didSet to one of those fields without realising.
        await Task.yield()
        try? await Task.sleep(for: .milliseconds(50))
        XCTAssertEqual(store.saveCount, 1)
    }

    func testIncomingMessageForFocusedSessionDoesNotIncrementUnread() {
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(), debounceInterval: .zero)
        let netID = UUID()
        let connID = UUID()
        controller.networks[netID] = Network(id: netID, displayName: "Net")
        controller.connections[connID] = Connection(
            id: connID, networkID: netID, serverName: "Net", selfNick: nil)
        let locA = SessionLocator.composed(connectionID: connID, channel: "#a")
        let sessA = ChatSession(connectionID: connID, channel: "#a", isActive: true, locator: locA)
        controller.sessions = [sessA]
        // Register a window focused on sessA — focusRefcount suppresses unread.
        controller.recordFocusTransition(from: nil, to: sessA.id)

        controller.appendMessageForTest(
            ChatMessage(sessionID: sessA.id, raw: "hi", kind: .message(body: "hi")))

        let keyA = ConversationKey(networkID: netID, channel: "#a")
        XCTAssertEqual(controller.conversations[keyA]?.unread ?? 0, 0)
    }

    // MARK: - Phase 7 — message persistence (Task 1, Codable on ChatMessage)

    private func roundTripChatMessageKind(_ kind: ChatMessageKind) throws {
        let original = ChatMessage(
            sessionID: UUID(uuidString: "55555555-5555-5555-5555-555555555555")!,
            raw: "raw text", kind: kind,
            author: MessageAuthor(nick: "alice", userID: nil),
            timestamp: Date(timeIntervalSince1970: 1_700_000_000))
        let enc = JSONEncoder()
        enc.outputFormatting = [.sortedKeys]
        enc.dateEncodingStrategy = .iso8601
        let data = try enc.encode(original)
        let dec = JSONDecoder()
        dec.dateDecodingStrategy = .iso8601
        let back = try dec.decode(ChatMessage.self, from: data)
        XCTAssertEqual(back.id, original.id, "id must round-trip")
        XCTAssertEqual(back.sessionID, original.sessionID)
        XCTAssertEqual(back.raw, original.raw)
        XCTAssertEqual(back.kind, original.kind, "kind round-trip failed for \(kind)")
        XCTAssertEqual(back.author?.nick, original.author?.nick)
        XCTAssertEqual(
            back.timestamp.timeIntervalSince1970,
            original.timestamp.timeIntervalSince1970, accuracy: 0.001)
    }

    func testChatMessageMessageKindRoundTrip() throws {
        try roundTripChatMessageKind(.message(body: "hello"))
    }
    func testChatMessageNoticeKindRoundTrip() throws {
        try roundTripChatMessageKind(.notice(body: "fyi"))
    }
    func testChatMessageActionKindRoundTrip() throws {
        try roundTripChatMessageKind(.action(body: "waves"))
    }
    func testChatMessageCommandKindRoundTrip() throws {
        try roundTripChatMessageKind(.command(body: "/join #a"))
    }
    func testChatMessageErrorKindRoundTrip() throws {
        try roundTripChatMessageKind(.error(body: "boom"))
    }
    func testChatMessageLifecycleKindRoundTrip() throws {
        try roundTripChatMessageKind(.lifecycle(phase: "READY", body: "ready"))
    }
    func testChatMessageJoinKindRoundTrip() throws {
        try roundTripChatMessageKind(.join)
    }
    func testChatMessagePartKindRoundTrip() throws {
        try roundTripChatMessageKind(.part(reason: "bye"))
        try roundTripChatMessageKind(.part(reason: nil))
    }
    func testChatMessageQuitKindRoundTrip() throws {
        try roundTripChatMessageKind(.quit(reason: "ping timeout"))
        try roundTripChatMessageKind(.quit(reason: nil))
    }
    func testChatMessageKickKindRoundTrip() throws {
        try roundTripChatMessageKind(.kick(target: "bob", reason: "spam"))
        try roundTripChatMessageKind(.kick(target: "bob", reason: nil))
    }
    func testChatMessageNickChangeKindRoundTrip() throws {
        try roundTripChatMessageKind(.nickChange(from: "alice", to: "alice_"))
    }
    func testChatMessageModeChangeKindRoundTrip() throws {
        try roundTripChatMessageKind(.modeChange(modes: "+o", args: "alice"))
        try roundTripChatMessageKind(.modeChange(modes: "+i", args: nil))
    }

    func testChatMessageOmitsNilAuthor() throws {
        let m = ChatMessage(
            sessionID: UUID(), raw: "system line", kind: .lifecycle(phase: "X", body: "x"))
        let enc = JSONEncoder()
        enc.outputFormatting = [.sortedKeys]
        let data = try enc.encode(m)
        let json = String(data: data, encoding: .utf8)!
        XCTAssertFalse(json.contains("\"author\""))
    }

    // MARK: - Phase 7 — message persistence (Task 2, MessageStore in-memory)

    private func makeMessage(
        in key: ConversationKey, body: String,
        timestamp: Date = Date(timeIntervalSince1970: 1_700_000_000)
    ) -> ChatMessage {
        ChatMessage(
            sessionID: UUID(),
            raw: body,
            kind: .message(body: body),
            author: MessageAuthor(nick: "alice", userID: nil),
            timestamp: timestamp)
    }

    func testInMemoryMessageStoreRoundTripsAppendAndPage() throws {
        let store = InMemoryMessageStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let m = makeMessage(in: key, body: "hello")
        try store.append(m, conversation: key)
        let page = try store.page(conversation: key, before: nil, limit: 10)
        XCTAssertEqual(page.count, 1)
        XCTAssertEqual(page[0].id, m.id)
        XCTAssertEqual(try store.count(conversation: key), 1)
    }

    func testInMemoryMessageStoreOrdersAscending() throws {
        let store = InMemoryMessageStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let t = Date(timeIntervalSince1970: 1_700_000_000)
        try store.append(makeMessage(in: key, body: "a", timestamp: t), conversation: key)
        try store.append(
            makeMessage(in: key, body: "b", timestamp: t.addingTimeInterval(1)),
            conversation: key)
        try store.append(
            makeMessage(in: key, body: "c", timestamp: t.addingTimeInterval(2)),
            conversation: key)
        let page = try store.page(conversation: key, before: nil, limit: 10)
        XCTAssertEqual(page.map(\.body), ["a", "b", "c"])
    }

    func testInMemoryMessageStorePageBeforeExcludesBoundary() throws {
        let store = InMemoryMessageStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let t = Date(timeIntervalSince1970: 1_700_000_000)
        for i in 0..<5 {
            try store.append(
                makeMessage(in: key, body: "m\(i)", timestamp: t.addingTimeInterval(Double(i))),
                conversation: key)
        }
        let page = try store.page(
            conversation: key, before: t.addingTimeInterval(2), limit: 10)
        XCTAssertEqual(page.map(\.body), ["m0", "m1"])
    }

    func testInMemoryMessageStorePageHonoursLimit() throws {
        let store = InMemoryMessageStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let t = Date(timeIntervalSince1970: 1_700_000_000)
        for i in 0..<10 {
            try store.append(
                makeMessage(in: key, body: "m\(i)", timestamp: t.addingTimeInterval(Double(i))),
                conversation: key)
        }
        let page = try store.page(conversation: key, before: nil, limit: 3)
        XCTAssertEqual(page.map(\.body), ["m7", "m8", "m9"])
    }

    func testInMemoryMessageStoreDuplicateIDIsNoOp() throws {
        let store = InMemoryMessageStore()
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let m = makeMessage(in: key, body: "hi")
        try store.append(m, conversation: key)
        try store.append(m, conversation: key)  // same id
        XCTAssertEqual(try store.count(conversation: key), 1)
    }

    func testInMemoryMessageStoreIsolatesConversations() throws {
        let store = InMemoryMessageStore()
        let netID = UUID()
        let keyA = ConversationKey(networkID: netID, channel: "#a")
        let keyB = ConversationKey(networkID: netID, channel: "#b")
        try store.append(makeMessage(in: keyA, body: "a"), conversation: keyA)
        try store.append(makeMessage(in: keyB, body: "b"), conversation: keyB)
        XCTAssertEqual(try store.count(conversation: keyA), 1)
        XCTAssertEqual(try store.count(conversation: keyB), 1)
        XCTAssertEqual(try store.page(conversation: keyA, before: nil, limit: 10).map(\.body), ["a"])
    }

    func testInMemoryMessageStoreCaseInsensitiveChannel() throws {
        let store = InMemoryMessageStore()
        let netID = UUID()
        let keyUpper = ConversationKey(networkID: netID, channel: "#HEX")
        let keyLower = ConversationKey(networkID: netID, channel: "#hex")
        try store.append(makeMessage(in: keyUpper, body: "x"), conversation: keyUpper)
        XCTAssertEqual(try store.count(conversation: keyLower), 1)
    }

    // MARK: - Phase 7 — message persistence (Task 6, controller wiring)

    func testEngineControllerWritesMessagesThroughToStore() async throws {
        let store = InMemoryMessageStore()
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: store,
            debounceInterval: .zero)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let sessA = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!

        controller.appendMessageForTest(
            ChatMessage(sessionID: sessA, raw: "hi", kind: .message(body: "hi")))

        // Write-through is fire-and-forget on a background queue; give it a beat.
        try? await Task.sleep(for: .milliseconds(100))
        let key = ConversationKey(networkID: netID, channel: "#a")
        XCTAssertEqual(try store.count(conversation: key), 1)
    }

    // MARK: - Phase 7 — message persistence (Task 7, ring + caps)

    func testMessageRingTracksPerConversation() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#b",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let sessA = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        let sessB = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#b"))!

        for i in 0..<3 {
            controller.appendMessageForTest(
                ChatMessage(sessionID: sessA, raw: "a\(i)", kind: .message(body: "a\(i)")))
        }
        controller.appendMessageForTest(
            ChatMessage(sessionID: sessB, raw: "b0", kind: .message(body: "b0")))

        let keyA = ConversationKey(networkID: netID, channel: "#a")
        let keyB = ConversationKey(networkID: netID, channel: "#b")
        XCTAssertEqual(controller.messageRingForTest(conversation: keyA).count, 3)
        XCTAssertEqual(controller.messageRingForTest(conversation: keyB).count, 1)
    }

    func testMessageRingHonoursPerConversationCap() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let sess = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!

        let cap = EngineController.messageRingPerConversation
        for i in 0..<(cap + 50) {
            controller.appendMessageForTest(
                ChatMessage(sessionID: sess, raw: "m\(i)", kind: .message(body: "m\(i)")))
        }
        let key = ConversationKey(networkID: netID, channel: "#a")
        XCTAssertEqual(controller.messageRingForTest(conversation: key).count, cap)
        XCTAssertEqual(
            controller.messageRingForTest(conversation: key).first?.body, "m50",
            "ring should drop oldest entries first")
    }

    func testGlobalMessagesArrayHonoursGlobalCap() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let sess = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!

        let cap = EngineController.messagesGlobalCap
        for i in 0..<(cap + 25) {
            controller.appendMessageForTest(
                ChatMessage(sessionID: sess, raw: "m\(i)", kind: .message(body: "m\(i)")))
        }
        XCTAssertEqual(controller.messages.count, cap)
        XCTAssertEqual(controller.messages.last?.body, "m\(cap + 24)")
    }

    // MARK: - Phase 7 — message persistence (Task 8, loadOlder)

    func testLoadOlderPrependsFromStore() throws {
        let store = InMemoryMessageStore()
        let netID = UUID()
        let key = ConversationKey(networkID: netID, channel: "#a")
        // Pre-seed 50 messages spanning timestamps t..t+49.
        let t = Date(timeIntervalSince1970: 1_700_000_000)
        for i in 0..<50 {
            let m = ChatMessage(
                sessionID: UUID(), raw: "m\(i)", kind: .message(body: "m\(i)"),
                timestamp: t.addingTimeInterval(Double(i)))
            try store.append(m, conversation: key)
        }
        // Construct controller; it doesn't load from the store on init (Phase 7 leaves
        // the cold-start cache empty by design — fast-path is incoming messages, not
        // disk reads).
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: store, debounceInterval: .zero)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1, selfNick: "me")
        // After SESSION_ACTIVATE we have a network — but it's a fresh UUID, not netID
        // above. Re-key the seeded data to match the runtime network so lookups land.
        let runtimeNet = controller.connectionsByServerID[1].flatMap {
            controller.connections[$0]?.networkID
        }!
        let runtimeKey = ConversationKey(networkID: runtimeNet, channel: "#a")
        for i in 0..<50 {
            let m = ChatMessage(
                sessionID: UUID(), raw: "m\(i)", kind: .message(body: "m\(i)"),
                timestamp: t.addingTimeInterval(Double(i)))
            try store.append(m, conversation: runtimeKey)
        }

        // Append one current-session message (sets ring oldest to t+100).
        let conn = controller.connectionsByServerID[1]!
        let sess = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        controller.appendMessageForTest(
            ChatMessage(
                sessionID: sess, raw: "current", kind: .message(body: "current"),
                timestamp: t.addingTimeInterval(100)))
        XCTAssertEqual(controller.messageRingForTest(conversation: runtimeKey).count, 1)

        let result = try controller.loadOlder(forConversation: runtimeKey, limit: 30)
        XCTAssertEqual(result.localCount, 30)
        XCTAssertEqual(controller.messageRingForTest(conversation: runtimeKey).count, 31)
        // Prepended in ascending timestamp order; "current" stays last.
        XCTAssertEqual(
            controller.messageRingForTest(conversation: runtimeKey).last?.body, "current")
    }

    func testLoadOlderReturnsZeroWhenStoreEmpty() throws {
        let controller = EngineController()
        let key = ConversationKey(networkID: UUID(), channel: "#empty")
        let result = try controller.loadOlder(forConversation: key, limit: 50)
        XCTAssertEqual(result.localCount, 0)
        XCTAssertFalse(result.requestedRemote)
    }

    // MARK: - Phase 7.5 task-3: cap-gated bridge dispatch (stub bridge)

    final class RecordingChathistoryBridge: ChathistoryBridge, @unchecked Sendable {
        struct Call: Equatable {
            let connectionID: UInt64
            let channel: String
            let beforeMsec: Int64
            let limit: Int
        }
        private let lock = NSLock()
        private var calls: [Call] = []

        func requestBefore(
            connectionID: UInt64, channel: String, beforeMsec: Int64, limit: Int
        ) {
            lock.lock()
            calls.append(
                Call(
                    connectionID: connectionID, channel: channel, beforeMsec: beforeMsec,
                    limit: limit))
            lock.unlock()
        }

        var records: [Call] {
            lock.lock()
            defer { lock.unlock() }
            return calls
        }
    }

    func testLoadOlderRequestsBridgeWhenLocalShort() throws {
        let store = InMemoryMessageStore()
        let bridge = RecordingChathistoryBridge()
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: store, debounceInterval: .zero, chathistory: bridge)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1, connectionHaveChathistory: true)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let key = ConversationKey(networkID: netID, channel: "#a")
        let t = Date(timeIntervalSince1970: 1_700_000_000)
        // 5 local rows in the store; loadOlder asks for 50.
        for i in 0..<5 {
            let m = ChatMessage(
                sessionID: UUID(), raw: "m\(i)", kind: .message(body: "m\(i)"),
                timestamp: t.addingTimeInterval(Double(i)))
            try store.append(m, conversation: key)
        }
        let result = try controller.loadOlder(forConversation: key, limit: 50)
        XCTAssertEqual(result.localCount, 5)
        XCTAssertTrue(result.requestedRemote)
        XCTAssertEqual(bridge.records.count, 1)
        XCTAssertEqual(bridge.records.first?.connectionID, 1)
        XCTAssertEqual(bridge.records.first?.channel, "#a")
        XCTAssertEqual(bridge.records.first?.limit, 45)
    }

    func testLoadOlderUsesPostFetchAnchor() throws {
        // Anchor passed to the bridge must be the OLDEST local row's timestamp
        // after the prepend, not the pre-fetch ring oldest, otherwise we
        // re-request rows we just prepended.
        let store = InMemoryMessageStore()
        let bridge = RecordingChathistoryBridge()
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: store, debounceInterval: .zero, chathistory: bridge)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1, connectionHaveChathistory: true)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let key = ConversationKey(networkID: netID, channel: "#a")
        let t = Date(timeIntervalSince1970: 1_700_000_000)

        // Pre-fetch ring oldest is t+100 (one message); the local store has 30
        // older rows at t+50..t+79. loadOlder(limit: 100) should prepend all
        // 30 rows then ask the bridge for "before t+50" (oldest of new ring),
        // limit 70. Critically: NOT "before t+100" (pre-fetch anchor).
        let sess = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        controller.appendMessageForTest(
            ChatMessage(
                sessionID: sess, raw: "current", kind: .message(body: "current"),
                timestamp: t.addingTimeInterval(100)))
        for i in 0..<30 {
            try store.append(
                ChatMessage(
                    sessionID: UUID(), raw: "older\(i)", kind: .message(body: "older\(i)"),
                    timestamp: t.addingTimeInterval(Double(50 + i))),
                conversation: key)
        }

        let result = try controller.loadOlder(forConversation: key, limit: 100)
        XCTAssertEqual(result.localCount, 30)
        XCTAssertTrue(result.requestedRemote)
        // Anchor must be t+50 (the oldest of the just-prepended page), not
        // t+100 (the pre-fetch ring oldest).
        let preFetchMs = Int64(t.addingTimeInterval(100).timeIntervalSince1970 * 1000)
        let postFetchMs = Int64(t.addingTimeInterval(50).timeIntervalSince1970 * 1000)
        XCTAssertEqual(bridge.records.first?.beforeMsec, postFetchMs)
        XCTAssertLessThan(bridge.records.first?.beforeMsec ?? .max, preFetchMs)
        XCTAssertEqual(bridge.records.first?.limit, 70)
    }

    func testLoadOlderSkipsBridgeWhenCapMissing() throws {
        let store = InMemoryMessageStore()
        let bridge = RecordingChathistoryBridge()
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: store, debounceInterval: .zero, chathistory: bridge)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1, connectionHaveChathistory: false)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let key = ConversationKey(networkID: netID, channel: "#a")
        let result = try controller.loadOlder(forConversation: key, limit: 50)
        XCTAssertEqual(result.localCount, 0)
        XCTAssertFalse(result.requestedRemote)
        XCTAssertTrue(bridge.records.isEmpty)
    }

    func testLoadOlderSkipsBridgeWhenLocalSatisfies() throws {
        let store = InMemoryMessageStore()
        let bridge = RecordingChathistoryBridge()
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: store, debounceInterval: .zero, chathistory: bridge)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1, connectionHaveChathistory: true)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let key = ConversationKey(networkID: netID, channel: "#a")
        let t = Date(timeIntervalSince1970: 1_700_000_000)
        for i in 0..<50 {
            try store.append(
                ChatMessage(
                    sessionID: UUID(), raw: "m\(i)", kind: .message(body: "m\(i)"),
                    timestamp: t.addingTimeInterval(Double(i))),
                conversation: key)
        }
        let result = try controller.loadOlder(forConversation: key, limit: 30)
        XCTAssertEqual(result.localCount, 30)
        XCTAssertFalse(result.requestedRemote)
        XCTAssertTrue(bridge.records.isEmpty)
    }

    func testLoadOlderSkipsBridgeForUnknownConnection() throws {
        let bridge = RecordingChathistoryBridge()
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: InMemoryMessageStore(), debounceInterval: .zero,
            chathistory: bridge)
        // No applySessionForTest — no live Connection for this networkID.
        let key = ConversationKey(networkID: UUID(), channel: "#a")
        let result = try controller.loadOlder(forConversation: key, limit: 50)
        XCTAssertFalse(result.requestedRemote)
        XCTAssertTrue(bridge.records.isEmpty)
    }

    func testLoadOlderRequestedRemoteFlagWithEmptyLocal() throws {
        let store = InMemoryMessageStore()
        let bridge = RecordingChathistoryBridge()
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: store, debounceInterval: .zero, chathistory: bridge)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1, connectionHaveChathistory: true)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let key = ConversationKey(networkID: netID, channel: "#a")
        let result = try controller.loadOlder(forConversation: key, limit: 50)
        XCTAssertEqual(result.localCount, 0)
        XCTAssertTrue(result.requestedRemote)
        XCTAssertEqual(bridge.records.count, 1)
        XCTAssertEqual(bridge.records.first?.limit, 50)
    }

    // MARK: - Phase 7.5 task-5: live CRuntimeChathistoryBridge smoke

    func testCRuntimeBridgeCallsCFunctionWithoutCrash() {
        // Smoke-level only — substantive behavior is covered by:
        //   * fe-apple-chathistory-bridge meson tests (formatter + dispatch lifecycle)
        //   * RecordingChathistoryBridge tests above (loadOlder gate matrix)
        // Here we only verify the production bridge can be constructed and
        // called from Swift without crashing. Connection 0 is unmapped on the
        // C side; the dispatched callback walks serv_list, finds nothing,
        // drops silently.
        let bridge = CRuntimeChathistoryBridge()
        bridge.requestBefore(
            connectionID: 0, channel: "#nope",
            beforeMsec: 1_700_000_000_000, limit: 50)
    }

    func testEngineControllerToleratesBrokenMessageStore() {
        struct BrokenMessageStore: MessageStore {
            func append(_ m: ChatMessage, conversation: ConversationKey) throws -> Bool {
                throw CocoaError(.fileWriteUnknown)
            }
            func page(conversation: ConversationKey, before: Date?, limit: Int) throws
                -> [ChatMessage]
            { throw CocoaError(.fileReadCorruptFile) }
            func count(conversation: ConversationKey) throws -> Int {
                throw CocoaError(.fileReadCorruptFile)
            }
        }
        // Construct succeeds; failed writes log and don't crash.
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: BrokenMessageStore(),
            debounceInterval: .zero)
        XCTAssertTrue(controller.messages.isEmpty)
        // Trigger a write-through; should not throw or crash.
        controller.appendMessageForTest(
            ChatMessage(sessionID: UUID(), raw: "x", kind: .message(body: "x")))
    }

    // MARK: - Phase 7.5 task-1b: sync write-through + ring insertion-sort

    func testRingInsertionSortHandlesOutOfOrderAppend() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let sess = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        let t = Date(timeIntervalSince1970: 1_700_000_000)
        controller.appendMessageForTest(
            ChatMessage(
                sessionID: sess, raw: "a", kind: .message(body: "a"),
                timestamp: t.addingTimeInterval(10)))
        controller.appendMessageForTest(
            ChatMessage(
                sessionID: sess, raw: "b", kind: .message(body: "b"),
                timestamp: t.addingTimeInterval(20)))
        // Out-of-order replay: timestamp earlier than both above.
        controller.appendMessageForTest(
            ChatMessage(
                sessionID: sess, raw: "c", kind: .message(body: "c"),
                timestamp: t.addingTimeInterval(5)))
        let key = ConversationKey(networkID: netID, channel: "#a")
        let ring = controller.messageRingForTest(conversation: key)
        XCTAssertEqual(ring.map { $0.body }, ["c", "a", "b"])
    }

    func testRingTrimDropsOldestNotNewest() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let sess = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        let cap = EngineController.messageRingPerConversation
        let t = Date(timeIntervalSince1970: 1_700_000_000)
        for i in 0..<cap {
            controller.appendMessageForTest(
                ChatMessage(
                    sessionID: sess, raw: "m\(i)", kind: .message(body: "m\(i)"),
                    timestamp: t.addingTimeInterval(Double(i))))
        }
        controller.appendMessageForTest(
            ChatMessage(
                sessionID: sess, raw: "newest", kind: .message(body: "newest"),
                timestamp: t.addingTimeInterval(Double(cap))))
        let key = ConversationKey(networkID: netID, channel: "#a")
        let ring = controller.messageRingForTest(conversation: key)
        XCTAssertEqual(ring.count, cap)
        XCTAssertEqual(ring.first?.body, "m1", "oldest (m0) should have been trimmed")
        XCTAssertEqual(ring.last?.body, "newest", "newest survives")
    }

    func testReplayDuplicateNeverReachesRing() throws {
        let store = InMemoryMessageStore()
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: store, debounceInterval: .zero)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let sess = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        let key = ConversationKey(networkID: netID, channel: "#a")
        let t = Date(timeIntervalSince1970: 1_700_000_000)

        controller.appendMessageForTest(
            ChatMessage(
                sessionID: sess, raw: "x", kind: .message(body: "x"),
                timestamp: t, serverMsgID: "abc"))
        XCTAssertEqual(controller.messageRingForTest(conversation: key).count, 1)
        XCTAssertEqual(try store.count(conversation: key), 1)

        // Logical replay: distinct ChatMessage.id but same (key, msgid, timestamp).
        // The store rejects (false), and Task 1b's invariant says the ring must
        // not mutate when the store rejects.
        controller.appendMessageForTest(
            ChatMessage(
                sessionID: sess, raw: "x", kind: .message(body: "x"),
                timestamp: t, serverMsgID: "abc"))
        XCTAssertEqual(
            controller.messageRingForTest(conversation: key).count, 1,
            "ring must not gain a duplicate row")
        XCTAssertEqual(try store.count(conversation: key), 1)
    }

    func testEmptyServerMsgIDDoesNotDedup() {
        let store = InMemoryMessageStore()
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: store, debounceInterval: .zero)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let sess = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        let key = ConversationKey(networkID: netID, channel: "#a")
        let t = Date(timeIntervalSince1970: 1_700_000_000)

        // Empty string serverMsgID: untagged content, two appends both insert.
        controller.appendMessageForTest(
            ChatMessage(
                sessionID: sess, raw: "x", kind: .message(body: "x"),
                timestamp: t, serverMsgID: ""))
        controller.appendMessageForTest(
            ChatMessage(
                sessionID: sess, raw: "y", kind: .message(body: "y"),
                timestamp: t, serverMsgID: ""))
        XCTAssertEqual(controller.messageRingForTest(conversation: key).count, 2)
        XCTAssertEqual(try store.count(conversation: key), 2)
    }

    // MARK: - Phase 7.5 task-2: thread server_msgid + have_chathistory

    func testLogLineEventCarriesServerMsgID() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let key = ConversationKey(networkID: netID, channel: "#a")
        controller.applyLogLineForTest(
            network: "Net", channel: "#a", text: "hi from msgid abc",
            sessionID: 0, connectionID: 1, selfNick: "me",
            serverMsgID: "abc")
        let ring = controller.messageRingForTest(conversation: key)
        XCTAssertEqual(ring.count, 1)
        XCTAssertEqual(ring.last?.serverMsgID, "abc")
    }

    func testLogLineEventDropsPendingMsgID() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let key = ConversationKey(networkID: netID, channel: "#a")
        controller.applyLogLineForTest(
            network: "Net", channel: "#a", text: "x",
            connectionID: 1, serverMsgID: "pending:label-42")
        let ring = controller.messageRingForTest(conversation: key)
        XCTAssertEqual(ring.count, 1)
        XCTAssertNil(ring.last?.serverMsgID, "pending:* placeholders normalize to nil")
    }

    func testTypedEventDropsServerMsgID() {
        // Even if a typed event helper somehow carried a serverMsgID, the
        // controller's appendMessage seam should drop it for non-LOG_LINE events.
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let key = ConversationKey(networkID: netID, channel: "#a")
        controller.applyMembershipForTest(
            action: HC_APPLE_MEMBERSHIP_JOIN, network: "Net", channel: "#a",
            nick: "alice", connectionID: 1, timestampSeconds: 1_700_000_000)
        let ring = controller.messageRingForTest(conversation: key)
        XCTAssertFalse(ring.isEmpty)
        XCTAssertNil(ring.last?.serverMsgID)
    }

    func testConnectionHaveChathistoryFlippedByEvent() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1, connectionHaveChathistory: false)
        let conn = controller.connectionsByServerID[1]!
        XCTAssertEqual(controller.connections[conn]?.haveChathistory, false)
        // CAP NEW arrives: subsequent event carries the flipped bit.
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Net", channel: "#a",
            connectionID: 1, connectionHaveChathistory: true)
        XCTAssertEqual(controller.connections[conn]?.haveChathistory, true)
        // CAP DEL flips it back off.
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Net", channel: "#a",
            connectionID: 1, connectionHaveChathistory: false)
        XCTAssertEqual(controller.connections[conn]?.haveChathistory, false)
    }

    func testReplayWithSameServerMsgIDIsDroppedOverEventPath() throws {
        // End-to-end: two log-line events with same (network, channel, msgid,
        // timestamp) collapse to one row in both ring and store.
        let store = InMemoryMessageStore()
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: store, debounceInterval: .zero)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1, connectionHaveChathistory: true)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let key = ConversationKey(networkID: netID, channel: "#a")

        controller.applyLogLineForTest(
            network: "Net", channel: "#a", text: "hi",
            connectionID: 1, serverMsgID: "abc",
            connectionHaveChathistory: true,
            timestampSeconds: 1_700_000_000)
        XCTAssertEqual(controller.messageRingForTest(conversation: key).count, 1)
        XCTAssertEqual(try store.count(conversation: key), 1)

        controller.applyLogLineForTest(
            network: "Net", channel: "#a", text: "hi",
            connectionID: 1, serverMsgID: "abc",
            connectionHaveChathistory: true,
            timestampSeconds: 1_700_000_000)
        XCTAssertEqual(controller.messageRingForTest(conversation: key).count, 1)
        XCTAssertEqual(try store.count(conversation: key), 1)
    }

    func testBrokenStoreDoesNotMutateRing() {
        struct BrokenStore: MessageStore {
            func append(_ m: ChatMessage, conversation: ConversationKey) throws -> Bool {
                throw CocoaError(.fileWriteUnknown)
            }
            func page(conversation: ConversationKey, before: Date?, limit: Int) throws
                -> [ChatMessage]
            { [] }
            func count(conversation: ConversationKey) throws -> Int { 0 }
        }
        let controller = EngineController(
            persistence: InMemoryPersistenceStore(),
            messageStore: BrokenStore(),
            debounceInterval: .zero)
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_ACTIVATE, network: "Net", channel: "#a",
            connectionID: 1)
        let conn = controller.connectionsByServerID[1]!
        let netID = controller.connections[conn]!.networkID
        let sess = controller.sessionUUID(for: .composed(connectionID: conn, channel: "#a"))!
        let key = ConversationKey(networkID: netID, channel: "#a")
        controller.appendMessageForTest(
            ChatMessage(sessionID: sess, raw: "x", kind: .message(body: "x")))
        // Sync write-through threw; controller must not have mutated ring or messages.
        XCTAssertTrue(controller.messageRingForTest(conversation: key).isEmpty)
        XCTAssertTrue(
            controller.messages.isEmpty,
            "global messages array also must not contain a row whose write failed")
    }

    // MARK: - Phase 8 — WindowSession + multi-window helpers

    func testWindowSessionFocusFiresMarkRead() {
        let controller = EngineController()
        controller.upsertNetworkForTest(id: UUID(), name: "Libera")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
        let connIDW = controller.connections.values.first!.id
        let aID = controller.sessionUUID(for: .composed(connectionID: connIDW, channel: "#a"))!
        let key = controller.conversationKey(for: aID)!
        let bumped = ConversationState(key: key, draft: "", unread: 7, lastReadAt: nil)
        controller.setConversationStateForTest(bumped)

        let window = WindowSession(controller: controller)
        window.focusedSessionID = aID

        XCTAssertEqual(
            controller.conversations[key]?.unread, 0,
            "focusing a session must zero its unread count")
        XCTAssertNotNil(controller.conversations[key]?.lastReadAt)
    }

    func testTwoWindowSessionsHoldDifferentFocus() {
        let controller = EngineController()
        controller.upsertNetworkForTest(id: UUID(), name: "Libera")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
        let connID = controller.connections.values.first!.id
        let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
        let bID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#b"))!

        let win1 = WindowSession(controller: controller, initial: aID)
        let win2 = WindowSession(controller: controller, initial: bID)

        XCTAssertEqual(win1.focusedSessionID, aID)
        XCTAssertEqual(win2.focusedSessionID, bID)
        win1.focusedSessionID = bID
        XCTAssertEqual(win1.focusedSessionID, bID)
        XCTAssertEqual(win2.focusedSessionID, bID, "win2 was already on bID; unchanged")
        win1.focusedSessionID = aID
        XCTAssertEqual(win2.focusedSessionID, bID, "win2 must NOT follow win1")
    }

    func testMarkReadAndFocusTransitionBothClearUnread() {
        let controller = EngineController()
        controller.upsertNetworkForTest(id: UUID(), name: "Libera")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
        let connID = controller.connections.values.first!.id
        let aID = controller.sessionUUID(for: .composed(connectionID: connID, channel: "#a"))!
        let key = controller.conversationKey(for: aID)!
        controller.setConversationStateForTest(ConversationState(key: key, unread: 5))

        // Path 1: explicit markRead call
        controller.markRead(forSession: aID)
        let after1 = controller.conversations[key]
        XCTAssertEqual(after1?.unread, 0)

        // Path 2: recordFocusTransition (WindowSession focus change) — must be equivalent
        controller.setConversationStateForTest(ConversationState(key: key, unread: 5))
        controller.recordFocusTransition(from: nil, to: aID)
        let after2 = controller.conversations[key]
        XCTAssertEqual(after2?.unread, 0)
        XCTAssertNotNil(after2?.lastReadAt)
    }

    func testVisibleHelpersHonorSessionParameter() {
        let controller = EngineController()
        controller.upsertNetworkForTest(id: UUID(), name: "Libera")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b",
            sessionID: 2, connectionID: 1, selfNick: "me")
        let aID = controller.sessionUUID(for: .runtime(id: 1))!
        let bID = controller.sessionUUID(for: .runtime(id: 2))!

        controller.applyLogLineForTest(
            network: "Libera", channel: "#a", text: "hello-a",
            sessionID: 1, connectionID: 1, selfNick: "me")
        controller.applyLogLineForTest(
            network: "Libera", channel: "#b", text: "hello-b",
            sessionID: 2, connectionID: 1, selfNick: "me")

        XCTAssertEqual(controller.visibleMessages(for: aID).map(\.raw), ["hello-a"])
        XCTAssertEqual(controller.visibleMessages(for: bID).map(\.raw), ["hello-b"])
        XCTAssertTrue(controller.visibleSessionTitle(for: aID).contains("#a"))
        XCTAssertTrue(controller.visibleSessionTitle(for: bID).contains("#b"))
    }

    func testDraftBindingForSessionScopesPerSession() {
        let controller = EngineController()
        controller.upsertNetworkForTest(id: UUID(), name: "Libera")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b",
            sessionID: 2, connectionID: 1, selfNick: "me")
        let aID = controller.sessionUUID(for: .runtime(id: 1))!
        let bID = controller.sessionUUID(for: .runtime(id: 2))!

        let aBinding = controller.draftBinding(for: aID)
        let bBinding = controller.draftBinding(for: bID)

        aBinding.wrappedValue = "draft-for-a"
        bBinding.wrappedValue = "draft-for-b"

        XCTAssertEqual(aBinding.wrappedValue, "draft-for-a")
        XCTAssertEqual(bBinding.wrappedValue, "draft-for-b")
        XCTAssertEqual(controller.draftBinding(for: aID).wrappedValue, "draft-for-a")
        XCTAssertEqual(controller.draftBinding(for: bID).wrappedValue, "draft-for-b")
    }

    func testDraftBindingForNilSessionReadsAndWritesEmpty() {
        let controller = EngineController()
        let nilBinding = controller.draftBinding(for: nil)
        XCTAssertEqual(nilBinding.wrappedValue, "")
        nilBinding.wrappedValue = "ignored"
        XCTAssertEqual(nilBinding.wrappedValue, "")
        XCTAssertTrue(controller.conversations.isEmpty, "nil binding must not create conversation state")
    }

    func testPrefillPrivateMessageForSessionTargetsTheRightDraft() {
        let controller = EngineController()
        controller.upsertNetworkForTest(id: UUID(), name: "Libera")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b",
            sessionID: 2, connectionID: 1, selfNick: "me")
        let aID = controller.sessionUUID(for: .runtime(id: 1))!
        let bID = controller.sessionUUID(for: .runtime(id: 2))!

        controller.prefillPrivateMessage(to: "alice", forSession: aID)
        XCTAssertEqual(controller.draftBinding(for: aID).wrappedValue, "/msg alice ")
        XCTAssertEqual(controller.draftBinding(for: bID).wrappedValue, "")
    }

    // MARK: - Phase 8 Task 3 — Transferable + Codable

    func testChatUserCarriesConnectionIDAndUniqueID() {
        let controller = EngineController()
        controller.upsertNetworkForTest(id: UUID(), name: "Libera")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 1, selfNick: "me")
        controller.applyUserlistForTest(
            action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a", nick: "alice",
            sessionID: 1, connectionID: 1)
        let user = controller.usersBySession.values.flatMap { $0 }.first { $0.nick == "alice" }!
        XCTAssertNotEqual(user.connectionID, UUID(uuid: (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)))
        XCTAssertTrue(user.id.contains(user.connectionID.uuidString.lowercased()))
        XCTAssertTrue(user.id.hasSuffix("::alice"))
    }

    func testChatSessionRoundTripsThroughTransferable() throws {
        let session = ChatSession(connectionID: UUID(), channel: "#a", isActive: true)
        let data = try JSONEncoder().encode(session)
        let decoded = try JSONDecoder().decode(ChatSession.self, from: data)
        XCTAssertEqual(decoded.id, session.id)
        XCTAssertEqual(decoded.connectionID, session.connectionID)
        XCTAssertEqual(decoded.channel, session.channel)
        XCTAssertEqual(decoded.isActive, session.isActive)
    }

    func testChatUserRoundTripsThroughTransferable() throws {
        let user = ChatUser(
            connectionID: UUID(), nick: "alice", modePrefix: "@",
            account: "alicea", host: "alice@example.com", isMe: false, isAway: true)
        let data = try JSONEncoder().encode(user)
        let decoded = try JSONDecoder().decode(ChatUser.self, from: data)
        XCTAssertEqual(decoded.connectionID, user.connectionID)
        XCTAssertEqual(decoded.nick, "alice")
        XCTAssertEqual(decoded.modePrefix, "@")
        XCTAssertEqual(decoded.account, "alicea")
        XCTAssertEqual(decoded.host, "alice@example.com")
        XCTAssertFalse(decoded.isMe)
        XCTAssertTrue(decoded.isAway)
    }

    func testConnectionRoundTripsThroughTransferable() throws {
        let conn = Connection(
            id: UUID(), networkID: UUID(),
            serverName: "irc.libera.chat", selfNick: "me", haveChathistory: true)
        let data = try JSONEncoder().encode(conn)
        let decoded = try JSONDecoder().decode(Connection.self, from: data)
        XCTAssertEqual(decoded.id, conn.id)
        XCTAssertEqual(decoded.networkID, conn.networkID)
        XCTAssertEqual(decoded.serverName, "irc.libera.chat")
        XCTAssertEqual(decoded.selfNick, "me")
        XCTAssertTrue(decoded.haveChathistory)
    }

    func testChatSessionTransferableExportsPlainText() {
        let session = ChatSession(connectionID: UUID(), channel: "#a", isActive: false)
        XCTAssertEqual(session.plainTextDescription, "#a")
    }

    func testChatUserTransferableExportsNickAsPlainText() {
        let user = ChatUser(connectionID: UUID(), nick: "alice")
        XCTAssertEqual(user.plainTextDescription, "alice")
    }

    func testChatMessageTransferableExportsBodyAsPlainText() {
        let m = ChatMessage(
            sessionID: UUID(), raw: "hello world", kind: .message(body: "hello world"))
        XCTAssertEqual(m.plainTextDescription, "hello world")
        let joinMsg = ChatMessage(
            sessionID: UUID(), raw: "alice has joined #a", kind: .join)
        XCTAssertEqual(
            joinMsg.plainTextDescription, "alice has joined #a",
            "kinds without a body fall through to .raw")
    }

    func testNetworkTransferableExportsDisplayNameAsPlainText() {
        let network = Network(id: UUID(), displayName: "Libera Chat")
        XCTAssertEqual(network.plainTextDescription, "Libera Chat")
    }

    func testConnectionTransferableExportsServerNameAsPlainText() {
        let conn = Connection(
            id: UUID(), networkID: UUID(),
            serverName: "irc.libera.chat", selfNick: nil, haveChathistory: false)
        XCTAssertEqual(conn.plainTextDescription, "irc.libera.chat")
    }

    func testConnectionDecoderTreatsMissingHaveChathistoryAsFalse() throws {
        let id = UUID().uuidString
        let networkID = UUID().uuidString
        let json = """
            {
                "id": "\(id)",
                "networkID": "\(networkID)",
                "serverName": "irc.libera.chat"
            }
            """.data(using: .utf8)!
        let decoded = try JSONDecoder().decode(Connection.self, from: json)
        XCTAssertEqual(decoded.serverName, "irc.libera.chat")
        XCTAssertNil(decoded.selfNick)
        XCTAssertFalse(decoded.haveChathistory, "missing key must decode to false")
    }

    func testChatUserDecoderRejectsMultiCharacterModePrefix() {
        // Construct a JSON payload by hand so we can inject an invalid modePrefix.
        let connID = UUID().uuidString
        let jsonStr =
            "{\"connectionID\":\"\(connID)\",\"nick\":\"alice\","
            + "\"modePrefix\":\"@@\",\"isMe\":false,\"isAway\":false}"
        let json = jsonStr.data(using: .utf8)!
        XCTAssertThrowsError(try JSONDecoder().decode(ChatUser.self, from: json)) { error in
            guard case DecodingError.dataCorrupted = error else {
                XCTFail("expected DecodingError.dataCorrupted, got \(error)")
                return
            }
        }
    }

    func testWindowSessionFocusTransitionRegistersWithController() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

        let window = WindowSession(controller: controller, initial: aID)
        XCTAssertEqual(controller.focusRefcount[aID], 1, "init with non-nil initial registers a refcount")
        XCTAssertEqual(controller.lastFocusedSessionID, aID)

        withExtendedLifetime(window) {}
    }

    func testTwoWindowSessionsTrackFocusIndependently() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id

        let win1 = WindowSession(controller: controller, initial: aID)
        let win2 = WindowSession(controller: controller, initial: bID)
        XCTAssertEqual(controller.focusRefcount[aID], 1)
        XCTAssertEqual(controller.focusRefcount[bID], 1)

        win1.focusedSessionID = bID
        XCTAssertNil(controller.focusRefcount[aID])
        XCTAssertEqual(controller.focusRefcount[bID], 2, "two windows on B contribute 2")
        XCTAssertEqual(controller.lastFocusedSessionID, bID)

        withExtendedLifetime(win2) {}
    }

    func testWindowSessionDeinitDecrementsRefcount() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

        do {
            let window = WindowSession(controller: controller, initial: aID)
            withExtendedLifetime(window) {
                XCTAssertEqual(controller.focusRefcount[aID], 1)
            }
        }  // window goes out of scope; deinit fires
        XCTAssertNil(controller.focusRefcount[aID], "deinit must remove the refcount entry")
        // lastFocusedSessionID survives deinit by design.
        XCTAssertEqual(controller.lastFocusedSessionID, aID)
    }

    func testPrefillPrivateMessageInvokedFromDropPath() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 1, selfNick: "me")
        let aID = controller.sessionUUID(for: .runtime(id: 1))!
        let connID = controller.connections.values.first!.id

        // Simulate the drop callback's body — what ContentView's
        // .dropDestination(for: ChatUser.self) closure will execute.
        let dropped = ChatUser(connectionID: connID, nick: "alice")
        controller.prefillPrivateMessage(to: dropped.nick, forSession: aID)

        XCTAssertEqual(controller.draftBinding(for: aID).wrappedValue, "/msg alice ")
    }

    func testSidebarRefocusFromDropPath() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 1, selfNick: "me")
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b",
            sessionID: 2, connectionID: 1, selfNick: "me")
        let aID = controller.sessionUUID(for: .runtime(id: 1))!
        let bID = controller.sessionUUID(for: .runtime(id: 2))!

        let window = WindowSession(controller: controller, initial: aID)
        let droppedSession = controller.sessions.first { $0.id == bID }!

        // Simulate the drop callback's body:
        if controller.sessions.contains(where: { $0.id == droppedSession.id }) {
            window.focusedSessionID = droppedSession.id
        }
        XCTAssertEqual(window.focusedSessionID, bID)
    }

    func testDroppedUnknownSessionIsNoOp() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a",
            sessionID: 1, connectionID: 1, selfNick: "me")
        let aID = controller.sessionUUID(for: .runtime(id: 1))!

        let window = WindowSession(controller: controller, initial: aID)
        let synthetic = ChatSession(connectionID: UUID(), channel: "#unknown", isActive: false)

        // Simulate the typed-rejection guard in the drop callback:
        if controller.sessions.contains(where: { $0.id == synthetic.id }) {
            window.focusedSessionID = synthetic.id
        }
        XCTAssertEqual(window.focusedSessionID, aID, "unknown session must not refocus the window")
    }

    func testWindowSessionUUIDStringEncodingRoundTrips() {
        let original = UUID()
        let encoded = WindowSession.encode(focused: original)
        XCTAssertFalse(encoded.isEmpty, "non-nil UUID must encode to non-empty string")
        XCTAssertEqual(WindowSession.decode(focused: encoded), original)

        XCTAssertEqual(WindowSession.encode(focused: nil), "", "nil UUID encodes as empty string")
        XCTAssertNil(WindowSession.decode(focused: ""), "empty string decodes as nil")
        XCTAssertNil(WindowSession.decode(focused: "garbage"), "non-UUID string decodes as nil")
    }

    // MARK: - Phase 9 Task 1: recordFocusTransition / focusRefcount / lastFocusedSessionID

    func testRecordFocusTransitionFromNilToASetsRefcountAndLastFocused() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        controller.recordFocusTransition(from: nil, to: aID)
        XCTAssertEqual(controller.focusRefcount[aID], 1)
        XCTAssertEqual(controller.lastFocusedSessionID, aID)
    }

    func testRecordFocusTransitionAToBMovesRefcountAndLastFocused() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id
        controller.recordFocusTransition(from: nil, to: aID)
        controller.recordFocusTransition(from: aID, to: bID)
        XCTAssertNil(controller.focusRefcount[aID], "old session removed from refcount when count reaches 0")
        XCTAssertEqual(controller.focusRefcount[bID], 1)
        XCTAssertEqual(controller.lastFocusedSessionID, bID)
    }

    func testRecordFocusTransitionTwoWindowsSameSessionRefcounts() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        controller.recordFocusTransition(from: nil, to: aID)
        controller.recordFocusTransition(from: nil, to: aID)
        XCTAssertEqual(controller.focusRefcount[aID], 2)
        controller.recordFocusTransition(from: aID, to: nil)
        XCTAssertEqual(controller.focusRefcount[aID], 1, "one window still focused — refcount stays positive")
    }

    func testRecordFocusTransitionToNilLeavesLastFocusedAlone() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        controller.recordFocusTransition(from: nil, to: aID)
        XCTAssertEqual(controller.lastFocusedSessionID, aID)
        controller.recordFocusTransition(from: aID, to: nil)
        XCTAssertEqual(controller.lastFocusedSessionID, aID, "nil-target focus must not erase cold-launch hint")
        XCTAssertNil(controller.focusRefcount[aID])
    }

    func testRecordFocusTransitionToNonNilTriggersMarkRead() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let key = controller.conversationKey(for: aID)!
        controller.setConversationStateForTest(ConversationState(key: key, draft: "", unread: 5, lastReadAt: nil))
        controller.recordFocusTransition(from: nil, to: aID)
        XCTAssertEqual(controller.conversations[key]?.unread, 0, "transitioning focus onto a session must clear unread")
    }

    func testRecordFocusTransitionSelfTransitionIsNoOp() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        controller.recordFocusTransition(from: nil, to: aID)
        XCTAssertEqual(controller.focusRefcount[aID], 1)

        controller.recordFocusTransition(from: aID, to: aID)
        XCTAssertEqual(controller.focusRefcount[aID], 1, "self-transition must not perturb refcount")
        XCTAssertEqual(controller.lastFocusedSessionID, aID, "self-transition must not erase lastFocusedSessionID")
    }

    func testSnapshotEmitsLastFocusedKeyFromLastFocusedSessionID() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let key = controller.conversationKey(for: aID)!
        controller.recordFocusTransition(from: nil, to: aID)

        let snapshot = controller.snapshotForPersistence()
        XCTAssertEqual(snapshot.lastFocusedKey, key)
    }

    func testApplyStoresLastFocusedKeyAsPendingAndUpsertResolves() {
        let controller = EngineController()
        // Build a snapshot whose lastFocusedKey points at a session not yet emitted.
        // The ConversationKey shape uses (networkID, channel) — see ConversationKey
        // definition near struct ConversationState in EngineController.swift.
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let key = controller.conversationKey(for: aID)!
        // Capture the network so the snapshot can carry the same UUID across the
        // STOPPED → apply boundary; otherwise reconnect mints a fresh networkID
        // and the pending key — keyed on the original UUID — never resolves.
        let liberaNet = controller.networks[key.networkID]!
        // Tear down so we can simulate "snapshot has key but session not yet emitted":
        controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)

        let snapshot = AppState(
            schemaVersion: AppState.currentSchemaVersion,
            networks: [liberaNet],
            conversations: [],
            lastFocusedKey: key,
            commandHistory: [])
        controller.applyForTest(snapshot)

        XCTAssertNil(controller.lastFocusedSessionID, "no session re-emitted yet — pending only")
        // The C core re-emits the session:
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let resolvedID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        XCTAssertEqual(controller.lastFocusedSessionID, resolvedID, "deferred resolution must fire on first matching upsert")
    }

    func testApplyWithNoLastFocusedKeyLeavesPendingNil() {
        let controller = EngineController()
        let snapshot = AppState(
            schemaVersion: AppState.currentSchemaVersion,
            networks: [], conversations: [], lastFocusedKey: nil, commandHistory: [])
        controller.applyForTest(snapshot)
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        XCTAssertNil(controller.lastFocusedSessionID, "no pending key — no resolution")
    }

    func testPendingLastFocusedKeySurvivesLifecycleStopped() {
        let controller = EngineController()
        // Set up a key that matches a future Libera/#a session.
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let key = controller.conversationKey(for: aID)!
        let liberaNet = controller.networks[key.networkID]!
        controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)

        let snapshot = AppState(
            schemaVersion: AppState.currentSchemaVersion,
            networks: [liberaNet], conversations: [], lastFocusedKey: key, commandHistory: [])
        controller.applyForTest(snapshot)
        // Hit STOPPED again before any session re-emitted — pending key must survive.
        controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)
        // After STOPPED, simulate reconnect: ACTIVATE the same channel.
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let resolvedID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        XCTAssertEqual(controller.lastFocusedSessionID, resolvedID, "pending key must survive STOPPED")
    }

    func testLifecycleStoppedClearsFocusRefcount() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        controller.recordFocusTransition(from: nil, to: aID)
        XCTAssertEqual(controller.focusRefcount[aID], 1)

        controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)
        XCTAssertTrue(controller.focusRefcount.isEmpty, "STOPPED clears refcount; live windows re-add on next focus change")
        // lastFocusedSessionID survives STOPPED.
        XCTAssertEqual(controller.lastFocusedSessionID, aID)
    }

    // MARK: - Phase 9 Task 5: focusRefcount-based unread suppression

    func testFocusedWindowSuppressesUnread() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let key = controller.conversationKey(for: aID)!
        let window = WindowSession(controller: controller, initial: aID)

        controller.appendMessageForTest(
            ChatMessage(sessionID: aID, raw: "hello", kind: .message(body: "hello")))
        XCTAssertEqual(controller.conversations[key]?.unread ?? 0, 0)

        withExtendedLifetime(window) {}
    }

    func testTenSequentialMessagesInFocusedSessionLeaveUnreadAtZero() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let key = controller.conversationKey(for: aID)!
        let window = WindowSession(controller: controller, initial: aID)

        for i in 0..<10 {
            controller.appendMessageForTest(
                ChatMessage(sessionID: aID, raw: "msg \(i)", kind: .message(body: "msg \(i)")))
        }
        XCTAssertEqual(
            controller.conversations[key]?.unread ?? 0, 0,
            "regression: didSet only fires on focus change, but refcount must keep suppression alive")

        withExtendedLifetime(window) {}
    }

    func testUnfocusedSessionStillIncrementsUnread() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id
        let bKey = controller.conversationKey(for: bID)!
        let window = WindowSession(controller: controller, initial: aID)

        controller.appendMessageForTest(
            ChatMessage(sessionID: bID, raw: "ping", kind: .message(body: "ping")))
        XCTAssertEqual(controller.conversations[bKey]?.unread ?? 0, 1)

        withExtendedLifetime(window) {}
    }

    func testTwoWindowsOneClosedKeepsSuppression() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let key = controller.conversationKey(for: aID)!

        let win1 = WindowSession(controller: controller, initial: aID)
        do {
            let win2 = WindowSession(controller: controller, initial: aID)
            withExtendedLifetime(win2) {}
        }  // win2 deinit → refcount goes from 2 to 1

        controller.appendMessageForTest(
            ChatMessage(sessionID: aID, raw: "hi", kind: .message(body: "hi")))
        XCTAssertEqual(
            controller.conversations[key]?.unread ?? 0, 0,
            "win1 still focused on A — suppression must hold")

        withExtendedLifetime(win1) {}
    }

    func testWindowSessionUnreadStartsEmpty() {
        let controller = EngineController()
        let window = WindowSession(controller: controller, initial: nil)
        XCTAssertTrue(window.unread.isEmpty)
    }

    func testWindowSessionFocusClearsItsOwnUnread() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

        let window = WindowSession(controller: controller, initial: nil)
        window.unread[aID] = 7
        window.focusedSessionID = aID
        XCTAssertEqual(window.unread[aID, default: 0], 0,
                       "focus transition must clear this window's unread for the new session")
        withExtendedLifetime(window) {}
    }

    func testWindowSessionSameValueWriteDoesNotClearOtherUnread() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id

        let window = WindowSession(controller: controller, initial: aID)
        window.unread[bID] = 3
        // Same-value write — didSet short-circuits.
        window.focusedSessionID = aID
        XCTAssertEqual(window.unread[bID, default: 0], 3,
                       "didSet short-circuit must not touch unread for other sessions")
        withExtendedLifetime(window) {}
    }

    func testWindowSessionFocusToNilLeavesUnreadAlone() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

        let window = WindowSession(controller: controller, initial: aID)
        window.unread[aID] = 5
        window.focusedSessionID = nil
        XCTAssertEqual(window.unread[aID, default: 0], 5,
                       "focus → nil is not mark-read; unread map is unchanged")
        withExtendedLifetime(window) {}
    }

    func testWindowRegistryRegistersOnInit() {
        let controller = EngineController()
        XCTAssertEqual(controller.registeredWindowCountForTest, 0)
        let window = WindowSession(controller: controller, initial: nil)
        XCTAssertEqual(controller.registeredWindowCountForTest, 1)
        withExtendedLifetime(window) {}
    }

    func testWindowRegistryUnregistersOnDeinit() {
        let controller = EngineController()
        do {
            let window = WindowSession(controller: controller, initial: nil)
            withExtendedLifetime(window) {
                XCTAssertEqual(controller.registeredWindowCountForTest, 1)
            }
        }
        XCTAssertEqual(controller.registeredWindowCountForTest, 0,
                       "deinit must unregister synchronously via MainActor.assumeIsolated")
    }

    func testWindowRegistryTracksMultipleWindowsIndependently() {
        let controller = EngineController()
        let win1 = WindowSession(controller: controller, initial: nil)
        let win2 = WindowSession(controller: controller, initial: nil)
        XCTAssertEqual(controller.registeredWindowCountForTest, 2)
        withExtendedLifetime(win1) {}
        withExtendedLifetime(win2) {}
    }

    // MARK: - Task 3: Per-window unread bump in recordActivity

    func testRecordActivityBumpsNonFocusedWindow() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id

        let window = WindowSession(controller: controller, initial: aID)
        controller.appendMessageForTest(
            ChatMessage(sessionID: bID, raw: "ping", kind: .message(body: "ping")))
        XCTAssertEqual(window.unread[bID, default: 0], 1,
                       "non-focused session must bump per-window unread")
        XCTAssertEqual(window.unread[aID, default: 0], 0,
                       "focused session must not bump per-window unread")
        withExtendedLifetime(window) {}
    }

    func testRecordActivityFocusedWindowStaysAtZero() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

        let window = WindowSession(controller: controller, initial: aID)
        for i in 0..<10 {
            controller.appendMessageForTest(
                ChatMessage(sessionID: aID, raw: "msg \(i)", kind: .message(body: "msg \(i)")))
        }
        XCTAssertEqual(window.unread[aID, default: 0], 0,
                       "10 messages in focused session must keep per-window unread at 0")
        withExtendedLifetime(window) {}
    }

    func testRecordActivityTwoWindowsTrackIndependently() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id

        let winA = WindowSession(controller: controller, initial: aID)
        let winB = WindowSession(controller: controller, initial: bID)

        controller.appendMessageForTest(
            ChatMessage(sessionID: bID, raw: "ping b", kind: .message(body: "ping b")))
        XCTAssertEqual(winA.unread[bID, default: 0], 1, "winA didn't focus B → bumps")
        XCTAssertEqual(winB.unread[bID, default: 0], 0, "winB focused B → stays 0")

        controller.appendMessageForTest(
            ChatMessage(sessionID: aID, raw: "ping a", kind: .message(body: "ping a")))
        XCTAssertEqual(winA.unread[aID, default: 0], 0, "winA focused A → stays 0")
        XCTAssertEqual(winB.unread[aID, default: 0], 1, "winB didn't focus A → bumps")
        withExtendedLifetime(winA) {}
        withExtendedLifetime(winB) {}
    }

    func testRecordActivityBothWindowsFocusedSameSessionStaysAtZero() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id

        let winA = WindowSession(controller: controller, initial: aID)
        let winB = WindowSession(controller: controller, initial: aID)

        controller.appendMessageForTest(
            ChatMessage(sessionID: aID, raw: "ping", kind: .message(body: "ping")))
        XCTAssertEqual(winA.unread[aID, default: 0], 0)
        XCTAssertEqual(winB.unread[aID, default: 0], 0)
        withExtendedLifetime(winA) {}
        withExtendedLifetime(winB) {}
    }

    func testRecordActivityGlobalCounterStillSuppressedByAnyFocus() {
        // Regression: existing focusRefcount-based global suppression must
        // continue to work alongside the new per-window bump.
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let key = controller.conversationKey(for: aID)!

        let window = WindowSession(controller: controller, initial: aID)
        controller.appendMessageForTest(
            ChatMessage(sessionID: aID, raw: "ping", kind: .message(body: "ping")))
        XCTAssertEqual(controller.conversations[key]?.unread ?? 0, 0,
                       "focusRefcount-based global suppression unchanged")
        withExtendedLifetime(window) {}
    }

    func testRecordActivityGlobalCounterBumpsWhenNoWindowFocused() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id
        let bKey = controller.conversationKey(for: bID)!

        let window = WindowSession(controller: controller, initial: aID)
        controller.appendMessageForTest(
            ChatMessage(sessionID: bID, raw: "ping", kind: .message(body: "ping")))
        XCTAssertEqual(controller.conversations[bKey]?.unread ?? 0, 1)
        withExtendedLifetime(window) {}
    }

    func testRecordActivitySystemPseudoSessionDoesNotBumpAnyCounter() {
        let controller = EngineController()
        let window = WindowSession(controller: controller, initial: nil)

        // Step 1: trigger creation of the system pseudo-session via an
        // unattributed message. resolveMessageSessionID(event:nil) falls through
        // to systemSessionUUID(), lazily populating systemSessionUUIDStorage.
        controller.appendUnattributedForTest(
            raw: "console banner",
            kind: .lifecycle(phase: "info", body: "console banner"))

        let systemConn = controller.systemConnectionUUIDForTest()
        let systemUUID = controller.sessions.first(where: { $0.connectionID == systemConn })!.id

        // Step 2: send an *attributed* message (attributed:true path) directly to
        // the system-pseudo-session UUID. This is the code path that hits
        // `recordActivity(on:)` — appendMessageForTest calls append(attributed:true)
        // which calls recordActivity. The guard `sessionID != systemSessionUUIDStorage`
        // must intercept and return without bumping any counter.
        controller.appendMessageForTest(
            ChatMessage(sessionID: systemUUID, raw: "server banner",
                        kind: .lifecycle(phase: "info", body: "server banner")))

        XCTAssertTrue(window.unread.isEmpty,
                      "system pseudo-session must not bump per-window unread")
        let key = controller.conversationKey(for: systemUUID)
        if let key {
            XCTAssertEqual(controller.conversations[key]?.unread ?? 0, 0,
                           "system pseudo-session must not bump global unread either")
        }
        // The system session may have no ConversationKey (system connection isn't
        // a real network connection); in that case the global guard's
        // `let key = conversationKey(for: sessionID)` would have bailed too. The
        // per-window early-return is what makes the standalone guard correct.
        withExtendedLifetime(window) {}
    }

    func testSessionRemoveScrubsStaleUUIDFromWindowUnread() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
        let aID = controller.sessions.first(where: { $0.channel == "#a" })!.id
        let bID = controller.sessions.first(where: { $0.channel == "#b" })!.id

        let window = WindowSession(controller: controller, initial: aID)
        controller.appendMessageForTest(
            ChatMessage(sessionID: bID, raw: "ping", kind: .message(body: "ping")))
        XCTAssertEqual(window.unread[bID, default: 0], 1)

        controller.applySessionForTest(action: HC_APPLE_SESSION_REMOVE, network: "Libera", channel: "#b")
        XCTAssertNil(window.unread[bID],
                     "REMOVE must scrub the UUID key from every window's unread map")
        withExtendedLifetime(window) {}
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
    _ = controller.visibleUsers(for: controller.activeSessionID)
}
#endif
