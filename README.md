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

About 280 kexts across amd64 and arm64, one directory each, named after the kext (`IntelEM/`,
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
  ACPI `_HID`, device tree). Drivers that cannot autoload yet are held back and listed in
  `data/not-converted.tsv`. The build fails if a hardware kext ends up without one.
- **Network drivers are kexts** (E21 D11): the modern PCI NICs moved out of the kernel, the
  net80211 stack is the `Wlan` family kext, and `RealtekRE` is Realtek's own driver, vendored
  (E21 D14, see `RealtekRE/VENDOR.md`). `if_vtnet` and the arm64 board NICs stay in the kernel.
- **Libraries and request-loaded kexts** (netgraph, GEOM, filesystems, packet filters, TCP stacks,
  MAC policies) carry no personalities; they load as dependencies or on request.

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
- `tools/fetch-kernel-sdk.sh`, `tools/stamp-kernel-build-id.py`: build against the published kernel
  SDK and record its `KERNEL-BUILD-ID` in each kext.

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
Every kext carries `NBSDKernelBuildID` in its Info.plist, and packaging and the image lanes refuse
kexts built for another kernel. After a kernel merge, rebuild with a merge here or a manual run of
the workflow.
