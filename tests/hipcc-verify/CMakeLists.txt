# Tests for chip-kernel-verify (HIPCC_VERIFY post-compile check).

# The IGC intel/intel-graphics-compiler#403 reproducer
# (hipcc_verify_detects_dropped_kernels) has been retired: current Intel
# drivers (ocloc 26.22.38646.4 and newer) compile the lookback_set_ops.spv
# fixture without dropping any kernels, so the verifier exits 0 and the
# WILL_FAIL reproducer can no longer pass. The fixture is kept as a generic
# real-world SPIR-V input for the off-mode sanity check below.

# Sanity: off mode must produce no output and exit 0 regardless of input.
add_test(NAME hipcc_verify_off_mode_silent
  COMMAND ${CMAKE_BINARY_DIR}/bin/chip-kernel-verify
          ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/lookback_set_ops.spv)
set_tests_properties(hipcc_verify_off_mode_silent PROPERTIES
  ENVIRONMENT "HIPCC_VERIFY=0"
  FAIL_REGULAR_EXPRESSION "missing from native binary|ocloc"
  TIMEOUT 30)

# The ocloc target must come from the GPUs present on this host, not a
# hardcoded one that may be absent and may lack a capability the code under
# test uses. See https://github.com/CHIP-SPV/chipStar/issues/1599
configure_file(TestFix1599VerifyDeviceFromPlatform.bash
  TestFix1599VerifyDeviceFromPlatform.bash @ONLY)
add_test(NAME TestFix1599VerifyDeviceFromPlatform
  COMMAND /bin/bash
          ${CMAKE_CURRENT_BINARY_DIR}/TestFix1599VerifyDeviceFromPlatform.bash)
set_tests_properties(TestFix1599VerifyDeviceFromPlatform PROPERTIES
  PASS_REGULAR_EXPRESSION "PASS"
  FAIL_REGULAR_EXPRESSION "FAIL"
  SKIP_REGULAR_EXPRESSION "HIP_SKIP_THIS_TEST"
  TIMEOUT 60)
