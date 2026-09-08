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
mode=\$(cat "${WORK}/stub-mode")
op=\$1
out=""; dump=""
while [ \$# -gt 0 ]; do
  case "\$1" in
    -device) [ "\$op" = compile ] && echo "\$2" >> "${LOG}" ;;
    -output) out="\$2" ;;
    -dump) dump="\$2" ;;
  esac
  shift
done
# 'reject' fails the compile, which is the ocloc-cannot-build-this path.
[ "\$mode" = reject ] && exit 1
# 'drop' succeeds but hands back a binary with no kernels in it, which is the
# IGC #403 shape the tool exists to catch.
if [ "\$op" = compile ]; then : > "\${out}.bin"; exit 0; fi
if [ "\$op" = disasm ]; then mkdir -p "\$dump"; : > "\$dump/sections.txt"; exit 0; fi
exit 0
STUB
chmod +x "${WORK}/ocloc"
echo reject > "${WORK}/stub-mode"

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
  echo $? > "${WORK}/rc"
  cat "${LOG}"
}

# The exit status is what hipcc acts on, so nothing above is worth much without
# it: a run that reports success having checked nothing is the whole bug.
expect_rc() { # expect_rc <label> <wanted>
  got_rc=$(cat "${WORK}/rc")
  if [ "${got_rc}" != "$2" ]; then
    echo "FAIL: $1: chip-kernel-verify exited ${got_rc}, expected $2"
    echo "stderr: $(cat "${ERR}")"
    exit 1
  fi
}

expect_stderr() { # expect_stderr <label> <substring>
  case "$(cat "${ERR}")" in
    *"$2"*) ;;
    *) echo "FAIL: $1: stderr does not mention '$2'"
       cat "${ERR}"
       exit 1 ;;
  esac
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

# The shipped configure-time default has to stay empty, or an unpinned build is
# back to a fixed target. It is read out of the top-level CMakeLists rather than
# out of the configured value, because a default of "dg2" and a deliberate
# -DHIPCC_VERIFY_DEVICES=dg2 reach this script as the same string.
if ! grep -q '^set(HIPCC_VERIFY_DEVICES "" CACHE STRING' "@CMAKE_SOURCE_DIR@/CMakeLists.txt"; then
  echo "FAIL: the HIPCC_VERIFY_DEVICES default is no longer empty, so a build"
  echo "      that pins nothing verifies against a fixed part again"
  grep -n 'set(HIPCC_VERIFY_DEVICES' "@CMAKE_SOURCE_DIR@/CMakeLists.txt"
  exit 1
fi

# 1. An explicit list wins over both the pin and detection, and reaches ocloc
#    verbatim. Neither of these is a device this host has, and neither can be
#    checked by the stub, so a list that was asked for and produced no coverage
#    has to fail rather than pass quietly.
check "explicit list" "$(ask 'dg2,pvc')" "$(printf 'dg2\npvc')"
expect_rc "explicit list that could not be checked" 1
expect_stderr "explicit list that could not be checked" "verified"

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
  expect_stderr "no Intel GPU" "nothing was verified"
fi
# A target nobody asked for and that could not be checked must not fail the
# build: an ocloc with no entry for the GPU in this machine would otherwise
# stop the machine building chipStar at all.
expect_rc "detected target that could not be checked" 0

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

# With a target that does check out, the run has to report the kernels IGC
# dropped and fail on them, and it must not claim it verified nothing. The stub
# hands back a native binary with no kernels in it, which is that shape.
echo drop > "${WORK}/stub-mode"
ask 'dg2' > /dev/null
expect_rc "kernels dropped, fail mode" 1
expect_stderr "kernels dropped" "missing from native binary"
case "$(cat "${ERR}")" in
  *"nothing was verified"*)
    echo "FAIL: a device that was checked was counted as unchecked"
    cat "${ERR}"; exit 1 ;;
esac

echo "PASS: ocloc verified against ${detected:-no target, and said so}"
exit 0
