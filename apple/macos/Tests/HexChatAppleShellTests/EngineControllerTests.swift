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

        controller.selectedSessionID = EngineController.sessionID(network: "Libera", channel: "#a")
        XCTAssertEqual(controller.visibleUsers, ["alice"])

        controller.selectedSessionID = EngineController.sessionID(network: "Libera", channel: "#b")
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
        controller.selectedSessionID = EngineController.sessionID(network: "Libera", channel: "#a")

        controller.applyLogLineForTest(network: "Libera", channel: "#b", text: "message for b")

        let sessionA = EngineController.sessionID(network: "Libera", channel: "#a")
        let sessionB = EngineController.sessionID(network: "Libera", channel: "#b")
        XCTAssertFalse(controller.messages.contains(where: { $0.sessionID == sessionA && $0.raw == "message for b" }))
        XCTAssertTrue(controller.messages.contains(where: { $0.sessionID == sessionB && $0.raw == "message for b" }))
    }

    func testRuntimeSessionIDSeparatesSameNetworkChannelLabel() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#same", sessionID: 101)
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#same", sessionID: 202)

        controller.applyLogLineForTest(network: "Libera", channel: "#same", text: "message for 202", sessionID: 202)

        XCTAssertTrue(controller.messages.contains(where: { $0.sessionID == EngineController.runtimeSessionID(202) && $0.raw == "message for 202" }))
        XCTAssertFalse(controller.messages.contains(where: { $0.sessionID == EngineController.runtimeSessionID(101) && $0.raw == "message for 202" }))
    }

    func testCommandEchoFollowsSelectedSessionNotActive() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#a")
        controller.applySessionForTest(action: HC_APPLE_SESSION_UPSERT, network: "Libera", channel: "#b")

        let a = EngineController.sessionID(network: "Libera", channel: "#a")
        let b = EngineController.sessionID(network: "Libera", channel: "#b")

        controller.selectedSessionID = b
        controller.send("/ping", trackHistory: false)

        XCTAssertFalse(controller.messages.contains { $0.sessionID == a && $0.raw == "> /ping" })
        XCTAssertTrue(controller.messages.contains { $0.sessionID == b && $0.raw == "> /ping" })
    }

    func testUserlistSupportsBangPrefixAndDeduplicatesCaseInsensitive() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#ops")

        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_INSERT, network: "Libera", channel: "#ops", nick: "!Alice")
        controller.applyUserlistForTest(action: HC_APPLE_USERLIST_UPDATE, network: "Libera", channel: "#ops", nick: "alice")

        XCTAssertEqual(controller.visibleUsers.count, 1)
        XCTAssertEqual(controller.visibleUsers.first, "alice")
    }

    func testMessageBufferIsBounded() {
        let controller = EngineController()
        controller.applySessionForTest(action: HC_APPLE_SESSION_ACTIVATE, network: "Libera", channel: "#hexchat")

        for idx in 0..<7000 {
            controller.applyLogLineForTest(network: "Libera", channel: "#hexchat", text: "line \(idx)")
        }

        XCTAssertLessThanOrEqual(controller.messages.count, 5000)
        XCTAssertEqual(controller.messages.last?.raw, "line 6999")
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
