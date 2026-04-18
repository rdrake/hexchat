import SwiftUI
import AppKit

struct CommandInputView: NSViewRepresentable {
    @Binding var text: String
    var onSubmit: () -> Void
    var onHistory: (Int) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(self)
    }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSScrollView()
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.borderType = .lineBorder

        let textView = HistoryTextView()
        textView.isRichText = false
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticDataDetectionEnabled = false
        textView.font = NSFont.monospacedSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
        textView.drawsBackground = true
        textView.backgroundColor = NSColor.windowBackgroundColor.withAlphaComponent(0.88)
        textView.textContainerInset = NSSize(width: 8, height: 8)
        textView.delegate = context.coordinator
        textView.historyDelegate = context.coordinator

        scroll.documentView = textView
        return scroll
    }

    func updateNSView(_ nsView: NSScrollView, context: Context) {
        guard let textView = nsView.documentView as? HistoryTextView else {
            return
        }
        if textView.string != text {
            textView.string = text
        }
    }

    final class Coordinator: NSObject, NSTextViewDelegate, HistoryTextViewDelegate {
        var parent: CommandInputView

        init(_ parent: CommandInputView) {
            self.parent = parent
        }

        func textDidChange(_ notification: Notification) {
            guard let textView = notification.object as? NSTextView else {
                return
            }
            parent.text = textView.string
        }

        func historyTextViewSubmit(_ textView: HistoryTextView) {
            parent.onSubmit()
        }

        func historyTextView(_ textView: HistoryTextView, browseHistory delta: Int) {
            parent.onHistory(delta)
        }
    }
}

protocol HistoryTextViewDelegate: AnyObject {
    func historyTextViewSubmit(_ textView: HistoryTextView)
    func historyTextView(_ textView: HistoryTextView, browseHistory delta: Int)
}

final class HistoryTextView: NSTextView {
    weak var historyDelegate: HistoryTextViewDelegate?

    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 36, 76:
            if event.modifierFlags.contains(.shift) {
                super.keyDown(with: event)
            } else {
                historyDelegate?.historyTextViewSubmit(self)
            }
        case 126:
            if string.contains("\n") {
                super.keyDown(with: event)
            } else {
                historyDelegate?.historyTextView(self, browseHistory: -1)
            }
        case 125:
            if string.contains("\n") {
                super.keyDown(with: event)
            } else {
                historyDelegate?.historyTextView(self, browseHistory: 1)
            }
        default:
            super.keyDown(with: event)
        }
    }
}
