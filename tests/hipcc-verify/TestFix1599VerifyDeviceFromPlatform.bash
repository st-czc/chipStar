#!/bin/bash
# Reproduces https://github.com/CHIP-SPV/chipStar/issues/1599
#
# chip-kernel-verify must run ocloc against the GPU the code will run on. The
# target used to be fixed at build time to dg2, and ocloc being a cross
# compiler is exactly what hid that: it builds for dg2 happily on any machine.
# The damage is capability, not reach. Where the fixed target lacks something
# the kernels use, fp64 on Aurora being the reported case, ocloc errors, that
# device is skipped, and the run checks nothing.
#
# So this asserts the -device values ocloc is actually asked for, not what the
# tool says it would use: a regression that reverts only the verification path
# has to go red here. ocloc itself is replaced by a stub that records its
# arguments, which keeps the test off real hardware and off a real compile, and
# means every host asserts something. A host with no Intel GPU asserts that
# nothing was compiled and that the tool said so.
set -u

BIN="@CMAKE_BINARY_DIR@/bin/chip-kernel-verify"
SPV="@CMAKE_CURRENT_SOURCE_DIR@/fixtures/lookback_set_ops.spv"
# Empty unless this build pinned -DHIPCC_VERIFY_DEVICES, in which case the pin
# is what must reach ocloc when the environment says nothing.
PINNED="@HIPCC_VERIFY_DEVICES@"

WORK=$(mktemp -d) || { echo "FAIL: mktemp"; exit 1; }
trap 'rm -rf "${WORK}"' EXIT
LOG="${WORK}/devices.txt"
ERR="${WORK}/stderr.txt"

# Stands in for ocloc. Records each -device it is asked to compile for, then
# reports failure so nothing tries to disassemble a native binary that was
# never produced; the device list is what is under test, not the compile.
cat > "${WORK}/ocloc" <<STUB
#!/bin/bash
[ "\$1" = "--help" ] && exit 0
if [ "\$1" = "compile" ]; then
  while [ \$# -gt 0 ]; do
    [ "\$1" = "-device" ] && echo "\$2" >> "${LOG}"
    shift
  done
fi
exit 1
STUB
chmod +x "${WORK}/ocloc"

# The Intel GPUs sysfs reports, which is what detection must come up with.
# Vendor 0x8086 is Intel; PCI base class 0x03 is "Display controller", which
# covers every sub-class a GPU may report.
#
# The class is matched with a substring test rather than a case statement: the
# bash 3.2 that macOS ships mis-parses a case pattern's ')' inside a command
# substitution and dies with "syntax error near unexpected token".
detected=$(for d in /sys/bus/pci/devices/*; do
  [ -r "${d}/vendor" ] || continue
  [ "$(cat "${d}/vendor")" = "0x8086" ] || continue
  cls=$(cat "${d}/class")
  [ "${cls:0:4}" = "0x03" ] || continue
  cat "${d}/device"
done | awk '!seen[$0]++')

# Run the verifier over the fixture and echo the -device values it asked for.
# $1 is the HIPCC_VERIFY_DEVICES setting, or the word 'unset'. The tool's own
# stderr goes to a file rather than a variable: every call site is a command
# substitution, so a variable set here would not survive the subshell.
ask() {
  : > "${LOG}"
  if [ "$1" = "unset" ]; then
    env -u HIPCC_VERIFY_DEVICES HIPCC_VERIFY_OCLOC="${WORK}/ocloc" \
      "${BIN}" "${SPV}" > /dev/null 2> "${ERR}"
  else
    HIPCC_VERIFY_DEVICES="$1" HIPCC_VERIFY_OCLOC="${WORK}/ocloc" \
      "${BIN}" "${SPV}" > /dev/null 2> "${ERR}"
  fi
  cat "${LOG}"
}

check() { # check <label> <what-was-asked-for> <what-was-expected>
  if [ "$2" != "$3" ]; then
    echo "FAIL: $1: ocloc was asked to compile for"
    echo "${2:-<nothing>}"
    echo "expected"
    echo "${3:-<nothing>}"
    echo "stderr: $(cat "${ERR}")"
    exit 1
  fi
}

# 1. An explicit list wins over both the pin and detection, and reaches ocloc
#    verbatim. Neither of these is a device this host has.
check "explicit list" "$(ask 'dg2,pvc')" "$(printf 'dg2\npvc')"

# 2. Set but empty asks for detection, which is the only way to reach the
#    detection path on a build that pinned a target, so this asserts on every
#    lane including the pinned Aurora one.
got=$(ask '')
if [ -n "${detected}" ]; then
  check "detection" "${got}" "${detected}"
else
  # No Intel GPU and nothing pinned leaves no honest target. Compiling for a
  # guessed part while reporting success is the bug this test exists for, so
  # the tool must compile for nothing and say that it checked nothing.
  check "detection on a host with no Intel GPU" "${got}" ""
  case "$(cat "${ERR}")" in
    *"nothing was verified"*) ;;
    *) echo "FAIL: nothing was verified and the tool did not say so"
       echo "$(cat "${ERR}")"; exit 1 ;;
  esac
fi

# 3. With the environment saying nothing, the configure-time pin decides, and
#    where nothing was pinned that is detection. This is what goes red if the
#    build default returns to a fixed part.
if [ -n "${PINNED}" ]; then
  expected=$(echo "${PINNED}" | tr ',' '\n' | sed '/^$/d')
  if [ -z "${expected}" ]; then
    echo "FAIL: HIPCC_VERIFY_DEVICES is pinned to '${PINNED}' but names no device"
    exit 1
  fi
  check "configure-time pin" "$(ask unset)" "${expected}"
else
  check "no pin, so detection" "$(ask unset)" "${detected}"
fi

# The query flag has to agree with the path that does the work, or it is a
# second source of truth that can drift away from it unnoticed.
printed=$(env -u HIPCC_VERIFY_DEVICES "${BIN}" --print-devices 2>/dev/null)
check "--print-devices" "${printed}" "$(ask unset)"

echo "PASS: ocloc verified against ${detected:-no target, and said so}"
exit 0
