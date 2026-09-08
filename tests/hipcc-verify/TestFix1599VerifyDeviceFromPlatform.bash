#!/bin/bash
# Reproduces https://github.com/CHIP-SPV/chipStar/issues/1599
#
# chip-kernel-verify must run ocloc against the Intel GPUs this host actually
# has. A hardcoded target is not merely imprecise: on Aurora the built-in
# 'dg2' has no fp64, so every module containing a double fails to compile,
# verification is skipped for it, and the tool checks nothing at all.
#
# The expected device list is read straight from sysfs. ocloc accepts a PCI
# device ID in hex ('-device 0xe20c' resolves to bmg-g21), so the raw IDs are
# usable -device values and neither side needs a mapping table.
set -u

BIN="@CMAKE_BINARY_DIR@/bin/chip-kernel-verify"
# Empty unless this build pinned -DHIPCC_VERIFY_DEVICES, in which case the pin
# wins over detection and that is what the tool must report.
PINNED="@HIPCC_VERIFY_DEVICES@"

if [ -n "${PINNED}" ]; then
  expected=$(echo "${PINNED}" | tr ',' '\n' | sed '/^$/d')
  what="the pinned device list"
else
  # Vendor 0x8086 is Intel; PCI class 0x0300 is "VGA compatible controller"
  # and 0x0380 is "Display controller, other", which is what a headless
  # compute GPU such as a PVC card reports.
  expected=$(for d in /sys/bus/pci/devices/*; do
    [ -r "${d}/vendor" ] || continue
    [ "$(cat "${d}/vendor")" = "0x8086" ] || continue
    case "$(cat "${d}/class")" in
      0x0300*|0x0380*) ;;
      *) continue ;;
    esac
    cat "${d}/device"
  done | awk '!seen[$0]++')
  what="the Intel GPUs present on this host"

  if [ -z "${expected}" ]; then
    echo "HIP_SKIP_THIS_TEST: no Intel GPU on this host and no pinned device list"
    exit 0
  fi
fi

got=$(env -u HIPCC_VERIFY_DEVICES "${BIN}" --print-devices 2>&1)
rc=$?

if [ ${rc} -ne 0 ]; then
  echo "FAIL: --print-devices exited ${rc}: ${got}"
  exit 1
fi

if [ "${got}" != "${expected}" ]; then
  echo "FAIL: chip-kernel-verify would verify against"
  echo "${got:-<nothing>}"
  echo "instead of ${what}"
  echo "${expected}"
  exit 1
fi

echo "PASS: verifying against ${what}: $(echo ${expected})"
exit 0
