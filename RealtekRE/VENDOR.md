# RealtekRE: vendored driver

- **What:** Realtek's official FreeBSD driver for its PCIe Ethernet controllers,
  1G to 10G: RTL8111/8168, RTL8125 (2.5G), RTL8126 (5G), RTL8127 (10G).
- **From:** the FreeBSD port `net/realtek-re-kmod` 1102.01, which builds
  https://github.com/alexdupre/rtl_bsd_drv at commit `cea3599` (Realtek's
  release with the port maintainer's stability patches).
- **License:** BSD-4-clause, in the header of `src/if_re.c`. Its advertising
  clause stays with the source.
- **Files:** `src/if_re.c`, `src/if_rereg.h`, `src/Readme.txt` and
  `src/Makefile.upstream` are upstream at `cea3599`, except one NextBSD change:
  - `nextbsd-pnp.patch`: a `MODULE_PNP_INFO` over the driver's own `re_devs[]`
    table, so the kext carries its device ids as PNP metadata, gets a generated
    PCI personality and autoloads (E21 D12). Without it the driver could only be
    loaded by hand.
- **Replaces base re(4)** (E21 D14): both are the `re` driver, so only one can
  exist. The kernel no longer compiles base `re` in.

To update: copy the new upstream files over `src/`, re-apply
`nextbsd-pnp.patch`, and update the commit and version above.
