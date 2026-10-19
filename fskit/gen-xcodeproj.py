#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate fskit/BtrfsMac.xcodeproj.

The project has two targets:
  BtrfsFS   FSKit module (ExtensionKit extension), Swift + the libbtrfsro C
            sources compiled in directly
  BtrfsMac  host application embedding BtrfsFS in Contents/Extensions

usage: gen-xcodeproj.py [--team TEAMID]

Run it again after adding source files. Selecting the team in Xcode
(Signing & Capabilities) works as well; that edit is kept until the next
regeneration, so pass --team to make it stick.
"""
import argparse
import glob
import hashlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))
BUNDLE_ID = "io.github.gereoffy.btrfs4mac"
DEPLOYMENT = "15.4"


def oid(*parts):
    """Deterministic 24 hex digit object id."""
    return hashlib.md5("/".join(parts).encode()).hexdigest()[:24].upper()


def q(value):
    """Quote a pbxproj string when needed."""
    if value and all(c.isalnum() or c in "._/$" for c in value):
        return value
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def fmt(value, indent):
    pad = "\t" * indent
    if isinstance(value, dict):
        out = "{\n"
        for k, v in value.items():
            out += f"{pad}\t{q(k)} = {fmt(v, indent + 1)};\n"
        return out + pad + "}"
    if isinstance(value, list):
        out = "(\n"
        for v in value:
            out += f"{pad}\t{fmt(v, indent + 1)},\n"
        return out + pad + ")"
    return q(str(value))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--team", default="")
    args = ap.parse_args()

    objects = {}

    def add(kind, ident, **fields):
        objects[ident] = dict(isa=kind, **fields)
        return ident

    # ---- files ----
    ext_swift = sorted(glob.glob(os.path.join(HERE, "BtrfsFS", "*.swift")))
    app_swift = sorted(glob.glob(os.path.join(HERE, "BtrfsMac", "*.swift")))
    core_c = sorted(glob.glob(os.path.join(HERE, "..", "core", "src", "*.c")))
    core_h = sorted(glob.glob(os.path.join(HERE, "..", "core", "src", "*.h")))
    zstd_c = sorted(glob.glob(os.path.join(HERE, "..", "core", "vendor", "zstd",
                                           "*", "*.c")))

    def rel(p, base):
        return os.path.relpath(p, base)

    def ref_in(group, path):
        ext = os.path.splitext(path)[1]
        ftype = {
            ".swift": "sourcecode.swift", ".c": "sourcecode.c.c",
            ".h": "sourcecode.c.h", ".plist": "text.plist.xml",
            ".entitlements": "text.plist.entitlements",
        }.get(ext, "text")
        return add("PBXFileReference", oid("ref", group, path),
                   lastKnownFileType=ftype, path=path, sourceTree="<group>")

    core_dir = os.path.join(HERE, "..", "core")
    ext_refs = [ref_in("BtrfsFS", os.path.basename(p)) for p in ext_swift]
    ext_misc = [ref_in("BtrfsFS", n) for n in ("Info.plist", "BtrfsFS.entitlements",
                                               "BtrfsFS-Bridging-Header.h")]
    app_refs = [ref_in("BtrfsMac", os.path.basename(p)) for p in app_swift]
    app_misc = [ref_in("BtrfsMac", "BtrfsMac.entitlements")]
    core_c_refs = [ref_in("core", rel(p, core_dir)) for p in core_c + zstd_c]
    core_h_refs = [ref_in("core", rel(p, core_dir)) for p in core_h] + \
                  [ref_in("core", "include/btrfsro.h")]

    app_product = add("PBXFileReference", oid("product", "app"),
                      explicitFileType="wrapper.application",
                      includeInIndex="0", path="BtrfsMac.app",
                      sourceTree="BUILT_PRODUCTS_DIR")
    ext_product = add("PBXFileReference", oid("product", "ext"),
                      explicitFileType="wrapper.extensionkit-extension",
                      includeInIndex="0", path="BtrfsFS.appex",
                      sourceTree="BUILT_PRODUCTS_DIR")

    # ---- groups ----
    g_ext = add("PBXGroup", oid("group", "BtrfsFS"), children=ext_refs + ext_misc,
                path="BtrfsFS", sourceTree="<group>")
    g_app = add("PBXGroup", oid("group", "BtrfsMac"), children=app_refs + app_misc,
                path="BtrfsMac", sourceTree="<group>")
    g_core = add("PBXGroup", oid("group", "core"), children=core_h_refs + core_c_refs,
                 name="libbtrfsro", path="../core", sourceTree="<group>")
    g_products = add("PBXGroup", oid("group", "Products"),
                     children=[app_product, ext_product], name="Products",
                     sourceTree="<group>")
    g_main = add("PBXGroup", oid("group", "main"),
                 children=[g_app, g_ext, g_core, g_products], sourceTree="<group>")

    # ---- build phases ----
    def build_file(ref, target, **extra):
        return add("PBXBuildFile", oid("build", target, ref), fileRef=ref, **extra)

    ext_sources = add("PBXSourcesBuildPhase", oid("phase", "ext", "sources"),
                      buildActionMask="2147483647",
                      files=[build_file(r, "ext") for r in ext_refs + core_c_refs],
                      runOnlyForDeploymentPostprocessing="0")
    ext_frameworks = add("PBXFrameworksBuildPhase", oid("phase", "ext", "fw"),
                         buildActionMask="2147483647", files=[],
                         runOnlyForDeploymentPostprocessing="0")
    app_sources = add("PBXSourcesBuildPhase", oid("phase", "app", "sources"),
                      buildActionMask="2147483647",
                      files=[build_file(r, "app") for r in app_refs],
                      runOnlyForDeploymentPostprocessing="0")
    app_frameworks = add("PBXFrameworksBuildPhase", oid("phase", "app", "fw"),
                         buildActionMask="2147483647", files=[],
                         runOnlyForDeploymentPostprocessing="0")
    embed = add("PBXCopyFilesBuildPhase", oid("phase", "app", "embed"),
                buildActionMask="2147483647", dstPath="$(EXTENSIONS_FOLDER_PATH)",
                dstSubfolderSpec="16",
                files=[build_file(ext_product, "embed",
                                  settings={"ATTRIBUTES": ["RemoveHeadersOnCopy"]})],
                name="Embed ExtensionKit Extensions",
                runOnlyForDeploymentPostprocessing="0")

    # ---- build settings ----
    common = {
        "ALWAYS_SEARCH_USER_PATHS": "NO",
        "CLANG_ENABLE_MODULES": "YES",
        "CODE_SIGN_STYLE": "Automatic",
        "CURRENT_PROJECT_VERSION": "1",
        "DEVELOPMENT_TEAM": args.team,
        "ENABLE_HARDENED_RUNTIME": "YES",
        "ENABLE_USER_SCRIPT_SANDBOXING": "YES",
        "GCC_C_LANGUAGE_STANDARD": "gnu11",
        "MACOSX_DEPLOYMENT_TARGET": DEPLOYMENT,
        "MARKETING_VERSION": "0.1",
        "SDKROOT": "macosx",
        "SWIFT_VERSION": "5.0",
    }
    project_debug = dict(common, **{
        "DEBUG_INFORMATION_FORMAT": "dwarf",
        "ENABLE_TESTABILITY": "YES",
        "GCC_OPTIMIZATION_LEVEL": "0",
        "ONLY_ACTIVE_ARCH": "YES",
        "SWIFT_ACTIVE_COMPILATION_CONDITIONS": "DEBUG",
        "SWIFT_OPTIMIZATION_LEVEL": "-Onone",
    })
    project_release = dict(common, **{
        "DEBUG_INFORMATION_FORMAT": "dwarf-with-dsym",
        "SWIFT_COMPILATION_MODE": "wholemodule",
        "SWIFT_OPTIMIZATION_LEVEL": "-O",
    })
    ext_settings = {
        "CODE_SIGN_ENTITLEMENTS": "BtrfsFS/BtrfsFS.entitlements",
        "GCC_PREPROCESSOR_DEFINITIONS": ["$(inherited)", "_FILE_OFFSET_BITS=64",
                                         "ZSTD_DISABLE_ASM", "ZSTD_LEGACY_SUPPORT=0",
                                         "DEBUGLEVEL=0"],
        "GCC_WARN_INHIBIT_ALL_WARNINGS": "YES",
        "GENERATE_INFOPLIST_FILE": "NO",
        "HEADER_SEARCH_PATHS": ["$(SRCROOT)/../core/include"],
        "INFOPLIST_FILE": "BtrfsFS/Info.plist",
        "LD_RUNPATH_SEARCH_PATHS": ["$(inherited)", "@executable_path/../Frameworks",
                                    "@executable_path/../../../../Frameworks"],
        "OTHER_LDFLAGS": ["$(inherited)", "-lz"],
        "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID + ".fsmodule",
        "PRODUCT_NAME": "$(TARGET_NAME)",
        "SKIP_INSTALL": "YES",
        "SWIFT_OBJC_BRIDGING_HEADER": "BtrfsFS/BtrfsFS-Bridging-Header.h",
    }
    app_settings = {
        "ASSETCATALOG_COMPILER_APPICON_NAME": "",
        "CODE_SIGN_ENTITLEMENTS": "BtrfsMac/BtrfsMac.entitlements",
        "COMBINE_HIDPI_IMAGES": "YES",
        "GENERATE_INFOPLIST_FILE": "YES",
        "INFOPLIST_KEY_CFBundleDisplayName": "btrfs for macOS",
        "INFOPLIST_KEY_LSApplicationCategoryType": "public.app-category.utilities",
        "INFOPLIST_KEY_NSHumanReadableCopyright": "MIT License",
        "LD_RUNPATH_SEARCH_PATHS": ["$(inherited)", "@executable_path/../Frameworks"],
        "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID,
        "PRODUCT_NAME": "BtrfsMac",
    }

    def config_list(name, debug, release):
        d = add("XCBuildConfiguration", oid("cfg", name, "Debug"),
                buildSettings=debug, name="Debug")
        r = add("XCBuildConfiguration", oid("cfg", name, "Release"),
                buildSettings=release, name="Release")
        return add("XCConfigurationList", oid("cfglist", name),
                   buildConfigurations=[d, r],
                   defaultConfigurationIsVisible="0",
                   defaultConfigurationName="Release")

    # ---- targets ----
    ext_target = add("PBXNativeTarget", oid("target", "ext"),
                     buildConfigurationList=config_list("ext", ext_settings, ext_settings),
                     buildPhases=[ext_sources, ext_frameworks],
                     buildRules=[], dependencies=[], name="BtrfsFS",
                     productName="BtrfsFS", productReference=ext_product,
                     productType="com.apple.product-type.extensionkit-extension")
    project_id = oid("project")
    proxy = add("PBXContainerItemProxy", oid("proxy", "ext"),
                containerPortal=project_id, proxyType="1",
                remoteGlobalIDString=ext_target, remoteInfo="BtrfsFS")
    dep = add("PBXTargetDependency", oid("dep", "ext"), target=ext_target,
              targetProxy=proxy)
    app_target = add("PBXNativeTarget", oid("target", "app"),
                     buildConfigurationList=config_list("app", app_settings, app_settings),
                     buildPhases=[app_sources, app_frameworks, embed],
                     buildRules=[], dependencies=[dep], name="BtrfsMac",
                     productName="BtrfsMac", productReference=app_product,
                     productType="com.apple.product-type.application")

    add("PBXProject", project_id,
        attributes={"BuildIndependentTargetsInParallel": "1",
                    "LastSwiftUpdateCheck": "1640",
                    "LastUpgradeCheck": "1640",
                    "TargetAttributes": {
                        app_target: {"CreatedOnToolsVersion": "16.4"},
                        ext_target: {"CreatedOnToolsVersion": "16.4"}}},
        buildConfigurationList=config_list("project", project_debug, project_release),
        compatibilityVersion="Xcode 14.0", developmentRegion="en",
        hasScannedForEncodings="0", knownRegions=["en", "Base"],
        mainGroup=g_main, productRefGroup=g_products, projectDirPath="",
        projectRoot="", targets=[app_target, ext_target])

    # ---- write ----
    out = "// !$*UTF8*$!\n{\n\tarchiveVersion = 1;\n\tclasses = {\n\t};\n"
    out += "\tobjectVersion = 56;\n\tobjects = {\n"
    for kind in sorted({o["isa"] for o in objects.values()}):
        out += f"\n/* Begin {kind} section */\n"
        for ident in sorted(i for i, o in objects.items() if o["isa"] == kind):
            out += f"\t\t{ident} = {fmt(objects[ident], 2)};\n"
        out += f"/* End {kind} section */\n"
    out += f"\t}};\n\trootObject = {project_id};\n}}\n"

    proj = os.path.join(HERE, "BtrfsMac.xcodeproj")
    os.makedirs(proj, exist_ok=True)
    with open(os.path.join(proj, "project.pbxproj"), "w") as f:
        f.write(out)
    print(f"wrote {proj} ({len(objects)} objects, team '{args.team}')")


if __name__ == "__main__":
    main()
