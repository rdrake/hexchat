//
//  HexChatAppleApp.swift
//  HexChatApple
//
//  Created by Richard Drake on 2026-04-22.
//

import AppKit
import SwiftUI

@main
struct HexChatAppleApp: App {
    @State private var controller = BasicRuntimeController()

    var body: some Scene {
        WindowGroup {
            ContentView(controller: controller)
                .task { controller.start() }
                .onReceive(
                    NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)
                ) { _ in
                    controller.stop()
                }
        }
    }
}
