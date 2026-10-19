// SPDX-License-Identifier: MIT

import DiskArbitration
import Foundation
import IOKit

/// A mounted btrfs volume (served by the FSKit module or macFUSE).
struct MountedVolume: Identifiable, Hashable {
    let mountPoint: String
    let device: String      // "/dev/disk7" or the FUSE fsname
    let viaFUSE: Bool

    var id: String { mountPoint }
    var name: String { (mountPoint as NSString).lastPathComponent }
}

/// A disk or partition that may hold btrfs but is not mounted.
struct CandidateDisk: Identifiable, Hashable {
    let bsdName: String     // "disk7s2"
    let content: String     // partition type / content hint, "" for raw
    let size: UInt64
    let isImage: Bool

    var id: String { bsdName }
}

enum DiskError: LocalizedError {
    case failed(String)

    var errorDescription: String? {
        switch self {
        case .failed(let message): return message
        }
    }
}

/// Finding, attaching, mounting and ejecting btrfs disks. Mounting itself is
/// done by DiskArbitration, which hands btrfs to the FSKit module.
final class DiskManager {
    /// Partition types that may contain btrfs
    static let linuxContentHints: Set<String> = [
        "0FC63DAF-8483-4772-8E79-3D69D8477DE4",  // GPT Linux filesystem data
        "Linux",                                 // MBR type 0x83
    ]

    private let session: DASession

    init() {
        session = DASessionCreate(kCFAllocatorDefault)!
        DASessionSetDispatchQueue(session, DispatchQueue.main)
    }

    // MARK: Mounted volumes

    func mountedVolumes() -> [MountedVolume] {
        var mounts: UnsafeMutablePointer<statfs>?
        let count = getmntinfo(&mounts, MNT_NOWAIT)
        guard count > 0, let mounts else { return [] }

        return (0..<Int(count)).compactMap { i -> MountedVolume? in
            var fs = mounts[i]
            let type = cString(&fs.f_fstypename)
            let from = cString(&fs.f_mntfromname)
            let on = cString(&fs.f_mntonname)
            if type == "btrfs" {
                return MountedVolume(mountPoint: on, device: from, viaFUSE: false)
            }
            if type == "macfuse" && from.hasPrefix("btrfs:") {
                return MountedVolume(mountPoint: on, device: String(from.dropFirst(6)), viaFUSE: true)
            }
            return nil
        }
    }

    private func cString<T>(_ tuple: inout T) -> String {
        withUnsafeBytes(of: &tuple) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
    }

    // MARK: Unmounted candidates

    /// Leaf media with a Linux partition type, or whole raw disks without a
    /// partition map (e.g. images attached without mounting), that are not
    /// mounted.
    func candidateDisks() -> [CandidateDisk] {
        var iterator: io_iterator_t = 0
        guard let matching = IOServiceMatching("IOMedia") else { return [] }
        (matching as NSMutableDictionary)["Leaf"] = true
        guard IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) == KERN_SUCCESS else {
            return []
        }
        defer { IOObjectRelease(iterator) }

