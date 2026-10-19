// SPDX-License-Identifier: MIT

import FSKit

/// One file or directory of the mounted btrfs.
final class BtrfsItem: FSItem {
    let inode: btrfsro_inode
    let fileID: FSItem.Identifier
    /// Parent directory as seen when the item was looked up
    let parentID: FSItem.Identifier

    init(inode: btrfsro_inode, fileID: FSItem.Identifier, parentID: FSItem.Identifier) {
        self.inode = inode
        self.fileID = fileID
        self.parentID = parentID
        super.init()
    }

    var isDirectory: Bool { inode.type == UInt8(BTRFSRO_FT_DIR) }
}

/// A read-only btrfs volume backed by libbtrfsro.
final class BtrfsVolume: FSVolume {
    private let device: BlockDevice
    private var fs: OpaquePointer?
    private let info: btrfsro_volinfo
    private let root: BtrfsItem

    /// Small numbers for subvolumes other than the default one, used to
    /// keep file IDs unique across inode number spaces.
    private var subvolIndex: [UInt64: UInt64] = [:]
    private let lock = NSLock()

    init(device: BlockDevice, fs: OpaquePointer, volumeID: FSVolume.Identifier,
         name: String, info: btrfsro_volinfo) {
        self.device = device
        self.fs = fs
        self.info = info
        var rootInode = btrfsro_inode()
        btrfsro_root(fs, &rootInode)
        self.root = BtrfsItem(inode: rootInode, fileID: .rootDirectory, parentID: .parentOfRoot)
        super.init(volumeID: volumeID, volumeName: FSFileName(string: name))
    }

    var rootInode: btrfsro_inode { root.inode }

    deinit {
        if let fs {
            btrfsro_unmount(fs)
        }
    }

    // MARK: - Helpers

    private func fileID(for inode: btrfsro_inode) -> FSItem.Identifier {
        if inode.subvol == root.inode.subvol {
            if inode.ino == root.inode.ino {
                return .rootDirectory
            }
            return FSItem.Identifier(rawValue: inode.ino) ?? .invalid
        }
        lock.lock()
        defer { lock.unlock() }
        let index: UInt64
        if let existing = subvolIndex[inode.subvol] {
            index = existing
        } else {
            index = UInt64(subvolIndex.count + 1)
            subvolIndex[inode.subvol] = index
        }
        return FSItem.Identifier(rawValue: (index << 48) | inode.ino) ?? .invalid
    }

    private static func itemType(_ ft: UInt8) -> FSItem.ItemType {
        switch Int(ft) {
        case BTRFSRO_FT_REG: return .file
        case BTRFSRO_FT_DIR: return .directory
        case BTRFSRO_FT_SYMLINK: return .symlink
        case BTRFSRO_FT_FIFO: return .fifo
        case BTRFSRO_FT_CHRDEV: return .charDevice
        case BTRFSRO_FT_BLKDEV: return .blockDevice
        case BTRFSRO_FT_SOCK: return .socket
        default: return .unknown
        }
    }

    private static func timespec(_ t: btrfsro_timespec) -> timespec {
        Darwin.timespec(tv_sec: Int(t.sec), tv_nsec: Int(t.nsec))
    }

    private func parentID(ofDirectory item: BtrfsItem) -> FSItem.Identifier {
        if item.fileID == .rootDirectory {
            return .parentOfRoot
        }
        var inode = item.inode
        var parent = btrfsro_inode()
        guard btrfsro_parent(fs, &inode, &parent) == 0 else {
            return item.parentID
        }
        return fileID(for: parent)
    }

    private func attributes(of inode: btrfsro_inode, fileID: FSItem.Identifier,
                            parentID: FSItem.Identifier) throws -> FSItem.Attributes {
        var inode = inode
        var st = btrfsro_stat()
        let ret = btrfsro_stat(fs, &inode, &st)
        guard ret == 0 else {
            throw posixError(ret)
        }
        let attrs = FSItem.Attributes()
        attrs.type = Self.itemType(inode.type)
        attrs.mode = st.mode & 0o7777
        attrs.linkCount = st.nlink
        attrs.uid = st.uid
        attrs.gid = st.gid
        attrs.flags = 0
        attrs.size = st.size
        attrs.allocSize = st.nbytes
        attrs.fileID = fileID
        attrs.parentID = parentID
        attrs.accessTime = Self.timespec(st.atime)
        attrs.modifyTime = Self.timespec(st.mtime)
        attrs.changeTime = Self.timespec(st.ctime)
        attrs.birthTime = Self.timespec(st.otime)
        attrs.supportsLimitedXAttrs = false
        return attrs
    }

    private func item(_ fsItem: FSItem) throws -> BtrfsItem {
        guard let item = fsItem as? BtrfsItem else {
            throw posixError(EINVAL)
        }
        return item
    }
}

