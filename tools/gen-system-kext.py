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
  CFBundleVersion     the kernel's release (newvers.sh REVISION, "15.1"), also
                      its OSBundleCompatibleVersion, as com.apple.kernel's is
  NBSDKernelBuildID   the exact kernel build (nextbsd-kernel#326). An OSKext
                      version can't hold <commit>.<run_id>.<attempt>, so the
                      build is matched by this key: OSKext refuses a kext whose
                      NBSDKernelBuildID differs from System.kext's
  NBSDModules         every module version compiled into the kernel (the
                      SDK's kernel.modules), the key the kexts use for the
                      names they provide, so kextd's module-name index
                      resolves a built-in module to org.nextbsd.kernel

From <inputs-dir>: KERNEL-BUILD-ID, kernel.modules and kernel.version, which
the modules job copies from the kernel SDK. Writes <out-dir>/System.kext.
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
    version = read(inputs, "kernel.version").strip()
    modules = sorted({m.strip() for m in read(inputs, "kernel.modules").splitlines() if m.strip()})
    if not build_id:
        sys.exit("gen-system-kext: KERNEL-BUILD-ID is empty")
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
        "NBSDKernelBuildID": build_id,
        "NBSDModules": modules,
    }
    contents = os.path.join(out, "System.kext", "Contents")
    os.makedirs(contents, exist_ok=True)
    with open(os.path.join(contents, "Info.plist"), "wb") as f:
        plistlib.dump(info, f, sort_keys=False)
    print(f"System.kext: {BUNDLE_ID} {version}, kernel {build_id}, {len(modules)} built-in modules")


if __name__ == "__main__":
    main(sys.argv)
