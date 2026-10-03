#!/bin/sh
# /usr/tests/nextbsd/kext/run.sh — the kext GPU validation (T4).
#
# The kext image is the nextbsd `continuous` image with the graphics stack
# injected (the FreeBSD-VM mutation drops the kexts into /System/Library/
# Extensions). kextd auto-loads on boot any GPU kext whose personality matches
# a present device -- no explicit kextload. The shared harness boots the amd64
# image on q35's DEFAULT VGA (a stdvga device), so the binding GPU kext is
# BochsGraphics. (The virtio-gpu trio, VirtIOGraphics, binds only on a
# -device virtio-gpu-pci machine, which the shared amd64 lane does not use.)
#
# We validate the load with kextstat only, then emit the sentinel the shared
# harness reads as the LAST line. A -FAIL means the GPU kext did not come up
# (a real graphics regression); the harness's fail-gates policy turns that into
# a non-zero boot rc.
set -u
ok=0
fail=0
skip=0

if kextstat 2>/dev/null | grep -qi bochs; then
    echo "BOCHS-OK: BochsGraphics is loaded (q35 stdvga bound)"
    kextstat 2>/dev/null | grep -i bochs || true
    ok=$((ok + 1))
else
    echo "BOCHS-FAIL: no bochs/stdvga GPU kext in kextstat"
    kextstat 2>/dev/null || true
    fail=$((fail + 1))
fi

echo "NEXTBSD-KEXT-SUITE-DONE"
echo "NEXTBSD-TEST-SUMMARY ok=$ok fail=$fail skip=$skip"