// MARK: - Volume operations

extension BtrfsVolume: FSVolume.Operations {

    var supportedVolumeCapabilities: FSVolume.SupportedCapabilities {
        let caps = FSVolume.SupportedCapabilities()
        caps.supportsPersistentObjectIDs = true
        caps.supportsSymbolicLinks = true
        caps.supportsHardLinks = true
        caps.supportsSparseFiles = true
        caps.supports2TBFiles = true
        caps.supports64BitObjectIDs = true
        caps.supportsFastStatFS = true
        caps.doesNotSupportSettingFilePermissions = true
        caps.caseFormat = .sensitive
        return caps
    }

    var volumeStatistics: FSStatFSResult {
        let stats = FSStatFSResult(fileSystemTypeName: "btrfs")
        let bs = UInt64(info.sectorsize)
        stats.blockSize = Int(bs)
        stats.ioSize = 128 * 1024
        stats.totalBytes = info.total_bytes
        stats.usedBytes = min(info.bytes_used, info.total_bytes)
        stats.freeBytes = info.total_bytes - stats.usedBytes
        // Read-only, but report the real free space like other file systems do
        stats.availableBytes = stats.freeBytes
        stats.totalBlocks = info.total_bytes / bs
        stats.usedBlocks = stats.usedBytes / bs
        stats.freeBlocks = stats.freeBytes / bs
        stats.availableBlocks = stats.freeBlocks
        return stats
    }

    // PathConf
    var maximumLinkCount: Int { 65535 }
    var maximumNameLength: Int { 255 }
    var restrictsOwnershipChanges: Bool { true }
    var truncatesLongNames: Bool { false }

    func activate(options: FSTaskOptions,
                  replyHandler: @escaping (FSItem?, Error?) -> Void) {
        replyHandler(root, nil)
    }

    func deactivate(options: FSDeactivateOptions,
                    replyHandler: @escaping (Error?) -> Void) {
        replyHandler(nil)
    }

    func mount(options: FSTaskOptions, replyHandler: @escaping (Error?) -> Void) {
        replyHandler(nil)
    }

    func unmount(replyHandler: @escaping () -> Void) {
        replyHandler()
    }

    func synchronize(flags: FSSyncFlags, replyHandler: @escaping (Error?) -> Void) {
        replyHandler(nil)
    }

    func getAttributes(_ desiredAttributes: FSItem.GetAttributesRequest, of fsItem: FSItem,
                       replyHandler: @escaping (FSItem.Attributes?, Error?) -> Void) {
        do {
            let item = try item(fsItem)
            let parent = item.isDirectory ? parentID(ofDirectory: item) : item.parentID
            replyHandler(try attributes(of: item.inode, fileID: item.fileID, parentID: parent), nil)
        } catch {
            replyHandler(nil, error)
        }
    }

    func setAttributes(_ newAttributes: FSItem.SetAttributesRequest, on item: FSItem,
                       replyHandler: @escaping (FSItem.Attributes?, Error?) -> Void) {
        replyHandler(nil, posixError(EROFS))
    }

    func lookupItem(named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        do {
            let dir = try item(directory)
            var dirInode = dir.inode
            var found = btrfsro_inode()
            let ret = name.data.withUnsafeBytes { raw in
                btrfsro_lookup(fs, &dirInode,
                               raw.baseAddress?.assumingMemoryBound(to: CChar.self),
                               raw.count, &found)
            }
            guard ret == 0 else {
                throw posixError(ret)
            }
            let item = BtrfsItem(inode: found, fileID: fileID(for: found), parentID: dir.fileID)
            replyHandler(item, name, nil)
        } catch {
            replyHandler(nil, nil, error)
        }
    }

    func reclaimItem(_ item: FSItem, replyHandler: @escaping (Error?) -> Void) {
        replyHandler(nil)
    }

    func readSymbolicLink(_ fsItem: FSItem,
                          replyHandler: @escaping (FSFileName?, Error?) -> Void) {
        do {
            let item = try item(fsItem)
            var inode = item.inode
            var buf = [CChar](repeating: 0, count: 4096)
            let ret = btrfsro_readlink(fs, &inode, &buf, buf.count)
            guard ret >= 0 else {
                throw posixError(ret)
            }
            let data = buf.withUnsafeBytes { Data($0.prefix(Int(ret))) }
            replyHandler(FSFileName(data: data), nil)
        } catch {
            replyHandler(nil, error)
        }
    }

