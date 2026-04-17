import SwiftUI

struct ContentView: View {
    @Bindable var controller: EngineController

    var body: some View {
        VStack(spacing: 12) {
            HStack {
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

                Button("Send") {
                    controller.send(controller.input)
                    controller.input = ""
                }
                .disabled(!controller.isRunning || controller.input.isEmpty)
            }

            TextField("Command", text: $controller.input)
                .textFieldStyle(.roundedBorder)

            List(controller.logs, id: \.self) { line in
                Text(line)
                    .font(.system(.body, design: .monospaced))
            }
        }
        .padding()
        .frame(minWidth: 720, minHeight: 420)
    }
}
