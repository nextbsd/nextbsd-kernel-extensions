#!/usr/bin/env python3
"""gen-kext-dirs.py — seed one kext directory per FreeBSD module (E21 #179, #3).

usage: gen-kext-dirs.py <repo-root> [<amd64 linker.hints> <arm64 linker.hints>]

Joins three data files under data/:
  module-inventory.tsv  every .ko built for either arch, with per-arch
                        BUILTIN / MODULE / PARTIAL / - (E21 audit 1)
  decisions.tsv         keep / drop / borderline per .ko, with a reason
                        (E21 D13: about 2012+ hardware)
  kext-catalog.tsv      the maintainer's catalog: kext name and bundle ID
                        per .ko (E21 D7)
and writes <KextName>/kext.env for every kept module, per arch:

  * a module that is a module on that arch;
  * a network driver compiled into GENERIC that NextBSD moves out of the
    kernel (E21 D11), when it is kept;
  * never a module compiled into that arch's kernel otherwise.

Autoload rule (E21 D12): a hardware driver ships as a kext on an arch only if
the kernel can match it there. With linker.hints from a FreeBSD release (one
per arch), a driver's PNP buses decide: pci, uhub (USB) and acpi match on both
arches; device-tree buses (simplebus, ofwbus, iicbus, spibus, gpiobus) only on
arm64. A driver with no table, or only on a bus the matcher does not handle
yet (hidbus, virtio, vmbus, ...), is held back on that arch. The kext build
re-checks this against each .ko's own metadata. Libraries and non-hardware
kexts (netgraph, GEOM, protocols, filesystems) load on request and need none.

Names: the catalog's kext name and bundle ID where it has a row; otherwise a
CamelCase name built the catalog's way, listed in data/kext-names-generated.tsv
for review. Filesystems are CamelCase too (D7: "uniformly").

Existing directories are left alone; everything not converted is listed with
its reason in data/not-converted.tsv.
"""
import csv
import os
import re
import struct
import sys
from collections import defaultdict

ARCHES = ("amd64", "arm64")

GROUP = {  # category -> bundle-ID group, for modules the catalog has no row for
    "hwdriver": "driver", "rdma-offload": "driver", "storage-net-target": "driver",
    "geom": "driver", "hw-support-lib": "kpi", "net-pseudo-protocol": "net",
    "netgraph": "net", "tcp-cc-stack": "net", "filesystem": "filesystems",
    "kernel-misc": "kext", "tracing-debug": "kext", "virtualization": "kext",
    "mac-policy": "security", "crypto-sw": "crypto", "abi-compat": "compat",
}
HARDWARE = {"hwdriver", "rdma-offload"}

# Modules not in the inventory (newer drivers releng/15.1 has in source but
# FreeBSD's release does not build): ko -> (sys/modules dir, arches, category).
EXTRA = {
    "if_aq": ("aq", ("amd64",), "hwdriver"),
    "if_rtw88": ("rtw88", ARCHES, "hwdriver"),
    "if_rtw89": ("rtw89", ARCHES, "hwdriver"),
    "if_mt76_core": ("mt76/core", ARCHES, "hw-support-lib"),
    "if_mt7921": ("mt76/mt7921", ARCHES, "hwdriver"),
    "if_mt7925": ("mt76/mt7925", ARCHES, "hwdriver"),
    "if_ath10k": ("ath10k", ARCHES, "hwdriver"),
    "athk_common": ("athk_common", ARCHES, "hw-support-lib"),
    "if_ath11k": ("ath11k", ARCHES, "hwdriver"),
    "if_ath12k": ("ath12k", ARCHES, "hwdriver"),
}

# Network drivers compiled into GENERIC that move out of the kernel (D11).
# Others compiled in stay (vtnet, and anything not kept).
NET_PREFIXES = ("if_",)
NET_STAYS = {"if_vtnet"}

# Built from this repo instead of sys/modules (their directories are curated
# by hand, not generated): the net80211 family (wlan + every plug-in, D11) and
# Realtek's own re driver (D14). Their inventory rows are consumed here.
LOCAL = {"wlan", "wlan_wep", "wlan_tkip", "wlan_ccmp", "wlan_gcmp", "wlan_amrr",
         "wlan_rssadapt", "wlan_xauth", "wlan_acl", "if_re"}
