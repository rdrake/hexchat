@testable import HexChat
import AppleAdapterBridge
import Testing

struct HexChatAppleTests {

    @MainActor
    @Test func startFailureAppendsErrorAndStaysStopped() {
        let runtime = MockRuntimeClient(startResult: false)
        let controller = BasicRuntimeController(runtime: runtime)

        controller.start()

        #expect(runtime.startCalls == 1)
        #expect(runtime.lastStartNoAuto == true)
        #expect(runtime.lastStartSkipPlugins == true)
        #expect(controller.isRunning == false)
        #expect(controller.logs == ["! runtime start failed"])
    }

    @MainActor
    @Test func successfulSendEchoesPostsAndClearsInput() async {
        let runtime = MockRuntimeClient(startResult: true, emitReadyOnStart: true)
        let controller = BasicRuntimeController(runtime: runtime)

        controller.start()
        await Task.yield()

        controller.commandInput = "  /join #hexchat  "
        controller.sendCurrentCommand()

        #expect(controller.isRunning == true)
        #expect(runtime.postedCommands == ["/join #hexchat"])
        #expect(controller.logs == ["> /join #hexchat"])
        #expect(controller.commandInput.isEmpty)
    }

    @MainActor
    @Test func sendFailureAppendsFailureLine() async {
        let runtime = MockRuntimeClient(startResult: true, emitReadyOnStart: true, postResult: false)
        let controller = BasicRuntimeController(runtime: runtime)

        controller.start()
        await Task.yield()

        controller.commandInput = "/join #hexchat"
        controller.sendCurrentCommand()

        #expect(runtime.postedCommands == ["/join #hexchat"])
        #expect(controller.logs == ["! failed to send command"])
        #expect(controller.commandInput == "/join #hexchat")
    }

    @MainActor
    @Test func whitespaceCommandIsIgnored() async {
        let runtime = MockRuntimeClient(startResult: true, emitReadyOnStart: true)
        let controller = BasicRuntimeController(runtime: runtime)

        controller.start()
        await Task.yield()

        controller.commandInput = "  \n\t  "
        controller.sendCurrentCommand()

        #expect(runtime.postedCommands.isEmpty)
        #expect(controller.logs.isEmpty)
        #expect(controller.commandInput == "  \n\t  ")
    }

    @MainActor
    @Test func logCapTrimsOldestEntries() {
        let controller = BasicRuntimeController(runtime: MockRuntimeClient(), logCap: 2)

        controller.appendLog("one")
        controller.appendLog("two")
        controller.appendLog("three")

        #expect(controller.logs == ["two", "three"])
    }
}

final class MockRuntimeClient: RuntimeClient {
    var startResult: Bool
    var postResult: Bool
    var emitReadyOnStart: Bool

    private(set) var startCalls = 0
    private(set) var lastStartNoAuto: Bool?
    private(set) var lastStartSkipPlugins: Bool?
    private(set) var postedCommands: [String] = []
    private(set) var stopCalls = 0

    private var onEvent: ((RuntimeEvent) -> Void)?

    init(startResult: Bool = true, emitReadyOnStart: Bool = false, postResult: Bool = true) {
        self.startResult = startResult
        self.emitReadyOnStart = emitReadyOnStart
        self.postResult = postResult
    }

    func start(noAuto: Bool, skipPlugins: Bool, onEvent: @escaping (RuntimeEvent) -> Void) -> Bool {
        startCalls += 1
        lastStartNoAuto = noAuto
        lastStartSkipPlugins = skipPlugins
        self.onEvent = onEvent
        if startResult, emitReadyOnStart {
            onEvent(RuntimeEvent(kind: HC_APPLE_EVENT_LIFECYCLE, text: nil, phase: HC_APPLE_LIFECYCLE_READY))
        }
        return startResult
    }

    func stop() {
        stopCalls += 1
        onEvent?(RuntimeEvent(kind: HC_APPLE_EVENT_LIFECYCLE, text: nil, phase: HC_APPLE_LIFECYCLE_STOPPED))
    }

    func postCommand(_ command: String) -> Bool {
        postedCommands.append(command)
        return postResult
    }
}
