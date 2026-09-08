// chip-kernel-verify
//
// Post-compile verifier invoked by hipcc (via HIPCC_VERIFY).
//
// Extracts SPIR-V from a hipcc output (object or linked executable), lists the
// OpEntryPoint Kernel names, then runs ocloc for each requested Intel GPU
// target and diffs against the kernels present in the native binary. Mismatch
// means IGC dropped entry points (most commonly intel/intel-graphics-compiler
// issue #403, SIMD32 register-pressure path).
//
// Env:
//   HIPCC_VERIFY           0 = skip; warn = print only; anything else = fail on mismatch.
//                          Defaults to CHIP_KERNEL_VERIFY_DEFAULT_MODE at build time.
//   HIPCC_VERIFY_DEVICES   comma-separated -device values for ocloc, replacing
//                          the list pinned at configure time. Set but empty
//                          asks for detection; unset falls back to the pin, and
//                          to detection when nothing was pinned.
//   HIPCC_VERIFY_OCLOC     override ocloc binary (default: ocloc).
//   HIPCC_VERIFY_SYSFS_ROOT  where to look for PCI devices, for tests that need
//                          an inventory this machine does not have.
//
// Args:
//   --print-devices        print the -device values that would be used, one
//                          per line, and exit. Nothing is compiled.
//
// ocloc is a cross compiler and will happily build for a part that is not in
// the machine, so the target is not about what ocloc can reach. It is about
// capability: a target with fewer capabilities than the GPU the code will run
// on fails to compile, verifyOneDevice skips it, and the run checks nothing.
// That is what made the old fixed dg2 target inert on a host whose kernels use
// fp64. See https://github.com/CHIP-SPV/chipStar/issues/1599

#include "spirv-extractor.hh"

// Note: Elf64_Ehdr / Elf64_Shdr / ELFMAG / SELFMAG are defined portably
// (Linux + macOS) by spirv-extractor.hh, included above.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// Set from -DHIPCC_VERIFY_DEVICES at configure time. Empty means the device
// list comes from the host instead of from the build.
#ifndef CHIP_KERNEL_VERIFY_DEFAULT_DEVICES
#define CHIP_KERNEL_VERIFY_DEFAULT_DEVICES ""
#endif

namespace fs = std::filesystem;

namespace {

enum class Mode { Off, Warn, Fail };

Mode readMode() {
  const char *env = std::getenv("HIPCC_VERIFY");
  if (!env) return Mode::Fail;
  std::string v(env);
  if (v == "0" || v == "off" || v == "OFF") return Mode::Off;
  if (v == "warn" || v == "WARN") return Mode::Warn;
  return Mode::Fail;
}

std::vector<std::string> split(const std::string &s, char delim) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == delim) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// The Intel GPUs this host has, as ocloc -device values.
//
// ocloc takes a PCI device ID in hex and resolves it itself ("-device 0xe20c"
// prints "Auto-detected target based on 0xe20c device id: bmg-g21"), so the
// IDs sysfs reports are usable -device values and no product table is needed
// here. An id resolves to the base stepping ocloc associates with it, which
// need not be the stepping installed (0x0bd5 resolves to pvc-xt-a0), so pin
// HIPCC_VERIFY_DEVICES where a specific stepping matters.
//
// Vendor 0x8086 is Intel. The class is matched on the PCI base class
// 0x03, "Display controller", rather than on a sub-class: which sub-class a
// GPU reports varies (0x0300 VGA compatible, 0x0302 3D, 0x0380 other) and a
// headless compute card need not report the one a desktop card does.
//
// Returns empty where there is no /sys to read, which includes macOS.
//
// The directory is a parameter so the rule above can be checked against a
// fixture holding devices this machine does not have: two cards of one model,
// a second model, a non-Intel card and a non-display Intel device all in one
// tree. Nothing in a build sets it.
std::vector<std::string> detectHostDevices(const fs::path &root) {
  std::vector<std::string> devices;
  std::error_code ec;
  fs::directory_iterator it(root, ec);
  if (ec) return devices;

  auto readField = [](const fs::path &p) {
    std::ifstream in(p);
    std::string v;
    std::getline(in, v);
    return v;
  };

  // directory_iterator order is unspecified, so sort the PCI addresses to keep
  // the device list stable across runs on a multi-GPU host. Increment through
  // the error_code overload so that a directory that cannot be walked ends the
  // whole run in "nothing was verified" rather than throwing out of a compile,
  // and rather than silently verifying whatever the walk reached first.
  std::vector<fs::path> entries;
  for (const fs::directory_iterator end; it != end; it.increment(ec)) {
    if (ec) return {};
    entries.push_back(it->path());
  }
  // An increment can fail and land on end in one step, which the check above
  // would never see; a partial list would silently narrow what gets verified.
  if (ec) return {};
  std::sort(entries.begin(), entries.end());

  for (const auto &dev : entries) {
    if (readField(dev / "vendor") != "0x8086") continue;
    if (readField(dev / "class").compare(0, 4, "0x03") != 0) continue;
    std::string id = readField(dev / "device");
    if (id.empty()) continue;
    // Two cards of the same model compile identically; one run covers both.
    if (std::find(devices.begin(), devices.end(), id) == devices.end())
      devices.push_back(id);
  }
  return devices;
}