    func enumerateDirectory(_ directory: FSItem, startingAt cookie: FSDirectoryCookie,
                            verifier: FSDirectoryVerifier,
                            attributes: FSItem.GetAttributesRequest?,
                            packer: FSDirectoryEntryPacker,
                            replyHandler: @escaping (FSDirectoryVerifier, Error?) -> Void) {
        // The filesystem never changes, so one verifier fits all directories
        let currentVerifier = FSDirectoryVerifier(rawValue: max(info.generation, 1))
        do {
            let dir = try item(directory)
            var start = cookie.rawValue

            // Cookies: 0 -> ".", 1 -> "..", n >= 2 -> btrfs dir index n
            if attributes == nil {
                if start == 0 {
                    if !packer.packEntry(name: FSFileName(string: "."), itemType: .directory,
                                         itemID: dir.fileID,
                                         nextCookie: FSDirectoryCookie(rawValue: 1),
                                         attributes: nil) {
                        replyHandler(currentVerifier, nil)
                        return
                    }
                    start = 1
                }
                if start == 1 {
                    if !packer.packEntry(name: FSFileName(string: ".."), itemType: .directory,
                                         itemID: parentID(ofDirectory: dir),
                                         nextCookie: FSDirectoryCookie(rawValue: 2),
                                         attributes: nil) {
                        replyHandler(currentVerifier, nil)
                        return
                    }
                    start = 2
                }
            }
            start = max(start, 2)

            let context = EnumerationContext(volume: self, directory: dir, packer: packer,
                                             wantAttributes: attributes != nil)
            var dirInode = dir.inode
            let ret = withExtendedLifetime(context) {
                btrfsro_readdir(fs, &dirInode, start, { ctx, name, nameLen, entry, index in
                    guard let ctx, let name, let entry else { return 1 }
                    let context = Unmanaged<EnumerationContext>.fromOpaque(ctx)
                        .takeUnretainedValue()
                    return context.pack(name: name, length: nameLen, entry: entry.pointee,
                                        index: index) ? 0 : 1
                }, Unmanaged.passUnretained(context).toOpaque())
            }
            if let error = context.error {
                throw error
            }
            guard ret == 0 else {
                throw posixError(ret)
            }
            replyHandler(currentVerifier, nil)
        } catch {
            replyHandler(currentVerifier, error)
        }
    }

    /// State shared with the C readdir callback.
    private final class EnumerationContext {
        let volume: BtrfsVolume
        let directory: BtrfsItem
        let packer: FSDirectoryEntryPacker
        let wantAttributes: Bool
        var error: Error?

        init(volume: BtrfsVolume, directory: BtrfsItem, packer: FSDirectoryEntryPacker,
             wantAttributes: Bool) {
            self.volume = volume
            self.directory = directory
            self.packer = packer
            self.wantAttributes = wantAttributes
        }

        /// Return false to stop the iteration.
        func pack(name: UnsafePointer<CChar>, length: Int, entry: btrfsro_inode,
                  index: UInt64) -> Bool {
            let fileName = FSFileName(data: Data(bytes: name, count: length))
            let id = volume.fileID(for: entry)
            var attrs: FSItem.Attributes?
            if wantAttributes {
                do {
                    attrs = try volume.attributes(of: entry, fileID: id,
                                                  parentID: directory.fileID)
                } catch {
                    self.error = error
                    return false
                }
            }
            return packer.packEntry(name: fileName, itemType: BtrfsVolume.itemType(entry.type),
                                    itemID: id,
                                    nextCookie: FSDirectoryCookie(rawValue: index + 1),
                                    attributes: attrs)
        }
    }

    // MARK: Writes are refused

    func createItem(named name: FSFileName, type: FSItem.ItemType, inDirectory directory: FSItem,
                    attributes newAttributes: FSItem.SetAttributesRequest,
                    replyHandler: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        replyHandler(nil, nil, posixError(EROFS))
    }

    func createSymbolicLink(named name: FSFileName, inDirectory directory: FSItem,
                            attributes newAttributes: FSItem.SetAttributesRequest,
                            linkContents contents: FSFileName,
                            replyHandler: @escaping (FSItem?, FSFileName?, Error?) -> Void) {
        replyHandler(nil, nil, posixError(EROFS))
    }

    func createLink(to item: FSItem, named name: FSFileName, inDirectory directory: FSItem,
                    replyHandler: @escaping (FSFileName?, Error?) -> Void) {
        replyHandler(nil, posixError(EROFS))
    }

    func removeItem(_ item: FSItem, named name: FSFileName, fromDirectory directory: FSItem,
                    replyHandler: @escaping (Error?) -> Void) {
        replyHandler(posixError(EROFS))
    }

    func renameItem(_ item: FSItem, inDirectory sourceDirectory: FSItem, named sourceName: FSFileName,
                    to destinationName: FSFileName, inDirectory destinationDirectory: FSItem,
                    overItem: FSItem?,
                    replyHandler: @escaping (FSFileName?, Error?) -> Void) {
        replyHandler(nil, posixError(EROFS))
    }
}

