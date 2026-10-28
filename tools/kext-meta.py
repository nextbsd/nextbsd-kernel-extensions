#!/usr/bin/env python3
"""kext-meta.py — a kext's Info.plist metadata from its module's own metadata
(E21 #4): the dependencies and device-id tables the .ko itself declares, as
FreeBSD's kldxref reads them, instead of a script per driver.

    usage: kext-meta.py <inputs-dir> <kext-name> <personalities-out>

<inputs-dir>/<Name>/ holds each kext's module, `kldxref -d -v -v` output for it
(<Name>.kldxref) and its bundle ID (kext.id), as `kexts.sh inputs` leaves them.

Prints one OSBundleLibraries bundle ID per line: for every MODULE_DEPEND of
this kext's module that another kext in the set provides (by MODULE name). A
dependency no kext provides is the kernel's own and needs no entry.

Writes IOKitPersonalities to <personalities-out> (nothing when the module has
no table the kernel matches), in Darwin's own keys, one personality per kind
of table (nextbsd-kernel#329):

  pci        IOPCIDevice + IOPCIPrimaryMatch (vendor/device), and
             IOPCIClassMatch (value&mask) for class-only entries
  uhub       IOUSBHostDevice + idVendor/idProduct for each vendor/product entry,
             IOUSBHostInterface + bInterfaceClass/SubClass/Protocol for each
             interface-class entry (host-mode tables only)
  acpi       IOACPIPlatformDevice + IONameMatch (the _HID strings)
  simplebus, ofwbus, iicbus, spibus, gpiobus
             IOPlatformDevice + IONameMatch (the compatible strings)
"""
import os
import re
import sys
from xml.sax.saxutils import escape

HDR = re.compile(r"^  pnp info for bus (\S+) format (.*) (\d+) entries of (\d+) bytes$")
DEP = re.compile(r"^  depends on (\S+)\.\d+ \(")
MOD = re.compile(r"^  module (\S+)$")
WILD = 0xFFFFFFFF
FDT_BUSES = {"simplebus", "ofwbus", "iicbus", "spibus", "gpiobus"}
# usb_device_id's match flags (sys/dev/usb/usbdi.h), the M16 "mask" field.
USB_VENDOR, USB_PRODUCT = 0x001, 0x002
USB_INT_CLASS, USB_INT_SUBCLASS, USB_INT_PROTOCOL = 0x080, 0x100, 0x200


def parse(path):
    """Return (modules, depends, pnp) from one kldxref -d -v -v dump.
    pnp is a list of (bus, format, [dict(key -> value), ...])."""
    modules, depends, pnp = [], [], []
    fields = None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = MOD.match(line)
            if m:
                modules.append(m.group(1)); fields = None; continue
            m = DEP.match(line)
            if m:
                depends.append(m.group(1)); fields = None; continue
            m = HDR.match(line)
            if m:
                fields = descriptor(m.group(2))
                pnp.append((m.group(1), m.group(2), []))
                continue
            if line.startswith(("Found type ", "Converting ")):
                continue  # kldxref -v -v tracing its parse of the format
            if fields is not None and line and not line.startswith(" "):
                entry = entry_values(line, fields)
                if entry is not None:
                    # T:key=value fields hold for every entry of the table
                    # (e.g. rtsx's "T:vendor=0x10ec").
                    pnp[-1][2].append({**constants(pnp[-1][1]), **entry})
                else:
                    fields = None
    return modules, depends, [t for t in pnp if t[2]]


def constants(fmt):
    """The T:key=value fields of a PNP format: values every entry shares."""
    out = {}
    for tok in fmt.split(";"):
        if tok.startswith("T:") and "=" in tok:
            key, _, val = tok[2:].partition("=")
            try:
                out[key] = int(val, 0)
            except ValueError:
                out[key] = val
    return out


def descriptor(fmt):
    """The keys kldxref prints values for, in order, from a PNP format string.
    P (pointer) and T (table marker) print nothing; W32 prints two values."""
    keys = []
    for tok in fmt.split(";"):
        if ":" not in tok:
            continue
        typ, key = tok.split(":", 1)
        if typ in ("P", "T"):
            continue
        if typ == "W32":
            a, _, b = key.partition("/")
            keys.append(("W32", (a, b)))
        elif typ in ("D", "Z", "E"):
            keys.append(("S", key))
        else:
            keys.append(("I", key))
    return keys


def entry_values(line, fields):
    out, pos = {}, 0
    for kind, key in fields:
        if kind == "W32":
            m = re.match(r"W32:(0x[0-9a-f]+|0):(0x[0-9a-f]+|0);", line[pos:])
            if not m:
                return None
            out[key[0]] = int(m.group(1), 16)
            out[key[1]] = int(m.group(2), 16)
        elif kind == "I":
            m = re.match(r"I:(-?0x[0-9a-f]+|-?\d+);", line[pos:])
            if not m:
                return None
            out.setdefault(key, int(m.group(1), 0) & WILD)
        else:
            m = re.match(r"[DZE]:([^;]*);", line[pos:])
            if not m:
                return None
            out.setdefault(key, m.group(1))
        pos += m.end()
    return out


def valid(v, width=0xFFFF):
    return v is not None and v not in (WILD, width)


