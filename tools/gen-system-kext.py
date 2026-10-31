#!/usr/bin/env python3
"""gen-system-kext.py — the codeless System.kext: what the kernel has built in
(E21 step 19, nextbsd-kernel-extensions#5).

    usage: gen-system-kext.py <inputs-dir> <out-dir>

Darwin describes its kernel to OSKext with System.kext, an Info.plist and no
code: com.apple.kernel, marked OSKernelResource, versioned with the kernel.
OSKext treats it as part of the kernel, so kexts can name it in
OSBundleLibraries and it is never loaded. This writes NextBSD's, for the kernel
the kexts in this set were built against:

  CFBundleIdentifier  org.nextbsd.kernel
  OSKernelResource    true; no CFBundleExecutable
  CFBundleVersion     the kernel version (the SDK's KERNEL-VERSION, "27.0.0",
                      nextbsd-kernel#376; newvers.sh REVISION, "15.1", from an
                      SDK that predates it), also its CFBundleShortVersionString
                      and OSBundleCompatibleVersion, as com.apple.kernel's are
  NBSDKernelABIVersion  the kext ABI version (the SDK's KERNEL-ABI-VERSION,
                      "27.0"), the key every kext in the set carries too.
                      OSKext refuses a kext whose NBSDKernelABIVersion differs
                      from the running kernel's (kern.osrelease major.minor,
                      or System.kext's on a kernel that predates #376)
  NBSDKernelBuildID   the legacy key: the SDK's KERNEL-BUILD-ID
                      (nextbsd-kernel#326), whatever it holds. OSKext falls
                      back to it for a kext without NBSDKernelABIVersion, and
                      packaging compares it with the kernel's
  NBSDModules         every module version compiled into the kernel (the
                      SDK's kernel.modules), the key the kexts use for the
                      names they provide, so kextd's module-name index
                      resolves a built-in module to org.nextbsd.kernel

From <inputs-dir>: KERNEL-BUILD-ID, KERNEL-ABI-VERSION, kernel.modules and
kernel.version, which the modules job takes from the kernel SDK (an SDK from
before nextbsd-kernel#376 has no KERNEL-ABI-VERSION; fetch-kernel-sdk.sh then
writes its KERNEL-BUILD-ID there). Writes <out-dir>/System.kext.
"""
import os
import plistlib
import sys

BUNDLE_ID = "org.nextbsd.kernel"


def read(inputs, name):
    path = os.path.join(inputs, name)
    try:
        with open(path) as f:
            return f.read()
    except OSError as e:
        sys.exit(f"gen-system-kext: {e}")


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__.strip().splitlines()[3].strip())
    inputs, out = argv[1], argv[2]
    build_id = read(inputs, "KERNEL-BUILD-ID").strip()
    abi = read(inputs, "KERNEL-ABI-VERSION").strip()
    version = read(inputs, "kernel.version").strip()
    modules = sorted({m.strip() for m in read(inputs, "kernel.modules").splitlines() if m.strip()})
    if not build_id:
        sys.exit("gen-system-kext: KERNEL-BUILD-ID is empty")
    if not abi:
        sys.exit("gen-system-kext: KERNEL-ABI-VERSION is empty")
    if not version.replace(".", "").isdigit():
        sys.exit(f"gen-system-kext: kernel.version {version!r} is not a kext version")
    if "kernel" not in modules or len(modules) < 2:
        sys.exit("gen-system-kext: kernel.modules lists no modules")

    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleIdentifier": BUNDLE_ID,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "System Resource Pseudoextension",
        "CFBundlePackageType": "KEXT",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "OSBundleCompatibleVersion": version,
        "OSKernelResource": True,
        "NBSDKernelABIVersion": abi,
        "NBSDKernelBuildID": build_id,
        "NBSDModules": modules,
    }
    contents = os.path.join(out, "System.kext", "Contents")
    os.makedirs(contents, exist_ok=True)
    with open(os.path.join(contents, "Info.plist"), "wb") as f:
        plistlib.dump(info, f, sort_keys=False)
    print(f"System.kext: {BUNDLE_ID} {version}, kernel ABI {abi}, kernel build {build_id}, {len(modules)} built-in modules")


if __name__ == "__main__":
    main(sys.argv)
