// SPDX-License-Identifier: MIT
// Lists the FSKit modules fskitd knows about and whether they are enabled.
// Build: swiftc -target x86_64-apple-macos15.4 tools/fsmodules.swift -o fsmodules
import FSKit

let done = DispatchSemaphore(value: 0)
FSClient.shared.fetchInstalledExtensions { modules, error in
    if let error {
        print("error: \(error)")
    }
    for module in modules ?? [] {
        print("\(module.bundleIdentifier)\tenabled=\(module.isEnabled)\t\(module.url.path)")
    }
    done.signal()
}
done.wait()
