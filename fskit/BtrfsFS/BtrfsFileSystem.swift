// SPDX-License-Identifier: MIT

import FSKit

/// The FSKit module: probes block devices for btrfs and loads volumes.
final class BtrfsFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations {

    override init() {
        _ = installLogHandler
        super.init()
    }

    /// Volume name: the btrfs label, or "btrfs" for unlabeled filesystems.
    private static func label(of info: btrfsro_volinfo) -> String {
        var info = info
        let label = withUnsafeBytes(of: &info.label) { raw in
            String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
        return label.isEmpty ? "btrfs" : label
    }

    private static func uuid(of info: btrfsro_volinfo) -> UUID {
        UUID(uuid: info.fsid)
    }

    func probeResource(resource: FSResource,
                       replyHandler: @escaping (FSProbeResult?, Error?) -> Void) {
        guard let block = resource as? FSBlockDeviceResource else {
            replyHandler(.notRecognized, nil)
            return
        }
        let device = BlockDevice(resource: block)
        var io = device.makeIO()
        var info = btrfsro_volinfo()
        let ret = btrfsro_probe(&io, &info)
        withExtendedLifetime(device) {}

        switch ret {
        case 0:
            let name = Self.label(of: info)
            let container = FSContainerIdentifier(uuid: Self.uuid(of: info))
            if info.num_devices != 1 {
                replyHandler(.recognized(name: name, containerID: container), nil)
            } else {
                replyHandler(.usable(name: name, containerID: container), nil)
            }
        case -EOPNOTSUPP:
            // A btrfs we cannot read (unsupported features)
            let name = Self.label(of: info)
            replyHandler(.recognized(name: name,
                                     containerID: FSContainerIdentifier(uuid: Self.uuid(of: info))),
                         nil)
        default:
            replyHandler(.notRecognized, nil)
        }
    }

    func loadResource(resource: FSResource, options: FSTaskOptions,
                      replyHandler: @escaping (FSVolume?, Error?) -> Void) {
        guard let block = resource as? FSBlockDeviceResource else {
            replyHandler(nil, fs_errorForPOSIXError(ENODEV))
            return
        }
        let device = BlockDevice(resource: block)
        var io = device.makeIO()
        var fs: OpaquePointer?
        let ret = btrfsro_mount(&io, &fs)
        guard ret == 0, let fs else {
            log.error("mount of \(block.bsdName, privacy: .public) failed: \(ret)")
            replyHandler(nil, posixError(ret))
            return
        }
        var info = btrfsro_volinfo()
        btrfsro_get_volinfo(fs, &info)
        let label = Self.label(of: info)
        let volume = BtrfsVolume(device: device, fs: fs,
                                 volumeID: FSVolume.Identifier(uuid: Self.uuid(of: info)),
                                 name: label,
                                 info: info)
        log.info("loaded btrfs \(label, privacy: .public) from \(block.bsdName, privacy: .public)")
        containerStatus = .ready
        replyHandler(volume, nil)
    }

    func unloadResource(resource: FSResource, options: FSTaskOptions,
                        replyHandler: @escaping (Error?) -> Void) {
        replyHandler(nil)
    }
}
