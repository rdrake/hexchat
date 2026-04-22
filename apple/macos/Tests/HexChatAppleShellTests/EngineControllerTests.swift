#if canImport(XCTest)
import XCTest
@testable import HexChatAppleShell
import AppleAdapterBridge

final class EngineControllerTests: XCTestCase {
    func testUserlistInsertUpdateRemoveAndClear() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#hexchat")

        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#hexchat", nick: "bob")
        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#hexchat", nick: "@alice")
        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#hexchat", nick: "+bob")

        XCTAssertEqual(controller.visibleUsers, ["@alice", "+bob"])

        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_REMOVE, network: "Libera", channel: "#hexchat", nick: "bob")
        XCTAssertEqual(controller.visibleUsers, ["@alice"])

        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_CLEAR, network: "Libera", channel: "#hexchat", nick: nil)
        XCTAssertTrue(controller.visibleUsers.isEmpty)
    }

    func testChannelScopedUserlistsDoNotBleed() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")

        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a", nick: "alice")
        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#b", nick: "bob")

        controller.selectedSessionID = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))
        XCTAssertEqual(controller.visibleUsers, ["alice"])

        controller.selectedSessionID = controller.sessionUUID(for: .composed(network: "Libera", channel: "#b"))
        XCTAssertEqual(controller.visibleUsers, ["bob"])
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
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")
        controller.selectedSessionID = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))

        controller.applyLogLineForTest(network: "Libera", channel: "#b", text: "message for b")

        let sessionA = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))!
        let sessionB = controller.sessionUUID(for: .composed(network: "Libera", channel: "#b"))!
        XCTAssertFalse(controller.messages.contains(where: { $0.sessionID == sessionA && $0.raw == "message for b" }))
        XCTAssertTrue(controller.messages.contains(where: { $0.sessionID == sessionB && $0.raw == "message for b" }))
    }

    func testRuntimeSessionIDSeparatesSameNetworkChannelLabel() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#same", sessionID: 101)
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#same", sessionID: 202)

        controller.applyLogLineForTest(network: "Libera", channel: "#same", text: "message for 202", sessionID: 202)

        let sessionB202 = controller.sessionUUID(for: .runtime(id: 202))!
        let sessionB101 = controller.sessionUUID(for: .runtime(id: 101))!
        XCTAssertTrue(controller.messages.contains(where: { $0.sessionID == sessionB202 && $0.raw == "message for 202" }))
        XCTAssertFalse(controller.messages.contains(where: { $0.sessionID == sessionB101 && $0.raw == "message for 202" }))
    }

    func testServerAndChannelSessionsAreDistinctForUILists() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "server", sessionID: 1)
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#cybercafe", sessionID: 2)

        let serverUUID = controller.sessionUUID(for: .runtime(id: 1))
        let channelUUID = controller.sessionUUID(for: .runtime(id: 2))
        XCTAssertTrue(controller.sessions.contains(where: { $0.id == serverUUID && $0.channel == "server" }))
        XCTAssertTrue(controller.sessions.contains(where: { $0.id == channelUUID && $0.channel == "#cybercafe" }))
        XCTAssertEqual(controller.networkSections.first(where: { $0.name == "AfterNET" })?.sessions.count, 2)
    }

    func testChannelUserlistDoesNotPopulateServerSessionUsers() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "server", sessionID: 1)
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "AfterNET", channel: "#cybercafe", sessionID: 2)

        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "AfterNET", channel: "#cybercafe", nick: "alice", sessionID: 2)
        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "AfterNET", channel: "#cybercafe", nick: "@bob", sessionID: 2)

        controller.selectedSessionID = controller.sessionUUID(for: .runtime(id: 1))
        XCTAssertTrue(controller.visibleUsers.isEmpty)

        controller.selectedSessionID = controller.sessionUUID(for: .runtime(id: 2))
        XCTAssertEqual(controller.visibleUsers, ["@bob", "alice"])
    }

    func testServerAndChannelMessagesRemainRoutedToOwnSessions() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "server", sessionID: 1)
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#cybercafe", sessionID: 2)

        controller.applyLogLineForTest(network: "AfterNET", channel: "server", text: "-server notice-", sessionID: 1)
        controller.applyLogLineForTest(network: "AfterNET", channel: "#cybercafe", text: "<alice> hi", sessionID: 2)

        let serverSession = controller.sessionUUID(for: .runtime(id: 1))!
        let channelSession = controller.sessionUUID(for: .runtime(id: 2))!
        XCTAssertTrue(controller.messages.contains(where: { $0.sessionID == serverSession && $0.raw == "-server notice-" }))
        XCTAssertTrue(controller.messages.contains(where: { $0.sessionID == channelSession && $0.raw == "<alice> hi" }))
        XCTAssertFalse(controller.messages.contains(where: { $0.sessionID == serverSession && $0.raw == "<alice> hi" }))
        XCTAssertFalse(controller.messages.contains(where: { $0.sessionID == channelSession && $0.raw == "-server notice-" }))
    }

    func testSessionLocatorRoundTripsComposedAndRuntimeKeys() {
        let composed = SessionLocator.composed(network: "Libera", channel: "#a")
        XCTAssertEqual(
            composed,
            SessionLocator.composed(network: "libera", channel: "#A"),
            "composed locator equality must be case-insensitive on network/channel"
        )

        let runtime = SessionLocator.runtime(id: 42)
        XCTAssertNotEqual(runtime, SessionLocator.runtime(id: 43))
        XCTAssertNotEqual(runtime, composed)

        // Hash parity: equal values must share a hash bucket.
        var seen: Set<SessionLocator> = []
        seen.insert(composed)
        XCTAssertTrue(seen.contains(SessionLocator.composed(network: "LIBERA", channel: "#A")))
    }

    func testVisibleSessionIDFallbackWhenNoSessions() {
        let controller = EngineController()
        XCTAssertTrue(controller.sessions.isEmpty)
        // With no sessions, visibleSessionID must return the specific synthetic fallback id.
        XCTAssertEqual(
            controller.visibleSessionID,
            "network::server",
            "fresh controller with no sessions must fall back to the synthetic network::server id"
        )
        // And visibleMessages must simply be empty, not crash.
        XCTAssertTrue(controller.visibleMessages.isEmpty)
        XCTAssertTrue(controller.visibleUsers.isEmpty)
    }

    func testVisibleSessionIDPrefersSelectedOverActiveOverFirst() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "AfterNET", channel: "#b")
        // Post-activate: active = #b. selected was set to #a by the first upsert.
        let a = "afternet::#a"
        let b = "afternet::#b"
        let aUUID = controller.sessionUUID(for: .composed(network: "AfterNET", channel: "#a"))!
        controller.selectedSessionID = aUUID
        XCTAssertEqual(controller.visibleSessionID, a, "selected takes precedence over active")
        controller.selectedSessionID = nil
        XCTAssertEqual(controller.visibleSessionID, b, "active chosen when selected is nil")
        controller.activeSessionID = nil
        // both selected and active are now nil — should fall back to sessions.first
        // sessions are sorted so #a comes first alphabetically.
        XCTAssertEqual(controller.visibleSessionID, a, "first session used when both selected and active are nil")
    }

    func testChatSessionCarriesStableUUIDAcrossMutations() {
        var session = ChatSession(network: "Libera", channel: "#a", isActive: false)
        let firstID = session.id
        session.network = "RenamedNetwork"
        session.channel = "#renamed"
        XCTAssertEqual(session.id, firstID, "ID must not change when other fields mutate")
    }

    func testSessionByLocatorIndexPopulatesAndRemoves() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
        let locator = SessionLocator.composed(network: "Libera", channel: "#a")
        XCTAssertNotNil(controller.sessionUUID(for: locator))

        controller.applySessionForTest(action: HC_APPLE_SESSION_REMOVE, network: "Libera", channel: "#a")
        XCTAssertNil(controller.sessionUUID(for: locator))
    }

    func testSessionByLocatorIndexHandlesRuntimeID() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#same", sessionID: 42)
        XCTAssertNotNil(controller.sessionUUID(for: .runtime(id: 42)))
    }

    func testSessionByLocatorPurgesStaleCompositesOnRename() {
        // If a session's (network, channel) changes, stale composed locators must not linger.
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#old")
        let oldLocator = SessionLocator.composed(network: "Libera", channel: "#old")
        let uuid = controller.sessionUUID(for: oldLocator)!

        // Mutate the session in place. Emitting UPSERT with the same locator but new channel simulates a rename.
        controller.applyRenameForTest(network: "Libera", fromChannel: "#old", toChannel: "#new")
        XCTAssertNil(controller.sessionUUID(for: oldLocator), "old composed locator must purge")
        XCTAssertEqual(controller.sessionUUID(for: .composed(network: "Libera", channel: "#new")), uuid)
    }

    func testLifecycleStoppedClearsLocatorIndex() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#a")
        controller.applyLifecycleForTest(phase: HC_APPLE_LIFECYCLE_STOPPED)
        XCTAssertNil(controller.sessionUUID(for: .composed(network: "Libera", channel: "#a")))
    }

    func testSessionRemovePurgesRuntimeLocator() {
        let controller = EngineController()
        controller.applySessionForTest(
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#r", sessionID: 77)
        XCTAssertNotNil(controller.sessionUUID(for: .runtime(id: 77)))

        controller.applySessionForTest(
            action: HC_APPLE_SESSION_REMOVE, network: "AfterNET", channel: "#r", sessionID: 77)
        XCTAssertNil(controller.sessionUUID(for: .runtime(id: 77)))
    }

    func testUsersBySessionIsKeyedByUUID() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#a", nick: "alice")
        let uuid = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))!
        XCTAssertEqual(controller.usersBySession[uuid], ["alice"])
    }

    func testChatMessageSessionIDIsUUID() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applyLogLineForTest(network: "Libera", channel: "#a", text: "hello")
        let uuid = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))!
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
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "AfterNET", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#b")

        let aUUID = controller.sessionUUID(for: .composed(network: "AfterNET", channel: "#a"))!
        let bUUID = controller.sessionUUID(for: .composed(network: "AfterNET", channel: "#b"))!
        controller.selectedSessionID = aUUID
        XCTAssertEqual(controller.activeSessionID, aUUID)

        // Put users in #a so we can assert the cleanup.
        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "AfterNET", channel: "#a", nick: "alice")
        XCTAssertFalse(controller.usersBySession[aUUID, default: []].isEmpty)

        controller.applySessionForTest(action: HC_APPLE_SESSION_REMOVE, network: "AfterNET", channel: "#a")

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
            action: HC_APPLE_SESSION_UPSERT, network: "AfterNET", channel: "#same", sessionID: 7)
        let uuid = controller.sessionUUID(for: .runtime(id: 7))!
        controller.selectedSessionID = uuid
        XCTAssertEqual(controller.numericRuntimeSessionID(forSelection: uuid), 7)
    }

    func testActiveSessionIDIsUUID() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        let uuid = controller.sessionUUID(for: .composed(network: "Libera", channel: "#a"))
        XCTAssertEqual(controller.activeSessionID, uuid)
    }

    func testChatSessionIDIsUUID() {
        let session = ChatSession(network: "Libera", channel: "#a", isActive: false)
        let _: UUID = session.id  // compile-time type assertion
        let another = ChatSession(network: "Libera", channel: "#b", isActive: false)
        XCTAssertNotEqual(session.id, another.id, "default UUIDs must be distinct")
    }

    func testUnattributedMessageRegistersSystemSessionLocator() {
        let controller = EngineController()
        controller.appendUnattributedForTest(raw: "! system error", kind: .error)
        XCTAssertNotNil(
            controller.sessionUUID(for: .composed(network: "network", channel: "server")),
            "system session must be registered in sessionByLocator so real events fold into it")
    }
}
#else
@testable import HexChatAppleShell
import AppleAdapterBridge

// XCTest is unavailable in this toolchain; keep reducer coverage code compiled.
func _hexchatAppleShellTestsCompileProbe() {
    let controller = EngineController()
    controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#probe")
    controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#probe", nick: "@alice")
    _ = controller.visibleUsers
}
#endif
