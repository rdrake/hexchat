//
//  HexChatAppleApp.swift
//  HexChatApple
//
//  Created by Richard Drake on 2026-04-22.
//

import SwiftUI

@main
struct HexChatAppleApp: App {
    @State private var controller = BasicRuntimeController()

    var body: some Scene {
        WindowGroup {
            ContentView(controller: controller)
        }
    }
}
