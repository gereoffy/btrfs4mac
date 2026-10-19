// SPDX-License-Identifier: MIT

import FSKit
import os

let log = Logger(subsystem: "io.github.gereoffy.btrfs4mac", category: "fsmodule")

/// FSKit error for a POSIX errno value (positive or negative).
func posixError(_ code: Int32) -> Error {
    fs_errorForPOSIXError(code < 0 ? -code : code)
}

/// Route libbtrfsro messages to the unified log, once per process.
let installLogHandler: Void = {
    btrfsro_set_log_handler({ _, level, msg in
        let text = msg.map { String(cString: $0) } ?? ""
        switch level {
        case Int32(BTRFSRO_LOG_ERROR):
            log.error("\(text, privacy: .public)")
        case Int32(BTRFSRO_LOG_WARNING):
            log.warning("\(text, privacy: .public)")
        default:
            log.info("\(text, privacy: .public)")
        }
    }, nil, Int32(BTRFSRO_LOG_INFO))
}()

/// Adapts an FSKit block device to the libbtrfsro read callback.
///
/// The library may ask for any offset and length; the device is only read in
/// whole blocks, using a bounce buffer for unaligned requests.
final class BlockDevice {
    let resource: FSBlockDeviceResource
    let blockSize: UInt64
    let size: UInt64

    init(resource: FSBlockDeviceResource) {
        self.resource = resource
        self.blockSize = max(resource.blockSize, 512)
        self.size = resource.blockSize * resource.blockCount
    }

    /// The libbtrfsro I/O descriptor. `self` must outlive any filesystem
    /// mounted with it.
    func makeIO() -> btrfsro_io {
        var io = btrfsro_io()
        io.read = { ctx, buf, offset, len in
            guard let ctx, let buf else { return -EINVAL }
            let dev = Unmanaged<BlockDevice>.fromOpaque(ctx).takeUnretainedValue()
            return dev.read(into: buf, offset: offset, length: len)
        }
        io.ctx = Unmanaged.passUnretained(self).toOpaque()
        io.size = size
        return io
    }

    func read(into buf: UnsafeMutableRawPointer, offset: UInt64, length: Int) -> Int32 {
        let len = UInt64(length)
        if offset % blockSize == 0 && len % blockSize == 0 {
            return readBlocks(into: buf, offset: offset, length: length)
        }
        let start = offset / blockSize * blockSize
        let end = (offset + len + blockSize - 1) / blockSize * blockSize
        let bounceLen = Int(end - start)
        let bounce = UnsafeMutableRawPointer.allocate(byteCount: bounceLen, alignment: 4096)
        defer { bounce.deallocate() }
        let ret = readBlocks(into: bounce, offset: start, length: bounceLen)
        if ret == 0 {
            buf.copyMemory(from: bounce + Int(offset - start), byteCount: length)
        }
        return ret
    }

    private func readBlocks(into buf: UnsafeMutableRawPointer, offset: UInt64, length: Int) -> Int32 {
        var done = 0
        while done < length {
            do {
                let n = try resource.read(
                    into: UnsafeMutableRawBufferPointer(start: buf + done, count: length - done),
                    startingAt: off_t(offset) + off_t(done),
                    length: length - done)
                if n <= 0 {
                    return -EIO
                }
                done += n
            } catch {
                let nsError = error as NSError
                log.error("device read at \(offset + UInt64(done)) failed: \(nsError, privacy: .public)")
                return nsError.domain == NSPOSIXErrorDomain ? -Int32(nsError.code) : -EIO
            }
        }
        return 0
    }
}
