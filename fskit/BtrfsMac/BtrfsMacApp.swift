// SPDX-License-Identifier: MIT

import AppKit
import FSKit
import SwiftUI
import UniformTypeIdentifiers

/// Host application of the btrfs FSKit module. It attaches btrfs images,
/// mounts unmounted btrfs disks and lists and ejects mounted btrfs volumes;
/// the file system itself is served by the module, also when the app does
/// not run.
@main
struct BtrfsMacApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = AppModel.shared

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 560, minHeight: 440)
        }
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Mount Image…") { model.chooseImage() }
                    .keyboardShortcut("o")
            }
        }
    }
}

/// Receives files opened with Finder's "Open With" or dropped on the icon.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func application(_ application: NSApplication, open urls: [URL]) {
        Task { @MainActor in
            for url in urls {
                await AppModel.shared.attach(url)
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

@MainActor
final class AppModel: ObservableObject {
    static let shared = AppModel()

    @Published var volumes: [MountedVolume] = []
    @Published var candidates: [CandidateDisk] = []
    @Published var busy = false
    @Published var message: String?
    @Published var moduleReported: Bool?

    private let disks = DiskManager()
    private var observers: [NSObjectProtocol] = []

    private init() {
        let center = NSWorkspace.shared.notificationCenter
        for name in [NSWorkspace.didMountNotification, NSWorkspace.didUnmountNotification] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { _ in
                Task { @MainActor in AppModel.shared.refresh() }
            })
        }
        refresh()
        Task { await checkModule() }
    }

    func refresh() {
        volumes = disks.mountedVolumes()
        candidates = disks.candidateDisks()
    }

    /// macOS 15 does not list third-party modules here, so "not reported"
    /// is not an error by itself.
    func checkModule() async {
        let modules = (try? await FSClient.shared.installedExtensions) ?? []
        moduleReported = modules.contains { $0.bundleIdentifier.hasSuffix("btrfs4mac.fsmodule") }
    }

    func chooseImage() {
        let panel = NSOpenPanel()
        panel.title = "Mount btrfs Image"
        panel.message = "Choose a btrfs image file to attach read-only"
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            Task { await attach(url) }
        }
    }

    func attach(_ url: URL) async {
        await perform("Attaching \(url.lastPathComponent)…") {
            try await self.disks.attachImage(url)
        }
    }

    func mount(_ disk: CandidateDisk) async {
        await perform("Mounting \(disk.bsdName)…") {
            try await self.disks.mount(bsdName: disk.bsdName)
        }
    }

    func eject(_ volume: MountedVolume) async {
        await perform("Ejecting \(volume.name)…") {
            try await self.disks.eject(volume)
        }
    }

    private func perform(_ status: String, _ action: @escaping () async throws -> Void) async {
        busy = true
        message = status
        do {
            try await action()
            message = nil
        } catch {
            message = error.localizedDescription
        }
        busy = false
        // DiskArbitration mounts asynchronously; look again a bit later
        refresh()
        try? await Task.sleep(for: .seconds(2))
        refresh()
    }

    func openExtensionSettings() {
        let urls = ["x-apple.systempreferences:com.apple.ExtensionsPreferences",
                    "x-apple.systempreferences:com.apple.LoginItems-Settings.extension"]
        for string in urls {
            if let url = URL(string: string), NSWorkspace.shared.open(url) {
                return
            }
        }
        NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Applications/System Settings.app"))
    }
}

struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @State private var dropTargeted = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            header
            Divider()
            volumesSection
            candidatesSection
            Spacer(minLength: 0)
            footer
        }
        .padding(20)
        .overlay {
            if dropTargeted {
                RoundedRectangle(cornerRadius: 12)
                    .stroke(Color.accentColor, lineWidth: 3)
                    .padding(4)
            }
        }
        .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
            for provider in providers {
                _ = provider.loadObject(ofClass: URL.self) { url, _ in
                    if let url {
                        Task { await model.attach(url) }
                    }
                }
            }
            return true
        }
    }

    private var header: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 4) {
                Text("btrfs for macOS")
                    .font(.title2.bold())
                Text("Read-only btrfs file system. Drop an image here, or:")
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button("Mount Image…") { model.chooseImage() }
                .keyboardShortcut("o")
                .disabled(model.busy)
        }
    }

    private var volumesSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Mounted btrfs volumes").font(.headline)
            if model.volumes.isEmpty {
                Text("None").foregroundStyle(.secondary)
            }
            ForEach(model.volumes) { volume in
                HStack {
                    Image(systemName: "externaldrive")
                    VStack(alignment: .leading) {
                        Text(volume.name)
                        Text("\(volume.mountPoint) — \(volume.device)\(volume.viaFUSE ? " (macFUSE)" : "")")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    }
                    Spacer()
                    Button("Open in Finder") {
                        // Open the volume itself, not its parent with it selected
                        NSWorkspace.shared.open(URL(fileURLWithPath: volume.mountPoint,
                                                    isDirectory: true))
                    }
                    Button("Eject") { Task { await model.eject(volume) } }
                        .disabled(model.busy)
                }
            }
        }
    }

    @ViewBuilder
    private var candidatesSection: some View {
        if !model.candidates.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                Text("Linux partitions and raw disks, not mounted").font(.headline)
                ForEach(model.candidates) { disk in
                    HStack {
                        Image(systemName: disk.isImage ? "doc" : "internaldrive")
                        Text(disk.bsdName)
                        Text(ByteCountFormatter.string(fromByteCount: Int64(disk.size),
                                                       countStyle: .file))
                            .foregroundStyle(.secondary)
                        if !disk.content.isEmpty {
                            Text(disk.content == "Linux" ? "Linux" : "Linux filesystem")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("Mount") { Task { await model.mount(disk) } }
                            .disabled(model.busy)
                    }
                }
            }
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let message = model.message {
                HStack(alignment: .top) {
                    if model.busy {
                        ProgressView().controlSize(.small)
                    } else {
                        Image(systemName: "exclamationmark.triangle")
                            .foregroundStyle(.orange)
                    }
                    Text(message)
                        .textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Divider()
            HStack {
                Text(moduleStatus)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Button("Extension Settings…") { model.openExtensionSettings() }
                Button("Refresh") { model.refresh() }
            }
        }
    }

    private var moduleStatus: String {
        switch model.moduleReported {
        case .some(true):
            return "FSKit module: installed"
        case .some(false):
            return "FSKit module: not reported by macOS (normal on macOS 15); " +
                "if mounting fails, enable \"btrfs\" in Extension Settings"
        case .none:
            return "FSKit module: checking…"
        }
    }
}
