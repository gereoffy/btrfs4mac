#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Build, sign and install BtrfsMac.app (with the btrfs FSKit module) into
# /Applications for local development.
#
#   fskit/install.sh                 sign with the first "Apple Development"
#                                    identity, or ad hoc if there is none
#   SIGN_IDENTITY=- fskit/install.sh force ad hoc signing
#
# Without a paid developer account the FSKit entitlement has no provisioning
# profile, so macOS only runs the module with AMFI relaxed (boot-arg
# amfi_get_out_of_my_way=1). With a paid team, build from Xcode instead.
#
# After installing: launch the app once, then enable "btrfs" in System
# Settings > General > Login Items & Extensions > File System Extensions.
# btrfs disks (and attached images) are then mounted automatically.

set -euo pipefail

cd "$(dirname "$0")"
APP=BtrfsMac.app
MODULE_ID=io.github.gereoffy.btrfs4mac.fsmodule
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
BUILT=build/Build/Products/Release/$APP

echo "== building"
xcodebuild -project BtrfsMac.xcodeproj -scheme BtrfsMac -configuration Release \
	-derivedDataPath build CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual \
	DEVELOPMENT_TEAM= PROVISIONING_PROFILE_SPECIFIER= build -quiet

if [ -z "${SIGN_IDENTITY:-}" ]; then
	SIGN_IDENTITY=$(security find-identity -v -p codesigning |
		awk '/Apple Development/ {print $2; exit}')
	SIGN_IDENTITY=${SIGN_IDENTITY:--}
fi
echo "== signing with ${SIGN_IDENTITY}"
codesign --force --options runtime --timestamp=none --sign "$SIGN_IDENTITY" \
	--entitlements BtrfsFS/BtrfsFS.entitlements \
	"$BUILT/Contents/Extensions/BtrfsFS.appex"
codesign --force --options runtime --timestamp=none --sign "$SIGN_IDENTITY" \
	--entitlements BtrfsMac/BtrfsMac.entitlements "$BUILT"

echo "== installing /Applications/$APP"
# The build products must not stay registered next to the installed copy
"$LSREGISTER" -u "$PWD/$BUILT" 2>/dev/null || true
pluginkit -r "$PWD/$BUILT/Contents/Extensions/BtrfsFS.appex" 2>/dev/null || true
rm -rf "/Applications/$APP"
ditto "$BUILT" "/Applications/$APP"
"$LSREGISTER" -f "/Applications/$APP"
pluginkit -a "/Applications/$APP/Contents/Extensions/BtrfsFS.appex"

# fskit_agent saves the list of enabled modules with an atomic write. When
# the home folder is not on the system volume its sandbox refuses the
# temporary file, the switch in System Settings flips back and the module
# stays disabled. The agent can still read the list, so write it directly.
SETTINGS="$HOME/Library/Group Containers/group.com.apple.fskit.settings"
if [ -d "$SETTINGS" ]; then
	python3 -I - "$SETTINGS/enabledModules.plist" "$MODULE_ID" <<'EOF'
import os, plistlib, sys
path, module = sys.argv[1:3]
enabled = []
if os.path.exists(path):
    with open(path, "rb") as f:
        enabled = plistlib.load(f)
if module not in enabled:
    enabled.append(module)
    with open(path, "wb") as f:
        plistlib.dump(enabled, f)
    print(f"== enabled {module} in {path}")
EOF
fi

echo "== done"
pluginkit -m -v -i "$MODULE_ID"
