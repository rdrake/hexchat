//
//  ContentView.swift
//  HexChatApple
//
//  Created by Richard Drake on 2026-04-22.
//

import SwiftUI

struct ContentView: View {
    @Bindable var controller: BasicRuntimeController

    var body: some View {
        VStack(spacing: 12) {
            HStack(spacing: 8) {
                Button("Start") { controller.start() }
                    .disabled(controller.isRunning)

                Button("Stop") { controller.stop() }
                    .disabled(!controller.isRunning)

                Spacer()
            }

            ScrollViewReader { proxy in
                List(Array(controller.logs.enumerated()), id: \.offset) { index, line in
                    Text(line)
                        .font(.system(.body, design: .monospaced))
                        .textSelection(.enabled)
                        .id(index)
                }
                .onChange(of: controller.logs.count) { _, _ in
                    guard let last = controller.logs.indices.last else { return }
                    proxy.scrollTo(last, anchor: .bottom)
                }
            }

            HStack(spacing: 8) {
                TextField("Type command (e.g. /server irc.libera.chat)", text: $controller.commandInput)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { controller.sendCurrentCommand() }

                Button("Send") { controller.sendCurrentCommand() }
                    .disabled(
                        !controller.isRunning
                            || controller.commandInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                    )
            }
        }
        .padding(16)
        .frame(minWidth: 760, minHeight: 480)
    }
}

#Preview {
    ContentView(controller: BasicRuntimeController())
}