def personalities(name, bundle_id, pnp):
    """[(provider, {key: value}), ...] in Darwin's keys."""
    pci_ids, pci_class, usb_dev, usb_if, acpi, fdt = set(), set(), set(), set(), [], []
    for bus, fmt, entries in pnp:
        if bus == "pci":
            for e in entries:
                v, d = e.get("vendor"), e.get("device")
                if valid(v) and v != 0 and valid(d):
                    pci_ids.add("0x%04x%04x" % (d & 0xFFFF, v & 0xFFFF))
                elif e.get("class") is not None and valid(e["class"], 0xFF):
                    val, mask = (e["class"] & 0xFF) << 24, 0xFF000000
                    if valid(e.get("subclass"), 0xFF):
                        val |= (e["subclass"] & 0xFF) << 16; mask |= 0xFF0000
                    if valid(e.get("progif"), 0xFF):
                        val |= (e["progif"] & 0xFF) << 8; mask |= 0xFF00
                    pci_class.add("0x%08x&0x%08x" % (val, mask))
        elif bus == "uhub" and "mode=device" not in fmt:
            for e in entries:
                fl = e.get("mask", 0)
                if fl & USB_VENDOR and fl & USB_PRODUCT:
                    usb_dev.add((e.get("vendor", 0) & 0xFFFF, e.get("product", 0) & 0xFFFF))
                elif fl & (USB_INT_CLASS | USB_INT_SUBCLASS | USB_INT_PROTOCOL):
                    t = []
                    for flag, key in ((USB_INT_CLASS, "intclass"),
                                      (USB_INT_SUBCLASS, "intsubclass"),
                                      (USB_INT_PROTOCOL, "intprotocol")):
                        t.append(e.get(key, 0) & 0xFF if fl & flag else None)
                    usb_if.add(tuple(t))
        elif bus == "acpi":
            for e in entries:
                hid = e.get("_HID")
                if hid and hid not in acpi:
                    acpi.append(hid)
        elif bus in FDT_BUSES:
            for e in entries:
                c = e.get("compat")
                if c and c not in fdt:
                    fdt.append(c)
    out = []
    if pci_ids or pci_class:
        p = {"IOProviderClass": "IOPCIDevice"}
        if pci_ids:
            p["IOPCIPrimaryMatch"] = " ".join(sorted(pci_ids))
        if pci_class:
            p["IOPCIClassMatch"] = " ".join(sorted(pci_class))
        out.append(p)
    for v, pid in sorted(usb_dev):
        out.append({"IOProviderClass": "IOUSBHostDevice", "idVendor": v, "idProduct": pid})
    for cls, sub, proto in sorted(usb_if, key=lambda t: tuple(-1 if x is None else x for x in t)):
        p = {"IOProviderClass": "IOUSBHostInterface"}
        if cls is not None:
            p["bInterfaceClass"] = cls
        if sub is not None:
            p["bInterfaceSubClass"] = sub
        if proto is not None:
            p["bInterfaceProtocol"] = proto
        out.append(p)
    if acpi:
        out.append({"IOProviderClass": "IOACPIPlatformDevice", "IONameMatch": acpi})
    if fdt:
        out.append({"IOProviderClass": "IOPlatformDevice", "IONameMatch": fdt})
    return out


def plist_value(v, ind):
    if isinstance(v, int):
        return f"{ind}<integer>{v}</integer>\n"
    if isinstance(v, list):
        return (f"{ind}<array>\n" + "".join(f"{ind}\t<string>{escape(s)}</string>\n" for s in v)
                + f"{ind}</array>\n")
    return f"{ind}<string>{escape(str(v))}</string>\n"


def write_personalities(path, name, bundle_id, pers):
    with open(path, "w") as out:
        out.write("\t<key>IOKitPersonalities</key>\n\t<dict>\n")
        for i, p in enumerate(pers):
            key = name if i == 0 else f"{name} {i}"
            out.write(f"\t\t<key>{escape(key)}</key>\n\t\t<dict>\n")
            body = {"CFBundleIdentifier": bundle_id, "IOClass": name, **p,
                    "IOProbeScore": 10000}
            for k, v in body.items():
                out.write(f"\t\t\t<key>{k}</key>\n" + plist_value(v, "\t\t\t"))
            out.write("\t\t</dict>\n")
        out.write("\t</dict>\n")


def main():
    if len(sys.argv) != 4:
        print(__doc__.splitlines()[4].strip(), file=sys.stderr)
        return 2
    inputs, name, pers_out = sys.argv[1:]
    provider = {}  # module name -> bundle ID, across the whole set
    for k in sorted(os.listdir(inputs)):
        dump = os.path.join(inputs, k, f"{k}.kldxref")
        idf = os.path.join(inputs, k, "kext.id")
        if not (os.path.isfile(dump) and os.path.isfile(idf)):
            continue
        with open(idf) as f:
            kid = f.read().strip()
        for mod in parse(dump)[0]:
            provider.setdefault(mod, kid)

    modules, depends, pnp = parse(os.path.join(inputs, name, f"{name}.kldxref"))
    with open(os.path.join(inputs, name, "kext.id")) as f:
        self_id = f.read().strip()
    libs = []
    for dep in depends:
        kid = provider.get(dep)
        if dep in modules or kid is None or kid == self_id or kid in libs:
            continue
        libs.append(kid)
    for kid in libs:
        print(kid)

    pers = personalities(name, self_id, pnp)
    if pers:
        write_personalities(pers_out, name, self_id, pers)
    return 0


if __name__ == "__main__":
    sys.exit(main())