ALWAYS_BOTH = {"if_em", "linuxkpi_wlan"}  # out of the kernel on both arches now

# Hardware-category modules that are libraries other kexts link against (bus
# and support code, no device of their own to match): converted without a
# personality, loaded as a dependency (OSBundleLibraries) of the drivers that
# use them.
LIBRARY = {"uether", "gpiobus", "spibus", "ow", "hgame"}

ACRONYMS = {
    "acpi", "ahci", "amd", "api", "arm", "ata", "bcm", "bpf", "bsd", "can", "car",
    "cc", "cd", "cpu", "crc", "dhcp", "dma", "dns", "dpdk", "dsp", "efi", "ehci",
    "fdt", "fs", "gpio", "gre", "hci", "hda", "hid", "http", "hw", "i2c", "iic",
    "io", "ip", "ip6", "ipsec", "isa", "lpm", "mac", "mii", "mmc", "nat", "nfs",
    "ntb", "nvme", "pci", "pf", "ppp", "pwm", "qat", "raid", "rdma", "rtc", "sas",
    "scsi", "sdt", "smb", "spi", "tcp", "tls", "tpm", "uart", "udf", "udp", "usb",
    "vlan", "vm", "vt", "wmi", "xhci",
}
FS_NAMES = {
    "autofs": "AutoFS", "cd9660_iconv": "CD9660Iconv", "ext2fs": "Ext2FS",
    "msdosfs_iconv": "MSDOSFSIconv", "p9fs": "P9FS", "smbfs": "SMBFS",
    "udf": "UDF", "udf_iconv": "UDFIconv", "virtio_p9fs": "VirtIOP9FS",
    "libiconv": "LibIconv", "libmchain": "LibMChain",
}
# Names camel() would get wrong because the module name runs words together.
FS_NAMES.update({"gpiobus": "GPIOBus", "spibus": "SPIBus", "uether": "USBEthernet",
                 "hgame": "HIDGame"})


def camel(ko):
    toks = [t for t in re.split(r"[_\-]", ko) if t]
    out = []
    for t in toks:
        if t.lower() in ACRONYMS:
            out.append(t.upper())
        elif t[:1].isdigit():
            out.append(t)
        else:
            out.append(t[:1].upper() + t[1:])
    return "".join(out)


def hints_buses(path):
    """.ko basename -> set of PNP buses, from a FreeBSD linker.hints."""
    bus = defaultdict(set)
    if not path:
        return bus
    d = open(path, "rb").read()
    off, kld = 4, None
    while True:
        off = (off + 3) & ~3
        if off + 4 > len(d):
            break
        n, = struct.unpack_from("<i", d, off)
        rec = d[off + 4:off + 4 + n]
        off += 4 + n
        t, = struct.unpack_from("<i", rec, 0)
        if t == 2:  # MDT_MODULE: module name, then kld file name
            n1 = rec[4]
            p = 5 + n1
            kld = rec[p + 1:p + 1 + rec[p]].decode()[:-3]
        elif t == 4 and kld:  # MDT_PNP_INFO: bus name first
            bus[kld].add(rec[5:5 + rec[4]].decode())
    return bus


def autoloads(buses, arch):
    if buses & {"pci", "uhub", "acpi"}:
        return True
    return arch == "arm64" and bool(buses & {"simplebus", "ofwbus", "iicbus",
                                             "spibus", "gpiobus"})


