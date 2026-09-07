import AppKit
import Darwin

// Same underlying clock as Python time.monotonic_ns on macOS.
func monotonicMilliseconds() -> UInt64 {
    var base = mach_timebase_info_data_t()
    mach_timebase_info(&base)
    let ticks = mach_absolute_time()
    return (ticks / UInt64(base.denom) * UInt64(base.numer)
        + ticks % UInt64(base.denom) * UInt64(base.numer) / UInt64(base.denom)) / 1_000_000
}

func crc16(_ data: [UInt8]) -> UInt16 {
    var crc: UInt16 = 0xffff
    for byte in data {
        crc ^= UInt16(byte) << 8
        for _ in 0..<8 { crc = (crc & 0x8000 != 0) ? (crc << 1) ^ 0x1021 : crc << 1 }
    }
    return crc
}

final class MarkerView: NSView {
    override var isFlipped: Bool { true }
    var sequence: UInt16 = 0
    override func draw(_ dirtyRect: NSRect) {
        let ms = UInt32(truncatingIfNeeded: monotonicMilliseconds())
        sequence &+= 1
        NSColor(calibratedWhite: 0.08, alpha: 1).setFill()
        bounds.fill()
        var payload: [UInt8] = [0x4d, 0x4c,
            UInt8((ms >> 24) & 255), UInt8((ms >> 16) & 255),
            UInt8((ms >> 8) & 255), UInt8(ms & 255),
            UInt8(sequence >> 8), UInt8(sequence & 255)]
        let crc = crc16(payload)
        payload += [UInt8(crc >> 8), UInt8(crc & 255)]
        let marker = NSRect(x: bounds.width * 0.08, y: bounds.height * 0.17,
                            width: bounds.width * 0.84, height: bounds.height * 0.66)
        NSColor.white.setFill()
        marker.fill()
        let grid = marker.insetBy(dx: marker.width * 0.05, dy: marker.height * 0.05)
        NSColor.black.setFill()
        grid.fill()
        NSGraphicsContext.current?.shouldAntialias = false
        for bit in 0..<80 {
            if payload[bit / 8] & (1 << (7 - bit % 8)) != 0 {
                NSColor.white.setFill()
                NSRect(x: grid.minX + CGFloat(bit % 10) * grid.width / 10,
                       y: grid.minY + CGFloat(bit / 10) * grid.height / 8,
                       width: grid.width / 10, height: grid.height / 8).fill()
            }
        }
        let style: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: bounds.height * 0.035, weight: .medium),
            .foregroundColor: NSColor(calibratedWhite: 0.65, alpha: 1)]
        ("Moonlight delay test  ·  Esc to close" as NSString).draw(
            at: NSPoint(x: bounds.width * 0.08, y: bounds.height * 0.07), withAttributes: style)
        ("Frame \(sequence)   Source clock \(ms) ms" as NSString).draw(
            at: NSPoint(x: bounds.width * 0.08, y: bounds.height * 0.9), withAttributes: style)
        // Reference colors establish the capture's byte order.
        NSColor.red.setFill()
        NSRect(x: 10, y: 10, width: 40, height: 40).fill()
        NSColor.blue.setFill()
        NSRect(x: 60, y: 10, width: 40, height: 40).fill()
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!
    var timer: Timer?
    func applicationDidFinishLaunching(_ notification: Notification) {
        let frame = NSScreen.main!.frame
        window = NSWindow(contentRect: frame, styleMask: [.borderless], backing: .buffered, defer: false)
        let marker = MarkerView(frame: NSRect(origin: .zero, size: frame.size))
        window.contentView = marker
        window.backgroundColor = .black
        window.level = .normal
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
            if event.keyCode == 53 { NSApp.terminate(nil); return nil }
            return event
        }
        timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { _ in
            marker.needsDisplay = true
        }
        RunLoop.main.add(timer!, forMode: .common)
        let duration = Double(CommandLine.arguments.dropFirst().first ?? "25") ?? 25
        Timer.scheduledTimer(withTimeInterval: duration, repeats: false) { _ in NSApp.terminate(nil) }
    }
}

if CommandLine.arguments.contains("--clock") {
    print(monotonicMilliseconds())
} else {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    app.setActivationPolicy(.regular)
    app.run()
}
