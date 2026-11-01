# nextbsd-kernel-extensions

The FreeBSD-derived kernel extensions of NextBSD: FreeBSD drivers, filesystems, protocols and support
code that ship as Apple-style kext bundles in `/System/Library/Extensions` instead of being compiled
into the kernel. Plan: [E21](https://github.com/nextbsd/nextbsd-userland/wiki/E21-FreeBSD-Module-Kext-Conversion-Plan).

> **This name was reused.** Until October 2026, `nextbsd-kernel-extensions` was the graphics repo,
> now [nextbsd-kernel-extensions-graphics](https://github.com/nextbsd/nextbsd-kernel-extensions-graphics).
> Links such as `nextbsd-kernel-extensions#51` in older issues, PRs and commits mean that repo's
> issue, and GitHub now opens this repo's issue of the same number. Graphics kexts live there;
> NVIDIA's in [nextbsd-kernel-extensions-nvidia](https://github.com/nextbsd/nextbsd-kernel-extensions-nvidia).

## What is here

125 kexts (90 on amd64, 101 on arm64), one directory each, named after the kext (`IntelEM/`,
`RealtekRE/`, `Wlan/`, `AutoFS/`, …). Which modules are converted, and what each kext is called, is
decided by data, not by the workflow:

| File | What it holds |
|---|---|
| `data/module-inventory.tsv` | every `.ko` either arch builds, and whether each arch compiles it in (E21 audit 1) |
| `data/decisions.tsv` | keep / drop / borderline per module, with a reason (E21 D13: about 2012-and-newer hardware) |
| `data/kext-catalog.tsv` | the maintainer's catalog: kext name and bundle ID per module (E21 D7, `org.nextbsd.<group>.<Name>`) |
| `data/kext-names-generated.tsv` | names generated, the catalog's way, for modules the catalog has no row for |
| `data/not-converted.tsv` | every module not converted, and why |

`tools/gen-kext-dirs.py` seeds a directory per kept module from those files; after that the
directories are what gets edited. To add a kext, add a directory (or flip its row in
`data/decisions.tsv` and re-run the generator).

### The rules

- **A hardware driver ships as a kext only if it autoloads** on the hardware it supports (E21 D12):
  it needs a personality the in-kernel IOKit matcher handles (PCI vendor/device and class, USB,
  ACPI `_HID`, device tree, HID usage and vendor/product, virtio and VMBus). Drivers that cannot
  autoload yet are held back and listed in
  `data/not-converted.tsv`. The build fails if a hardware kext ends up without one.
- **Network drivers are kexts** (E21 D11): the modern PCI NICs moved out of the kernel, the
  net80211 stack is the `Wlan` family kext, and `RealtekRE` is Realtek's own driver, vendored
  (E21 D14, see `RealtekRE/VENDOR.md`). `if_vtnet` and the arm64 board NICs stay in the kernel.
  `Wlan` carries NextBSD's net80211 changes in `Wlan/src/` (patches applied to a copy at build
  time): each radio is published in the IORegistry as an `IO80211Controller` entry, which
  wland matches (nextbsd-userland#502).
- **Every kext has something that loads it** (E21 kext loaders review, 2026-10-28). The kernel
  doesn't load modules by name, so a kext ships only if IOKit matching, another kext's
  `OSBundleLibraries`, or a planned userland loader brings it in: AutoFS through automount, the
  GEOM layers through partition matching (nextbsd-kernel#364), the Bluetooth stack's upper layers
  through the future `bluetoothd`. What xnu compiles in (bridge, lagg, IPsec, pf, DTrace, vmm, UDF,
  iconv) is compiled into NextBSD's kernel instead, and the rest was dropped; `data/decisions.tsv`
  gives each reason.

- **`OSBundleRequired` by role, as Mac OS X 10.9 tags its kexts** (`data/osbundlerequired.tsv`): storage
  controllers and GEOM layers are `Local-Root`, NICs and Wi-Fi `Network-Root`, keyboards, mice and
  trackpads `Console`, the SMBus family `Root`; SMBus controllers, SMC and sensors, backlight,
  Bluetooth and AutoFS `Safe Boot`. The rest carry no key. Nothing reads it yet: it is for a pre-root
  load path, such as the boot loader preloading a ZFS-root kext (which would add `Root`).

## Layout of a kext directory

Each directory holds a `kext.env`, which `tools/kexts.sh` reads (its header documents every field):
the kext name and bundle ID, its arches, the `sys/modules/<module>` it is built from (or `local`,
built from the directory's own `Makefile`), its category, whether it must match hardware, and any
firmware it bundles. The workflow names no kext, module or source path.

Each kext's `IOKitPersonalities` and `OSBundleLibraries` come from its module's own metadata:
`kldxref` reads the `.ko`'s `MODULE_PNP_INFO` and `MODULE_DEPEND`, and `tools/kext-meta.py` turns
them into Darwin's keys (`IOPCIPrimaryMatch`, `IOPCIClassMatch`, `IOUSBHostDevice`,
`IOUSBHostInterface`, `IOACPIPlatformDevice`, `IOPlatformDevice` + `IONameMatch`). A hand-written
generator can still be named in `kext.env` (IntelIwlwifi keeps its own).

Each Info.plist also says what the kext provides by name, for kextd's module-name index (NextBSD's
`linker.hints`): `NBSDModuleFile` is the `.ko` it wraps (`if_em.ko`), and `NBSDModules` lists the
module and `MODULE_VERSION` interface names in it (`wlan`, `wlan_ccmp`, …), the names `kldload`
and `MODULE_DEPEND` look modules up by.

Tools:

- `tools/gen-kext-dirs.py`: seeds the directories from `data/`.
- `tools/kexts.sh`: builds every kext from its directory's data.
- `tools/kext-meta.py`: personalities and dependencies from a module's metadata.
- `tools/ko2kext.sh`: wraps a `.ko` in a kext bundle.
- `tools/check-kext-symbols.py`: fails the build if a kext would not resolve at kldload.
- `tools/gen-iwlwifi-personalities.sh`: IntelIwlwifi's own personalities.
- `tools/gen-geom-personalities.sh`: the GEOM layers' personalities (GeomEli, GeomMirror,
  GeomStripe, GeomConcat, GeomMultipath): `IOProviderClass` `IOMedia` with the class's metadata
  magic (`GEOM::ELI`, read from its `G_*_MAGIC` define) as `Content Mask`, the keys CoreStorage
  and AppleRAID match on Darwin. The kernel offers each disk's GEOM metadata and GPT type to the
  matcher, so the layer loads when a disk carrying it appears (nextbsd-kernel#364).
- `tools/fetch-kernel-sdk.sh`, `tools/stamp-kernel-abi.py`: build against the published kernel
  SDK and record its kext ABI version (`NBSDKernelABIVersion`) and `KERNEL-BUILD-ID` in each kext.
- `tools/gen-system-kext.py`: writes `System.kext`.

## System.kext

The set also ships a codeless **`System.kext`** (`org.nextbsd.kernel`), generated per kernel build
from the kernel SDK, as Darwin describes its kernel with System.kext (`com.apple.kernel`). It has an
Info.plist and no code: `OSKernelResource` marks it as part of the kernel, so OSKext never loads it
and a kext can name it in `OSBundleLibraries` for code compiled into the kernel. It carries:

- `CFBundleVersion`, `CFBundleShortVersionString` and `OSBundleCompatibleVersion`: the kernel
  version, the SDK's `KERNEL-VERSION` (`27.0.0`, nextbsd-kernel#376). An SDK published before
  that has none, and the kernel's release (`REVISION`, `15.1`) is used;
- `NBSDKernelABIVersion`: the kext ABI version, the SDK's `KERNEL-ABI-VERSION` (`27.0`, the
  kernel version's major.minor), which every kext in the set carries too. OSKext compares a
  kext's with the running kernel's (`kern.osrelease` major.minor, or System.kext's on an older
  kernel) and refuses a mismatch before it kldloads anything. A kernel release that keeps the ABI
  (27.0.x) needs no kext rebuild;
- `NBSDKernelBuildID`: the legacy key, the SDK's `KERNEL-BUILD-ID` whatever it holds. OSKext
  falls back to it for a kext without `NBSDKernelABIVersion`, and the pinned nextbsd-ci and
  nextbsd-pkg compare it with the kernel's `KERNEL-BUILD-ID`;
- `NBSDModules`: every module version compiled into the kernel (the SDK's `kernel.modules`), so
  kextd's module-name index resolves a built-in module to `org.nextbsd.kernel`.

## Build and release

```
nextbsd-kernel ──kernel-updated──▶ this repo ──▶ continuous: freebsd-kexts-<arch>.tar.gz
                                                    │
                                                    ▼
                                    nextbsd-pkg: NextBSD-kernel-extensions
```

The modules are built from freebsd-src's `sys/modules` (one `make -k` pass with `MODULES_OVERRIDE`
set to just the kexts' modules) against the **kernel SDK** from nextbsd-kernel's `continuous`
release (`kernel-sdk-<arch>.tar.gz`, nextbsd-kernel#327): its headers as `SYSDIR`, its generated
options as `KERNBUILDDIR`. Nothing needs the kernel's source or obj tree or relinks the kernel.
Every kext carries `NBSDKernelABIVersion` and `NBSDKernelBuildID` in its Info.plist, and packaging
and the image lanes refuse kexts built for another kernel. Until the SDK publishes
`KERNEL-ABI-VERSION` (nextbsd-kernel#376), `fetch-kernel-sdk.sh` uses its `KERNEL-BUILD-ID` for
`NBSDKernelABIVersion` too. After a kernel merge, rebuild with a merge here or a manual run of
the workflow.