// MARK: - Open / close

extension BtrfsVolume: FSVolume.OpenCloseOperations {
    func openItem(_ item: FSItem, modes: FSVolume.OpenModes,
                  replyHandler: @escaping (Error?) -> Void) {
        replyHandler(modes.contains(.write) ? posixError(EROFS) : nil)
    }

    func closeItem(_ item: FSItem, modes: FSVolume.OpenModes,
                   replyHandler: @escaping (Error?) -> Void) {
        replyHandler(nil)
    }
}

// MARK: - Read / write

extension BtrfsVolume: FSVolume.ReadWriteOperations {
    func read(from fsItem: FSItem, at offset: off_t, length: Int, into buffer: FSMutableFileDataBuffer,
              replyHandler: @escaping (Int, Error?) -> Void) {
        do {
            let item = try item(fsItem)
            guard offset >= 0 else {
                throw posixError(EINVAL)
            }
            var inode = item.inode
            let ret = buffer.withUnsafeMutableBytes { raw -> Int in
                btrfsro_read(fs, &inode, UInt64(offset), raw.baseAddress,
                             min(length, raw.count))
            }
            guard ret >= 0 else {
                throw posixError(Int32(ret))
            }
            replyHandler(ret, nil)
        } catch {
            replyHandler(0, error)
        }
    }

    func write(contents: Data, to item: FSItem, at offset: off_t,
               replyHandler: @escaping (Int, Error?) -> Void) {
        replyHandler(0, posixError(EROFS))
    }
}

// MARK: - Extended attributes

extension BtrfsVolume: FSVolume.XattrOperations {
    func getXattr(named name: FSFileName, of fsItem: FSItem,
                  replyHandler: @escaping (Data?, Error?) -> Void) {
        do {
            let item = try item(fsItem)
            guard let key = name.string else {
                throw posixError(ENOATTR)
            }
            var inode = item.inode
            let size = btrfsro_getxattr(fs, &inode, key, nil, 0)
            guard size >= 0 else {
                throw posixError(size == -ENODATA ? ENOATTR : Int32(size))
            }
            var data = Data(count: size)
            let ret = data.withUnsafeMutableBytes { raw in
                btrfsro_getxattr(fs, &inode, key, raw.baseAddress, size)
            }
            guard ret >= 0 else {
                throw posixError(Int32(ret))
            }
            replyHandler(data.prefix(ret), nil)
        } catch {
            replyHandler(nil, error)
        }
    }

    func setXattr(named name: FSFileName, to value: Data?, on item: FSItem,
                  policy: FSVolume.SetXattrPolicy, replyHandler: @escaping (Error?) -> Void) {
        replyHandler(posixError(EROFS))
    }

    func listXattrs(of fsItem: FSItem, replyHandler: @escaping ([FSFileName]?, Error?) -> Void) {
        do {
            let item = try item(fsItem)
            var inode = item.inode
            let size = btrfsro_listxattr(fs, &inode, nil, 0)
            guard size >= 0 else {
                throw posixError(Int32(size))
            }
            var buf = [CChar](repeating: 0, count: max(size, 1))
            let ret = btrfsro_listxattr(fs, &inode, &buf, buf.count)
            guard ret >= 0 else {
                throw posixError(Int32(ret))
            }
            let names = buf.prefix(ret).split(separator: 0).map { chars in
                FSFileName(data: Data(chars.map { UInt8(bitPattern: $0) }))
            }
            replyHandler(names, nil)
        } catch {
            replyHandler(nil, error)
        }
    }
}

// MARK: - Check (called by DiskArbitration before mounting)

extension BtrfsVolume: FSManageableResourceMaintenanceOperations {
    /// There is nothing to repair on a read-only file system; verify that the
    /// root directory can be read, so an unreadable file system is reported.
    func startCheck(task: FSTask, options: FSTaskOptions) throws -> Progress {
        let progress = Progress(totalUnitCount: 1)
        DispatchQueue.global().async {
            var result: Error?
            var root = self.rootInode
            var st = btrfsro_stat()
            var ret = btrfsro_stat(self.fs, &root, &st)
            if ret == 0 {
                ret = btrfsro_readdir(self.fs, &root, 0, { _, _, _, _, _ in 1 }, nil)
            }
            if ret != 0 {
                result = posixError(ret)
                task.logMessage("btrfs: cannot read the root directory (error \(-ret))")
            } else {
                task.logMessage("btrfs: read-only file system, nothing to repair")
            }
            progress.completedUnitCount = 1
            task.didComplete(error: result)
        }
        return progress
    }

    func startFormat(task: FSTask, options: FSTaskOptions) throws -> Progress {
        throw posixError(EROFS)
    }
}