// The resolved targets, and whether they were asked for rather than found. An
// empty list from a setting that was given is a different mistake from an
// empty list on a host with no GPU, and the two need different diagnostics.
struct DeviceList {
  std::vector<std::string> Devices;
  bool Requested = false;
};

// Precedence: the HIPCC_VERIFY_DEVICES environment variable, then the list
// pinned at configure time, then the GPUs found on this host. There is no
// guessed target behind those: verifying against a part nobody asked for and
// nobody has is the inert verification this tool exists to stop.
DeviceList resolveDevices() {
  const char *Env = std::getenv("HIPCC_VERIFY_DEVICES");
  // Setting the variable replaces the configure-time list outright, so setting
  // it to nothing asks for detection. That is the only way to get detection out
  // of a build that pinned a target, and it is what the test uses to assert
  // detection on a pinned build.
  std::string devsEnv = Env ? Env : CHIP_KERNEL_VERIFY_DEFAULT_DEVICES;
  if (!devsEnv.empty()) return {split(devsEnv, ','), true};

  const char *Root = std::getenv("HIPCC_VERIFY_SYSFS_ROOT");
  return {detectHostDevices(Root ? Root : "/sys/bus/pci/devices"), false};
}

// A run that checked nothing, said the same way wherever it is reached. A
// target that was asked for, by HIPCC_VERIFY_DEVICES or by the configure-time
// pin, and could not be checked is a failure of something the user set up. A
// target that was only detected is not: an ocloc with no entry for the GPU in
// this machine must not be able to stop the machine building chipStar at all.
int reportNoCoverage(const DeviceList &resolved, Mode mode,
                     const std::string &why) {
  std::cerr << "[chip-kernel-verify] " << why
            << " — nothing was verified. Set HIPCC_VERIFY_DEVICES to an ocloc"
               " -device value to check against a specific target.\n";
  return resolved.Requested && mode == Mode::Fail ? 1 : 0;
}

std::string oclocBin() {
  const char *e = std::getenv("HIPCC_VERIFY_OCLOC");
  return e ? std::string(e) : std::string("ocloc");
}