        var result: [CandidateDisk] = []
        while case let media = IOIteratorNext(iterator), media != 0 {
            defer { IOObjectRelease(media) }
            guard let bsd = property(media, "BSD Name") as? String else { continue }
            let content = property(media, "Content") as? String ?? ""
            let whole = property(media, "Whole") as? Bool ?? false
            let size = (property(media, "Size") as? NSNumber)?.uint64Value ?? 0

            guard Self.linuxContentHints.contains(content) || (whole && content.isEmpty) else {
                continue
            }
            guard let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd),
                  let desc = DADiskCopyDescription(disk) as? [CFString: Any] else { continue }
            if desc[kDADiskDescriptionVolumePathKey] != nil {
                continue  // already mounted
            }
            let model = desc[kDADiskDescriptionDeviceModelKey] as? String ?? ""
            result.append(CandidateDisk(bsdName: bsd, content: content, size: size,
                                        isImage: model.contains("Disk Image")))
        }
        return result.sorted { $0.bsdName < $1.bsdName }
    }

    private func property(_ entry: io_registry_entry_t, _ key: String) -> Any? {
        IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue()
    }

    // MARK: Actions

    /// Attach an image file read-only; DiskArbitration then mounts it.
    /// Returns the whole disk device, e.g. "/dev/disk9".
    @discardableResult
    func attachImage(_ url: URL, mount: Bool = true) async throws -> String {
        var args = ["attach", "-readonly", "-plist", "-imagekey", "diskimage-class=CRawDiskImage"]
        if !mount {
            args.append("-nomount")
        }
        args.append(url.path)
        let output = try await run("/usr/bin/hdiutil", args)
        if output.status != 0 {
            var message = output.text.trimmingCharacters(in: .whitespacesAndNewlines)
            if message.contains("no mountable file systems") {
                message += "\n\nIs it a btrfs image, and is \"btrfs\" enabled in File System Extensions?"
            }
            throw DiskError.failed(message.isEmpty ? "hdiutil failed" : message)
        }
        // The plist lists the attached entities; the shortest dev-entry is the whole disk
        guard let start = output.text.range(of: "<?xml"),
              let plist = try? PropertyListSerialization.propertyList(
                  from: Data(output.text[start.lowerBound...].utf8), format: nil) as? [String: Any],
              let entities = plist["system-entities"] as? [[String: Any]],
              let device = entities.compactMap({ $0["dev-entry"] as? String })
                  .min(by: { $0.count < $1.count }) else {
            throw DiskError.failed("hdiutil did not report the attached device")
        }
        return device
    }

    /// Ask DiskArbitration to mount a disk (it probes the FSKit modules).
    func mount(bsdName: String) async throws {
        guard let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsdName) else {
            throw DiskError.failed("\(bsdName) not found")
        }
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            let box = Unmanaged.passRetained(ContinuationBox(cont)).toOpaque()
            // Read-only, like the file system itself; NULL terminated argument list
            let rdonly = Unmanaged.passRetained("rdonly" as CFString)
            let args = UnsafeMutablePointer<Unmanaged<CFString>?>.allocate(capacity: 2)
            args[0] = rdonly
            args[1] = nil
            DADiskMountWithArguments(disk, nil, DADiskMountOptions(kDADiskMountOptionDefault), { _, dissenter, ctx in
                Unmanaged<ContinuationBox>.fromOpaque(ctx!).takeRetainedValue()
                    .resume(dissenter, action: "mount")
            }, box, args)
            args.deallocate()
            rdonly.release()
        }
    }

    /// Unmount a volume and eject the disk it is on (detaches images).
    func eject(_ volume: MountedVolume) async throws {
        if volume.viaFUSE {
            let output = try await run("/sbin/umount", [volume.mountPoint])
            if output.status != 0 {
                throw DiskError.failed(output.text)
            }
            return
        }
        let bsd = (volume.device as NSString).lastPathComponent
        guard let disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, bsd) else {
            throw DiskError.failed("\(volume.device) not found")
        }
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            let box = Unmanaged.passRetained(ContinuationBox(cont)).toOpaque()
            DADiskUnmount(disk, DADiskUnmountOptions(kDADiskUnmountOptionDefault), { _, dissenter, ctx in
                Unmanaged<ContinuationBox>.fromOpaque(ctx!).takeRetainedValue()
                    .resume(dissenter, action: "unmount")
            }, box)
        }
        let whole = DADiskCopyWholeDisk(disk) ?? disk
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            let box = Unmanaged.passRetained(ContinuationBox(cont)).toOpaque()
            DADiskEject(whole, DADiskEjectOptions(kDADiskEjectOptionDefault), { _, dissenter, ctx in
                Unmanaged<ContinuationBox>.fromOpaque(ctx!).takeRetainedValue()
                    .resume(dissenter, action: "eject")
            }, box)
        }
    }

    // MARK: Helpers

    private final class ContinuationBox {
        let cont: CheckedContinuation<Void, Error>

        init(_ cont: CheckedContinuation<Void, Error>) {
            self.cont = cont
        }

        func resume(_ dissenter: DADissenter?, action: String) {
            guard let dissenter else {
                cont.resume()
                return
            }
            let status = DADissenterGetStatus(dissenter)
            var text = DADissenterGetStatusString(dissenter).map { $0 as String } ?? ""
            if text.isEmpty {
                text = DiskManager.describe(status)
            }
            let hex = String(UInt32(bitPattern: status), radix: 16)
            cont.resume(throwing: DiskError.failed("\(action) failed: \(text) (status 0x\(hex))"))
        }
    }

    /// Text for a DiskArbitration status code (kDAReturn* or a Unix errno).
    static func describe(_ status: DAReturn) -> String {
        let code = UInt32(bitPattern: status)
        switch code {
        case 0xF8DA0001: return "error"
        case 0xF8DA0002: return "the disk is busy (a program is using it)"
        case 0xF8DA0003: return "bad argument"
        case 0xF8DA0004: return "the disk is in exclusive use"
        case 0xF8DA0005: return "no resources"
        case 0xF8DA0006: return "not found"
        case 0xF8DA0007: return "not mounted"
        case 0xF8DA0008: return "not permitted"
        case 0xF8DA0009: return "not privileged"
        case 0xF8DA000A: return "not ready"
        case 0xF8DA000B: return "not writable"
        case 0xF8DA000C: return "no supported file system found (not btrfs, or the btrfs extension is not enabled)"
        default:
            break
        }
        // A plain errno, e.g. 45 (ENOTSUP) when the disk check failed
        if code > 0 && code < 0x100 {
            var text = String(cString: strerror(Int32(code)))
            if code == UInt32(ENOTSUP) {
                text += " (the btrfs module could not check the disk; a newer module fixes this)"
            }
            return text
        }
        if let text = mach_error_string(mach_error_t(status)) {
            return String(cString: text)
        }
        return "unknown error"
    }

    private func run(_ tool: String, _ args: [String]) async throws -> (status: Int32, text: String) {
        try await withCheckedThrowingContinuation { cont in
            let process = Process()
            let pipe = Pipe()
            process.executableURL = URL(fileURLWithPath: tool)
            process.arguments = args
            process.standardOutput = pipe
            process.standardError = pipe
            process.terminationHandler = { p in
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                cont.resume(returning: (p.terminationStatus, String(decoding: data, as: UTF8.self)))
            }
            do {
                try process.run()
            } catch {
                cont.resume(throwing: error)
            }
        }
    }
}
