// SPDX-License-Identifier: MIT

import FSKit
import SwiftUI

/// Host application. Its only job is to carry the FSKit extension and to
/// tell the user how to enable and use it.
@main
struct BtrfsMacApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
                .frame(minWidth: 520, minHeight: 360)
        }
        .windowResizability(.contentSize)
    }
}

struct ContentView: View {
    @State private var status = "Checking…"

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("btrfs for macOS")
                .font(.title)
            Text("Read-only btrfs file system extension (FSKit).")
                .foregroundStyle(.secondary)
            Divider()
            Text("Extension status: \(status)")
            Text("""
                1. Enable "btrfs" in System Settings → General → Login Items & \
                Extensions → File System Extensions.
                2. btrfs disks are then mounted automatically. A disk image can be \
                attached and mounted from Terminal:

                   hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage image.img
                   mkdir /tmp/btrfs && mount -F -t btrfs -o rdonly diskN /tmp/btrfs
                """)
                .font(.callout)
                .textSelection(.enabled)
            Spacer()
            HStack {
                Spacer()
                Button("Refresh") { Task { await refresh() } }
            }
        }
        .padding(24)
        .task { await refresh() }
    }

    private func refresh() async {
        do {
            let modules = try await FSClient.shared.installedExtensions
            if let ours = modules.first(where: { $0.bundleIdentifier.hasSuffix(".fsmodule") &&
                                                 $0.bundleIdentifier.contains("btrfs4mac") }) {
                status = ours.isEnabled ? "installed and enabled" : "installed, not enabled"
            } else {
                status = "not found"
            }
        } catch {
            status = "unknown (\(error.localizedDescription))"
        }
    }
}
