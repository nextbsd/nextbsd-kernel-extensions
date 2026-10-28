# nextbsd-kernel-extensions

The FreeBSD-derived kernel extensions of NextBSD: FreeBSD drivers, filesystems and protocols that
ship as Apple-style kext bundles in `/System/Library/Extensions` instead of being compiled into the
kernel. Plan: [E21](https://github.com/nextbsd/nextbsd-userland/wiki/E21-FreeBSD-Module-Kext-Conversion-Plan).

> **This name was reused.** Until October 2026, `nextbsd-kernel-extensions` was the graphics repo,
> now [nextbsd-kernel-extensions-graphics](https://github.com/nextbsd/nextbsd-kernel-extensions-graphics).
> Links such as `nextbsd-kernel-extensions#51` in older issues, PRs and commits mean that repo's
> issue, and GitHub now opens this repo's issue of the same number. Graphics kexts live there;
> NVIDIA's in [nextbsd-kernel-extensions-nvidia](https://github.com/nextbsd/nextbsd-kernel-extensions-nvidia).

## Kexts

| Kext | Arches | From | Notes |
|---|---|---|---|
| `IntelWiFi.kext` | amd64 | `if_iwlwifi.ko` | every iwlwifi model (7260 to BE200), with linux-firmware's iwlwifi set |
| `autofs.kext` | amd64, arm64 | `autofs.ko` | the automounter filesystem; no personalities, loaded on demand by automount |

## Layout

Each kext is one directory named after it, holding a `kext.env` that says which module it is built
from, where its personalities come from and what firmware it bundles. `tools/kexts.sh` reads these
(its header documents the fields); the workflow names no kext, module or source path. To add a kext,
add a directory.

- `tools/kexts.sh`: builds every kext from its directory's data.
- `tools/ko2kext.sh`: wraps a `.ko` in a kext bundle.
- `tools/gen-*-personalities.sh`: IOKitPersonalities from a driver's own PCI id tables.
- `tools/fetch-kernel-sdk.sh`, `tools/stamp-kernel-build-id.py`: build against the published kernel
  SDK and record its `KERNEL-BUILD-ID` in each kext.

The tools were copied from nextbsd-kernel-extensions-graphics at `24bac54`.

## Build and release

```
nextbsd-kernel ──kernel-updated──▶ this repo ──▶ continuous: freebsd-kexts-<arch>.tar.gz
                                                    │
                                                    ▼
                                    nextbsd-pkg: NextBSD-kernel-extensions
```

The modules are built from freebsd-src's `sys/modules/<module>` against the **kernel SDK** from
nextbsd-kernel's `continuous` release (`kernel-sdk-<arch>.tar.gz`, nextbsd-kernel#327): its headers as
`SYSDIR`, its generated options as `KERNBUILDDIR`. Only the modules a kext ships are built, and
nothing needs the kernel's source or obj tree or relinks the kernel. Every kext
carries `NBSDKernelBuildID` in its Info.plist, and packaging and the image lanes refuse kexts built
for another kernel. After a kernel merge, rebuild with a merge here or a manual run of the workflow.