def main():
    root = sys.argv[1]
    hints = {a: hints_buses(sys.argv[2 + i] if len(sys.argv) > 2 + i else None)
             for i, a in enumerate(ARCHES)}
    data = os.path.join(root, "data")

    def tsv(name):
        with open(os.path.join(data, name), newline="") as f:
            return [r for r in csv.reader(f, delimiter="\t") if r and not r[0].startswith("#")]

    inv = tsv("module-inventory.tsv")[1:]
    dec = {r[0]: (r[1], r[2]) for r in tsv("decisions.tsv")[1:]}
    cat = {r[0]: r for r in tsv("kext-catalog.tsv")[1:]}

    rows = {}  # ko -> (moddir, {arch: status}, category, driver_module)
    for r in inv:
        rows[r[0]] = (r[1], {"amd64": r[3], "arm64": r[4]}, r[5], r[7] == "1")
    for ko, (moddir, arches, category) in EXTRA.items():
        rows[ko] = (moddir, {a: ("MODULE" if a in arches else "-") for a in ARCHES},
                    category, category == "hwdriver")

    made, generated, skipped = 0, [], []
    for ko, (moddir, st, category, is_driver) in sorted(rows.items()):
        if ko in LOCAL:
            continue
        decision, why = dec.get(ko, ("drop", "not reviewed"))
        if decision != "keep":
            here = [a for a in ARCHES if st[a] in ("MODULE", "PARTIAL", "BUILTIN")]
            if here and (decision != "drop" or any(st[a] != "BUILTIN" for a in here)):
                skipped.append((ko, " ".join(here), f"{decision}: {why}"))
            continue
        arches = []
        for a in ARCHES:
            s = st[a]
            moving = (ko in ALWAYS_BOTH or (s == "BUILTIN" and ko.startswith(NET_PREFIXES)
                      and ko not in NET_STAYS and category == "hwdriver"))
            if s in ("MODULE", "PARTIAL") or (s != "-" and moving) or (ko in ALWAYS_BOTH):
                if s == "BUILTIN" and not moving:
                    continue
                if ko in LIBRARY:
                    arches.append(a)
                    continue
                if (category in HARDWARE and is_driver and ko in hints[a]
                        and not autoloads(hints[a][ko], a)):
                    skipped.append((ko, a, "held: no ID table the kernel matches on "
                                    f"{a} yet ({'/'.join(sorted(hints[a][ko]))})"))
                    continue
                if (category in HARDWARE and is_driver and hints[a]
                        and ko not in hints[a] and ko not in EXTRA):
                    skipped.append((ko, a, "held: no PNP table, so it cannot autoload "
                                    "(identify- or class-matched; needs a personality)"))
                    continue
                arches.append(a)
        if not arches:
            continue
        c = cat.get(ko)
        if c and c[5]:
            name, bid = c[5], c[4]
            if c[2] == "filesystem" or bid.startswith("org.nextbsd.filesystems."):
                name = FS_NAMES.get(ko, camel(ko))
                bid = "org.nextbsd.filesystems." + name
        else:
            name = FS_NAMES.get(ko, camel(ko))
            bid = f"org.nextbsd.{GROUP.get(category, 'kext')}.{name}"
            generated.append((ko, name, bid))
        d = os.path.join(root, name)
        if os.path.exists(d):
            continue
        os.makedirs(d)
        match = "required" if (category in HARDWARE and is_driver and ko not in LIBRARY) else "none"
        with open(os.path.join(d, "kext.env"), "w") as out:
            out.write(f"# {name}.kext: FreeBSD's {ko} module (sys/modules/{moddir}), category {category}.\n")
            out.write("# Seeded by tools/gen-kext-dirs.py from data/; edit here.\n")
            out.write(f"KEXT_NAME={name}\nKEXT_ID={bid}\n")
            out.write(f'KEXT_ARCHES="{" ".join(arches)}"\n')
            out.write(f"KEXT_MODULE={moddir}\nKEXT_KO={ko}.ko\n")
            out.write(f"KEXT_CATEGORY={category}\nKEXT_MATCH={match}\n")
            out.write('KEXT_PERSONALITIES=""\nKEXT_FIRMWARE=""\n')
        made += 1

    with open(os.path.join(data, "not-converted.tsv"), "w") as out:
        out.write("ko\tarches\treason\n")
        for r in sorted(set(skipped)):
            out.write("\t".join(r) + "\n")
    with open(os.path.join(data, "kext-names-generated.tsv"), "w") as out:
        out.write("# Kext names generated for modules the catalog has no row for (review).\nko\tkext\tbundle_id\n")
        for r in sorted(generated):
            out.write("\t".join(r) + "\n")
    print(f"gen-kext-dirs: {made} kext directories, {len(generated)} generated names, "
          f"{len(set(skipped))} not converted (data/not-converted.tsv)")


if __name__ == "__main__":
    sys.exit(main())
