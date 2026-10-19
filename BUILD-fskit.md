# Building and signing the btrfs FSKit module

Step by step guide for building a signed `BtrfsMac.app` from a fresh clone,
to run on your own or another Mac. Installing it is described at the end
and in the main [README](README.md).

## What you need

* A Mac with macOS 15.4 or later.
* Xcode 16.3 or later (FSKit is in the macOS 15.4 SDK).
* A **paid** Apple Developer Program membership. A free "Personal Team"
  cannot get the FSKit entitlement, and macOS refuses to run the module
  without it.
* For Developer ID signing (recommended, see step 4): the Account Holder
  or an Admin role in the team, or an existing "Developer ID Application"
  certificate.

## 1. Get the source

    git clone https://github.com/gereoffy/btrfs4mac.git
    cd btrfs4mac

The btrfs library (`core/`) does not need to be built separately: the
Xcode project compiles its C sources (and the bundled zstd decompressor)
directly into the FSKit module. Optionally, check that the library builds
and works with the command line tool:

    make
    build/btrfsro info /path/to/btrfs.img

(`make test` skips the tests whose images are not in the repository.)

## 2. Prepare Xcode

1. Start Xcode once and let it install its components, or run:

       sudo xcodebuild -license accept
       xcodebuild -runFirstLaunch

2. In Xcode open **Settings > Accounts**, add the Apple ID of the paid team
   and check that the team appears (not "Personal Team").

3. Note the **Team ID**: 10 characters, shown on
   https://developer.apple.com/account under Membership details, or in
   Xcode's Accounts pane next to the team.

You do not need to open the project or set up certificates and profiles
by hand; the build script lets Xcode create them.

## 3. Store notarization credentials (once)

Notarization lets the app pass Gatekeeper on other Macs.

1. Create an app-specific password at https://account.apple.com
   (Sign-In and Security > App-Specific Passwords).
2. Store it in the keychain under a profile name, here `notary`:

       xcrun notarytool store-credentials notary \
           --apple-id you@example.com --team-id ABCDE12345 \
           --password abcd-efgh-ijkl-mnop

You can skip this step to build without notarization (then the receiving
Mac has to allow the app manually, see Troubleshooting).

## 4. Build and sign

From the repository root:

    TEAM_ID=ABCDE12345 NOTARY_PROFILE=notary fskit/build.sh

This:

1. archives the app with automatic signing; on the first run Xcode
   registers the App IDs `io.github.gereoffy.btrfs4mac` and
   `io.github.gereoffy.btrfs4mac.fsmodule` (the latter with the FSKit
   Module capability) and creates the certificates and provisioning
   profiles,
2. exports it signed with Developer ID,
3. uploads it for notarization, waits (usually a few minutes) and staples
   the ticket,
4. writes `fskit/dist/BtrfsMac.app` and `fskit/dist/BtrfsMac.zip`.

The end of the output should look like:

    == built developer-id
    Authority=Developer ID Application: <Team Name> (ABCDE12345)
    TeamIdentifier=ABCDE12345
    provisioning profile: embedded

To build for a few known Macs instead of distribution, use
`MODE=development` (Apple Development signing). Every Mac that should run
it must then be registered as a device in the team; its Provisioning UDID
is shown by `system_profiler SPHardwareDataType | grep "Provisioning UDID"`.

## 5. Check the result

    spctl -a -vvv -t exec fskit/dist/BtrfsMac.app
    xcrun stapler validate fskit/dist/BtrfsMac.app

`spctl` should report `accepted` and `source=Notarized Developer ID`.

## 6. Hand it over

Send `fskit/dist/BtrfsMac.zip`. On the target Mac, with this repository
checked out (only the script is needed, no Xcode):

    fskit/install.sh ~/Downloads/BtrfsMac.zip

then launch `/Applications/BtrfsMac.app` once and enable "btrfs" in System
Settings > General > Login Items & Extensions > File System Extensions.
btrfs disks and attached images are then mounted read-only automatically:

    hdiutil attach -readonly -imagekey diskimage-class=CRawDiskImage image.img

## Troubleshooting

* **"No signing certificate Developer ID Application found"**: only the
  Account Holder (or an Admin) can create one. Create it in Xcode
  Settings > Accounts > Manage Certificates > + > Developer ID Application,
  or ask the Account Holder, then run the build again.
* **"Personal development teams ... do not support the FSKit Module
  capability"**: Xcode used the free personal team; check `TEAM_ID` and
  that the paid team is signed in.
* **The bundle ID is already in use by another team**: change `BUNDLE_ID`
  in `fskit/gen-xcodeproj.py`, regenerate the project with
  `python3 fskit/gen-xcodeproj.py`, and change `MODULE_ID` in
  `fskit/install.sh` to the new `<BUNDLE_ID>.fsmodule`.
* **The Developer ID export is refused for the FSKit capability**: build
  with `MODE=development` for registered Macs instead.
* **Notarization fails**: show the reason with
  `xcrun notarytool log <submission id> --keychain-profile notary`.
* **Not notarized, the target Mac blocks the app**: after installing, open
  it once with right click > Open, or allow it in System Settings >
  Privacy & Security.
* **The module does not show up or the switch flips back off** on the
  target Mac: see the notes in the main README (home folder outside the
  system volume, other copies of the app registered).
