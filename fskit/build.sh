#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Build and sign BtrfsMac.app (with the btrfs FSKit module).
# The result is fskit/dist/BtrfsMac.app and fskit/dist/BtrfsMac.zip; install
# it with fskit/install.sh (on this or another Mac).
#
# The FSKit entitlement needs a provisioning profile, so a paid Apple
# Developer Program team is required for a module that macOS will run.
# Xcode must be signed in to that team's account (Settings > Accounts);
# certificates and profiles are created automatically.
#
#   TEAM_ID=ABCDE12345 fskit/build.sh
#       Developer ID signed, runs on any Mac (default when TEAM_ID is set).
#       Add NOTARY_PROFILE=<name> to notarize and staple it, after storing
#       the credentials once with:
#         xcrun notarytool store-credentials <name> --apple-id <id> --team-id <team>
#
#   TEAM_ID=ABCDE12345 MODE=development fskit/build.sh
#       Apple Development signed, runs only on Macs registered in the team.
#       The Provisioning UDID of a Mac is shown by:
#         system_profiler SPHardwareDataType | grep "Provisioning UDID"
#
#   MODE=local fskit/build.sh
#       No team: signed with the first "Apple Development" identity (or ad
#       hoc) and no provisioning profile. AMFI refuses to run the module
#       unless it is relaxed, so this is only useful for testing the build.

set -euo pipefail

cd "$(dirname "$0")"
APP=BtrfsMac.app
PROJECT=BtrfsMac.xcodeproj
SCHEME=BtrfsMac
DIST=dist
ARCHIVE=build/BtrfsMac.xcarchive

TEAM_ID=${TEAM_ID:-}
if [ -n "$TEAM_ID" ]; then
	MODE=${MODE:-developer-id}
else
	MODE=${MODE:-local}
fi

rm -rf "$DIST"
mkdir -p "$DIST"

export_archive() { # <export method>
	local options=build/ExportOptions.plist

	cat > "$options" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>method</key>
	<string>$1</string>
	<key>signingStyle</key>
	<string>automatic</string>
	<key>teamID</key>
	<string>$TEAM_ID</string>
</dict>
</plist>
EOF
	echo "== archiving (team $TEAM_ID)"
	xcodebuild -project "$PROJECT" -scheme "$SCHEME" -configuration Release \
		-derivedDataPath build -archivePath "$ARCHIVE" \
		DEVELOPMENT_TEAM="$TEAM_ID" CODE_SIGN_STYLE=Automatic \
		-allowProvisioningUpdates archive -quiet
	echo "== exporting ($1)"
	xcodebuild -exportArchive -archivePath "$ARCHIVE" \
		-exportOptionsPlist "$options" -exportPath "$DIST" \
		-allowProvisioningUpdates -quiet
}

case "$MODE" in
developer-id)
	[ -n "$TEAM_ID" ] || { echo "TEAM_ID is required for $MODE" >&2; exit 2; }
	export_archive developer-id
	;;
development)
	[ -n "$TEAM_ID" ] || { echo "TEAM_ID is required for $MODE" >&2; exit 2; }
	export_archive debugging
	;;
local)
	echo "== building"
	xcodebuild -project "$PROJECT" -scheme "$SCHEME" -configuration Release \
		-derivedDataPath build CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual \
		DEVELOPMENT_TEAM= PROVISIONING_PROFILE_SPECIFIER= build -quiet
	ditto "build/Build/Products/Release/$APP" "$DIST/$APP"
	identity=$(security find-identity -v -p codesigning |
		awk '/Apple Development/ {print $2; exit}')
	identity=${identity:--}
	echo "== signing with $identity (no provisioning profile)"
	codesign --force --options runtime --timestamp=none --sign "$identity" \
		--entitlements BtrfsFS/BtrfsFS.entitlements \
		"$DIST/$APP/Contents/Extensions/BtrfsFS.appex"
	codesign --force --options runtime --timestamp=none --sign "$identity" \
		--entitlements BtrfsMac/BtrfsMac.entitlements "$DIST/$APP"
	;;
*)
	echo "unknown MODE '$MODE' (developer-id, development, local)" >&2
	exit 2
	;;
esac

codesign --verify --deep --strict "$DIST/$APP"
ditto -c -k --keepParent "$DIST/$APP" "$DIST/BtrfsMac.zip"

if [ "$MODE" = developer-id ] && [ -n "${NOTARY_PROFILE:-}" ]; then
	echo "== notarizing"
	xcrun notarytool submit "$DIST/BtrfsMac.zip" --keychain-profile "$NOTARY_PROFILE" --wait
	xcrun stapler staple "$DIST/$APP"
	rm "$DIST/BtrfsMac.zip"
	ditto -c -k --keepParent "$DIST/$APP" "$DIST/BtrfsMac.zip"
fi

echo "== built $MODE"
codesign -dvv "$DIST/$APP/Contents/Extensions/BtrfsFS.appex" 2>&1 |
	grep -E '^(Authority|TeamIdentifier)=' | head -2
if [ -f "$DIST/$APP/Contents/Extensions/BtrfsFS.appex/Contents/embedded.provisionprofile" ]; then
	echo "provisioning profile: embedded"
else
	echo "provisioning profile: none (the module will not run with AMFI enabled)"
fi
ls -l "$DIST"
