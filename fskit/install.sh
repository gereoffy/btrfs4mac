#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Install a built BtrfsMac.app (see fskit/build.sh) into /Applications and
# register its btrfs FSKit module. Needs no developer tools.
#
#   fskit/install.sh                     installs fskit/dist/BtrfsMac.app
#   fskit/install.sh path/to/BtrfsMac.app
#   fskit/install.sh path/to/BtrfsMac.zip
#
# Afterwards launch /Applications/BtrfsMac.app once, then enable "btrfs" in
# System Settings > General > Login Items & Extensions > File System
# Extensions. btrfs disks and attached images are then mounted read-only
# automatically, e.g.:
#   hdiutil attach -readonly -imagekey diskimage-class=CRawDiskImage image.img

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
APP=BtrfsMac.app
DEST=/Applications/$APP
MODULE_ID=io.github.gereoffy.btrfs4mac.fsmodule
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

SRC=${1:-$HERE/dist/$APP}
TMP=

cleanup() {
	[ -n "$TMP" ] && rm -rf "$TMP"
	return 0
}
trap cleanup EXIT

case "$SRC" in
*.zip)
	TMP=$(mktemp -d)
	ditto -x -k "$SRC" "$TMP"
	SRC=$TMP/$APP
	;;
esac
SRC=${SRC%/}
if [ ! -d "$SRC/Contents/Extensions/BtrfsFS.appex" ]; then
	echo "not a BtrfsMac.app: $SRC" >&2
	exit 1
fi

echo "== checking signature"
codesign --verify --deep --strict "$SRC"
codesign -dvv "$SRC/Contents/Extensions/BtrfsFS.appex" 2>&1 |
	grep -E '^(Authority|TeamIdentifier)=' | head -2 || true
if [ ! -f "$SRC/Contents/Extensions/BtrfsFS.appex/Contents/embedded.provisionprofile" ]; then
	echo "warning: no provisioning profile, macOS will not run the module" \
	     "unless AMFI is relaxed" >&2
fi

# Copies elsewhere with the same bundle ID (build products, downloads)
# confuse the extension registry: only the installed one may stay registered.
for other in "$HERE/build/Build/Products/Release/$APP" \
	     "$HERE/build/Build/Products/Debug/$APP" "$HERE/dist/$APP" "$SRC"; do
	[ -d "$other" ] || continue
	pluginkit -r "$other/Contents/Extensions/BtrfsFS.appex" 2>/dev/null || true
	"$LSREGISTER" -u "$other" 2>/dev/null || true
done

echo "== installing $DEST"
rm -rf "$DEST"
ditto "$SRC" "$DEST"
"$LSREGISTER" -f "$DEST"
pluginkit -a "$DEST/Contents/Extensions/BtrfsFS.appex"

# fskit_agent saves the list of enabled modules with an atomic write. When
# the home folder is not on the system volume its sandbox refuses the
# temporary file, the switch in System Settings flips back and the module
# stays disabled. The agent can still read the list, so add the module to it.
SETTINGS="$HOME/Library/Group Containers/group.com.apple.fskit.settings"
LIST=$SETTINGS/enabledModules.plist
if [ -d "$SETTINGS" ]; then
	if [ ! -f "$LIST" ]; then
		cat > "$LIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<array>
	<string>$MODULE_ID</string>
</array>
</plist>
EOF
		echo "== enabled $MODULE_ID in $LIST"
	elif ! plutil -p "$LIST" | grep -q "\"$MODULE_ID\""; then
		count=$(plutil -p "$LIST" | grep -c '=>' || true)
		/usr/libexec/PlistBuddy -c "Add :$count string $MODULE_ID" "$LIST"
		echo "== enabled $MODULE_ID in $LIST"
	fi
fi

echo "== done"
pluginkit -m -v -i "$MODULE_ID"
echo "Now launch $DEST once and enable \"btrfs\" in System Settings >" \
     "General > Login Items & Extensions > File System Extensions."