bool oclocAvailable() {
  std::string cmd = oclocBin() + " --help > /dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

// Run a shell command; capture combined stdout+stderr and return exit code.
int runCapture(const std::string &cmd, std::string &out) {
  std::string full = cmd + " 2>&1";
  FILE *p = popen(full.c_str(), "r");
  if (!p) return -1;
  char buf[4096];
  while (size_t n = fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
  int rc = pclose(p);
  if (WIFEXITED(rc)) return WEXITSTATUS(rc);
  return -1;
}

// Extract OpEntryPoint Kernel names from SPIR-V disassembly text.
// Disassembled form: `OpEntryPoint Kernel %<id> "<name>" ...`
std::set<std::string> scanEntryPoints(const std::string &spvText) {
  std::set<std::string> names;
  static const std::regex re(R"(OpEntryPoint\s+Kernel\s+%\S+\s+\"([^\"]+)\")");
  std::istringstream iss(spvText);
  std::string line;
  while (std::getline(iss, line)) {
    std::smatch m;
    if (std::regex_search(line, m, re)) names.insert(m[1].str());
  }
  return names;
}

// Parse the sections.txt produced by `ocloc disasm ... -dump <dir>`; collect
// kernel names from `.text.<name>, 1` lines.
std::set<std::string> scanNativeKernels(const fs::path &sectionsFile) {
  std::set<std::string> names;
  std::ifstream in(sectionsFile);
  std::string line;
  static const std::regex re(R"(^\.text\.([^,]+),)");
  while (std::getline(in, line)) {
    std::smatch m;
    if (std::regex_search(line, m, re)) names.insert(m[1].str());
  }
  return names;
}

// Whether this device got as far as diffing kernel names, and how many
// mismatches that diff counted. A device that never got there checked nothing,
// which the caller has to know: a run where no device got there is a run that
// verified nothing at all.
struct DeviceResult {
  bool Compared = false;
  int Failures = 0;
};

DeviceResult verifyOneDevice(const fs::path &spvFile, const std::string &device,
                             const std::set<std::string> &spvNames, Mode mode,
                             const fs::path &workDir) {
  fs::path outBase = workDir / ("native_" + device);
  std::string cmd = oclocBin() + " compile -spirv_input"
                    + " -file " + spvFile.string()
                    + " -device " + device
                    + " -output_no_suffix -output " + outBase.string();
  std::string log;
  int rc = runCapture(cmd, log);
  fs::path binFile = outBase.string() + ".bin";
  if (rc != 0 || !fs::exists(binFile)) {
    // An id this ocloc has no target for is not a compile error at all, and the
    // full ocloc log says nothing useful about it. It happens for a GPU older
    // than the ocloc in use, whose id detection reports faithfully and ocloc
    // then rejects, so answer the question the user actually has.
    if (log.find("Could not determine device target") != std::string::npos) {
      std::cerr << "[chip-kernel-verify] ocloc has no target for device '"
                << device << "', so it was not checked. Set"
                   " HIPCC_VERIFY_DEVICES to an ocloc -device value, or"
                   " HIPCC_VERIFY=0 to turn this off.\n";
      return {};
    }
    // ocloc explicitly errored (e.g. fp64 not supported on this device) — that
    // is a real but ordinary compile error, not the silent-drop class of
    // problem this tool is here to detect (intel-graphics-compiler#403).
    // Print the diagnostics for visibility and skip; do not fail the build.
    std::cerr << "[chip-kernel-verify] ocloc compile errored for device '"
              << device << "' (exit " << rc << ") — not an IGC #403 drop;"
              << " skipping verification for this device:\n" << log << "\n";
    return {};
  }

  fs::path dumpDir = workDir / ("dump_" + device);
  fs::create_directories(dumpDir);
  std::string dcmd = oclocBin() + " disasm -file " + binFile.string()
                     + " -device " + device
                     + " -dump " + dumpDir.string();
  std::string dlog;
  int drc = runCapture(dcmd, dlog);
  fs::path sectionsFile = dumpDir / "sections.txt";
  if (!fs::exists(sectionsFile)) {
    std::cerr << "[chip-kernel-verify] ocloc disasm produced no sections.txt for '"
              << device << "' (exit " << drc << "); skipping diff\n";
    return {};
  }

  auto native = scanNativeKernels(sectionsFile);
  std::vector<std::string> missing;
  for (const auto &n : spvNames)
    if (!native.count(n)) missing.push_back(n);

  if (missing.empty()) return {true, 0};

  std::cerr << "[chip-kernel-verify] device=" << device
            << ": " << missing.size() << " kernel(s) missing from native binary"
            << " (IGC likely dropped them; see"
               " https://github.com/intel/intel-graphics-compiler/issues/403 ):\n";
  for (const auto &m : missing) std::cerr << "    - " << m << "\n";

  return {true, mode == Mode::Fail ? 1 : 0};
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc > 1 && std::strcmp(argv[1], "--print-devices") == 0) {
    if (argc != 2) {
      std::cerr << "usage: chip-kernel-verify --print-devices\n";
      return 2;
    }
    for (const auto &d : resolveDevices().Devices) std::cout << d << "\n";
    return 0;
  }

  Mode mode = readMode();
  if (mode == Mode::Off) return 0;

  if (argc < 2) {
    std::cerr << "usage: chip-kernel-verify <compiled-output> | --print-devices\n";
    return 0; // don't fail user's build over a missing arg
  }
  // Without this an option typo is taken for a filename, does not exist, and
  // the run reports success having checked nothing.
  if (argv[1][0] == '-' && argv[1][1] == '-') {
    std::cerr << "chip-kernel-verify: unknown option '" << argv[1] << "'\n"
              << "usage: chip-kernel-verify <compiled-output> | --print-devices\n";
    return 2;
  }
  fs::path input = argv[1];
  if (!fs::exists(input)) {
    // Compiler may have produced nothing (e.g. -E); silently skip.
    return 0;
  }

  // Slurp the input. seekToMagic() in spirv-extractor does an unbounded 1MB
  // scan on the raw data path, so pad the buffer to avoid OOB reads on small
  // inputs (raw .spv files, small objects, etc).
  std::ifstream f(input, std::ios::binary);
  if (!f) return 0;
  std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
  if (buf.empty()) return 0;

  // If the input is ELF, only verify when it actually has a .hip_fatbin
  // section. Intermediate object files (e.g. -dc/-c output from hipcc) carry
  // partial / pre-link offload bundles that the SPIR-V extractor isn't built
  // to parse and previously crashed it.
  if (buf.size() >= sizeof(Elf64_Ehdr)) {
    auto *eh = reinterpret_cast<const Elf64_Ehdr *>(buf.data());
    // Magic match implies an ELF host (Linux). macOS uses Mach-O so this
    // never fires there — Mach-O outputs follow the existing scan paths.
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0) {
      bool hasHipFatbin = false;
      if (eh->e_shoff + eh->e_shnum * sizeof(Elf64_Shdr) <= buf.size() &&
          eh->e_shstrndx < eh->e_shnum) {
        auto *sh = reinterpret_cast<const Elf64_Shdr *>(buf.data() + eh->e_shoff);
        const auto &strtab_hdr = sh[eh->e_shstrndx];
        if (strtab_hdr.sh_offset + strtab_hdr.sh_size <= buf.size()) {
          const char *strtab = buf.data() + strtab_hdr.sh_offset;
          for (uint16_t i = 0; i < eh->e_shnum; ++i) {
            if (sh[i].sh_name >= strtab_hdr.sh_size) continue;
            if (std::strcmp(strtab + sh[i].sh_name, ".hip_fatbin") == 0) {
              hasHipFatbin = true;
              break;
            }
          }
        }
      }
      if (!hasHipFatbin) return 0; // not a final HIP-bearing image; skip
    }
  }

  // A *textual* offload bundle (what `hipcc -E` emits) spells the bundler
  // magic out in ASCII inside its delimiter comments:
  //   // __CLANG_OFFLOAD_BUNDLE____START__ hip-spirv64----generic
  // seekToMagic() matches that literal and extractSPIRVModule() then reads the
  // following ASCII as a binary __ClangOffloadBundleHeader, yielding a garbage
  // entry count and walking off the buffer. Detect the textual form by its
  // comment prefix and skip: there is no SPIR-V in preprocessed output anyway.
  {
    static constexpr char kTextBundlePrefix[] = "// __CLANG_OFFLOAD_BUNDLE__";
    constexpr size_t kPrefixLen = sizeof(kTextBundlePrefix) - 1;
    if (buf.size() >= kPrefixLen &&
        std::memcmp(buf.data(), kTextBundlePrefix, kPrefixLen) == 0)
      return 0;
  }

  constexpr size_t kScanPad = 1024 * 1024 + 64;
  if (buf.size() < kScanPad) buf.resize(kScanPad, 0);

  std::string err;
  std::string_view spirv = extractSPIRVModule(buf.data(), err, buf.size());
  if (spirv.empty()) {
    // No HIP fatbin present (e.g. plain C object, or preprocess-only). Not an
    // error — just nothing to check.
    return 0;
  }

  std::string text = disassembleSPIRV(spirv);
  auto spvNames = scanEntryPoints(text);
  if (spvNames.empty()) return 0;

  // Only now is there something this tool was meant to check, so only now does
  // a missing ocloc mean coverage was lost. Asking earlier would reject a
  // host-only link, which carries no kernels and never needed ocloc.
  if (!oclocAvailable())
    return reportNoCoverage(resolveDevices(), mode,
                            "ocloc not found (tried '" + oclocBin() + "')");

  DeviceList resolved = resolveDevices();
  const auto &devices = resolved.Devices;

  if (devices.empty())
    return reportNoCoverage(resolved, mode,
                            resolved.Requested
                                ? "the requested device list names no device"
                                : "no Intel GPU found in /sys/bus/pci/devices"
                                  " and no HIPCC_VERIFY_DEVICES set");

  fs::path workDir = fs::temp_directory_path() /
                     ("chip-kernel-verify-" + std::to_string(getpid()));
  fs::create_directories(workDir);
  fs::path spvFile = workDir / "module.spv";
  {
    std::ofstream out(spvFile, std::ios::binary);
    out.write(spirv.data(), spirv.size());
  }

  int fails = 0;
  int compared = 0;
  for (const auto &d : devices) {
    DeviceResult r = verifyOneDevice(spvFile, d, spvNames, mode, workDir);
    fails += r.Failures;
    compared += r.Compared;
  }

  std::error_code ec;
  fs::remove_all(workDir, ec);

  // Every device skipped means the run produced no coverage at all, which is
  // indistinguishable from a clean pass unless it is said out loud.
  if (compared == 0)
    return reportNoCoverage(resolved, mode,
                            "none of the " + std::to_string(devices.size()) +
                                " device(s) could be checked");

  return fails == 0 ? 0 : 1;
}
