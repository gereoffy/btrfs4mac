// SPDX-License-Identifier: MIT

import ExtensionFoundation
import FSKit

/// Entry point of the FSKit module.
@main
struct BtrfsFSExtension: UnaryFileSystemExtension {
    let fileSystem = BtrfsFileSystem()
}
