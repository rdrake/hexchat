import SwiftUI

struct ContentView: View {
    @Bindable var controller: EngineController
    @Bindable var window: WindowSession
    @SceneStorage("focusedSessionID") private var storedFocus: String = ""

    var body: some View {
        ZStack {
            LinearGradient(
                colors: [Color(red: 0.95, green: 0.96, blue: 0.94), Color(red: 0.90, green: 0.92, blue: 0.89)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            .ignoresSafeArea()

            HStack(spacing: 14) {
                sidebar
                    .frame(minWidth: 240, idealWidth: 260, maxWidth: 300)
                    .padding(10)
                    .background(panelFill)
                    .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))

                chatPane
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .padding(10)
                    .background(panelFill)
                    .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))

                userPane
                    .frame(minWidth: 220, idealWidth: 240, maxWidth: 300)
                    .padding(10)
                    .background(panelFill)
                    .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
            }
            .padding(12)
        }
        .frame(minWidth: 1080, minHeight: 620)
        .focusedSceneValue(\.focusedSessionID, window.focusedSessionID)
        .onAppear {
            if window.focusedSessionID == nil,
                let restored = WindowSession.decode(focused: storedFocus)
            {
                // Only restore if the restored UUID still corresponds to a known
                // session. Otherwise leave focusedSessionID at nil so the controller
                // falls back to its first session.
                if controller.sessions.contains(where: { $0.id == restored }) {
                    window.focusedSessionID = restored
                }
            }
        }
        .onChange(of: window.focusedSessionID) { _, newValue in
            storedFocus = WindowSession.encode(focused: newValue)
        }
    }

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Networks")
                .font(.system(size: 13, weight: .semibold, design: .rounded))
                .foregroundStyle(.secondary)

            List(selection: $window.focusedSessionID) {
                ForEach(controller.networkSections) { section in
                    Section(section.name.uppercased()) {
                        ForEach(section.sessions) { session in
                            HStack(spacing: 8) {
                                Circle()
                                    .fill(session.isActive ? Color.green : Color.gray.opacity(0.5))
                                    .frame(width: 7, height: 7)
                                Image(systemName: session.isChannel ? "number" : "network")
                                    .foregroundStyle(.secondary)
                                Text(session.channel)
                                    .font(.system(.body, design: .monospaced))
                                    .lineLimit(1)
                            }
                            .tag(Optional(session.id))
                            .draggable(session)
                        }
                    }
                }
            }
            .scrollContentBackground(.hidden)
            .listStyle(.inset)
            .dropDestination(for: ChatSession.self) { droppedSessions, _ in
                guard let dropped = droppedSessions.first,
                    controller.sessions.contains(where: { $0.id == dropped.id })
                else { return false }
                window.focusedSessionID = dropped.id
                return true
            }
        }
    }

    private var chatPane: some View {
        VStack(spacing: 12) {
            HStack {
                Text(controller.visibleSessionTitle(for: window.focusedSessionID))
                    .font(.system(size: 18, weight: .semibold, design: .rounded))

                statusChip

                Spacer()

                Button("Start") {
                    controller.start()
                }
                .disabled(controller.isRunning)

                Button("Stop") {
                    controller.stop()
                }
                .disabled(!controller.isRunning)

                Button("Quit") {
                    controller.send("quit")
                }
                .disabled(!controller.isRunning)
            }
            .padding(.horizontal, 6)

            List(controller.visibleMessages(for: window.focusedSessionID)) { message in
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text(messagePrefix(message.kind))
                        .font(.system(size: 10, weight: .bold, design: .monospaced))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(messageColor(message.kind))
                        .clipShape(Capsule())

                    Text(message.raw)
                        .font(.system(.body, design: .monospaced))
                        .textSelection(.enabled)
                }
                .padding(.vertical, 2)
            }
            .scrollContentBackground(.hidden)
            .listStyle(.plain)

            CommandInputView(
                text: controller.draftBinding(for: window.focusedSessionID),
                onSubmit: {
                    let draft = controller.draftBinding(for: window.focusedSessionID)
                    controller.send(draft.wrappedValue, forSession: window.focusedSessionID)
                    draft.wrappedValue = ""
                },
                onHistory: { delta in
                    controller.browseHistory(delta: delta)
                }
            )
            .frame(minHeight: 72, maxHeight: 110)
            .overlay(alignment: .trailing) {
                Button("Send") {
                    let draft = controller.draftBinding(for: window.focusedSessionID)
                    controller.send(draft.wrappedValue, forSession: window.focusedSessionID)
                    draft.wrappedValue = ""
                }
                .buttonStyle(.borderedProminent)
                .tint(Color(red: 0.13, green: 0.37, blue: 0.28))
                .padding(10)
                .disabled(!controller.isRunning || isDraftEmpty)
            }
            .dropDestination(for: ChatUser.self) { droppedUsers, _ in
                guard let dropped = droppedUsers.first,
                    let sessionID = window.focusedSessionID
                else { return false }
                controller.prefillPrivateMessage(to: dropped.nick, forSession: sessionID)
                return true
            }
        }
    }

    private var userPane: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Users (\(controller.visibleUsers(for: window.focusedSessionID).count))")
                .font(.system(size: 17, weight: .semibold, design: .rounded))

            List(controller.visibleUsers(for: window.focusedSessionID)) { user in
                HStack(spacing: 8) {
                    Text(user.modePrefix.map(String.init) ?? "")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .frame(width: 16, alignment: .center)
                    Text(user.nick)
                        .font(.system(.body, design: .monospaced))
                }
                .contentShape(Rectangle())
                .draggable(user)
                .onTapGesture(count: 2) {
                    controller.prefillPrivateMessage(to: user.nick, forSession: window.focusedSessionID)
                }
            }
            .scrollContentBackground(.hidden)
            .listStyle(.inset)
        }
    }

    private var isDraftEmpty: Bool {
        controller.draftBinding(for: window.focusedSessionID).wrappedValue
            .trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private var statusChip: some View {
        Text(controller.isRunning ? "Connected" : "Offline")
            .font(.system(size: 11, weight: .semibold, design: .rounded))
            .foregroundStyle(controller.isRunning ? Color.green : Color.orange)
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background((controller.isRunning ? Color.green : Color.orange).opacity(0.12))
            .clipShape(Capsule())
    }

    private var panelFill: some ShapeStyle {
        Color.white.opacity(0.78)
    }

    private func messagePrefix(_ kind: ChatMessageKind) -> String {
        switch kind {
        case .message: return "MSG"
        case .notice: return "NTC"
        case .action: return "ACT"
        case .join: return "JOIN"
        case .part: return "PART"
        case .quit: return "QUIT"
        case .kick: return "KICK"
        case .nickChange: return "NICK"
        case .modeChange: return "MODE"
        case .command: return "CMD"
        case .error: return "ERR"
        case .lifecycle: return "LIFE"
        }
    }

    private func messageColor(_ kind: ChatMessageKind) -> Color {
        switch kind {
        case .error: return Color(red: 0.72, green: 0.19, blue: 0.16)
        case .join: return Color(red: 0.17, green: 0.55, blue: 0.25)
        case .part, .quit, .kick, .nickChange, .modeChange: return Color(red: 0.82, green: 0.46, blue: 0.14)
        case .command: return Color(red: 0.18, green: 0.42, blue: 0.66)
        case .notice, .lifecycle, .action: return Color(red: 0.44, green: 0.46, blue: 0.49)
        case .message: return Color(red: 0.30, green: 0.33, blue: 0.36)
        }
    }
}
