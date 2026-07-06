#include <assert.h>
#include <stdio.h>

#include "../include/cpuinfo.h"

static const char *
type_name(cpuinfo_core_type_t type) {
  switch (type) {
  case cpuinfo_core_type_performance:
    return "P";
  case cpuinfo_core_type_efficiency:
    return "E";
  case cpuinfo_core_type_unknown:
  default:
    return "-";
  }
}

static const char *
arch_name(cpuinfo_arch_t arch) {
  switch (arch) {
  case cpuinfo_arch_x86:
    return "x86";
  case cpuinfo_arch_x86_64:
    return "x86_64";
  case cpuinfo_arch_arm:
    return "arm";
  case cpuinfo_arch_arm64:
    return "arm64";
  case cpuinfo_arch_unknown:
  default:
    return "unknown";
  }
}

int
main() {
  int err;

  cpuinfo_t *info;

  err = cpuinfo_init(&info);
  assert(err == 0);

  cpuinfo_cpu_t cpu;

  err = cpuinfo_query(info, &cpu);
  assert(err == 0);

  printf("cpu: %s (%s), %s\n", cpu.name, cpu.vendor, arch_name(cpu.arch));
  printf("cores: %u physical, %u logical\n", cpu.physical_cores, cpu.logical_cores);

  if (cpu.performance_cores > 0 || cpu.efficiency_cores > 0) {
    printf("hybrid: %u performance, %u efficiency\n", cpu.performance_cores, cpu.efficiency_cores);
  }

  printf("cache: line %u B\n", cpu.cache_line);
  printf("memory: %llu MiB\n", (unsigned long long) (cpu.memory / (1024 * 1024)));

  // A logical core count of zero would indicate detection failed entirely.
  assert(cpu.logical_cores > 0);

  cpuinfo_features_t features;

  err = cpuinfo_features(info, &features);
  assert(err == 0);

  printf("features:");
  // Arm.
  if (features.arm_neon) printf(" neon");
  if (features.arm_aes) printf(" aes");
  if (features.arm_pmull) printf(" pmull");
  if (features.arm_sha1) printf(" sha1");
  if (features.arm_sha2) printf(" sha2");
  if (features.arm_sha512) printf(" sha512");
  if (features.arm_sha3) printf(" sha3");
  if (features.arm_crc32) printf(" crc32");
  if (features.arm_atomics) printf(" atomics");
  if (features.arm_dotprod) printf(" dotprod");
  if (features.arm_fp16) printf(" fp16");
  if (features.arm_sve) printf(" sve");
  if (features.arm_sve2) printf(" sve2");
  // x86.
  if (features.x86_sse2) printf(" sse2");
  if (features.x86_sse4_1) printf(" sse4.1");
  if (features.x86_sse4_2) printf(" sse4.2");
  if (features.x86_avx) printf(" avx");
  if (features.x86_avx2) printf(" avx2");
  if (features.x86_avx512f) printf(" avx512f");
  if (features.x86_bmi) printf(" bmi");
  if (features.x86_aes) printf(" aes");
  if (features.x86_pclmulqdq) printf(" pclmulqdq");
  if (features.x86_sha) printf(" sha");
  if (features.x86_popcnt) printf(" popcnt");
  if (features.x86_rdrand) printf(" rdrand");
  if (features.x86_rdseed) printf(" rdseed");
  if (features.x86_adx) printf(" adx");
  if (features.x86_f16c) printf(" f16c");
  if (features.x86_vaes) printf(" vaes");
  if (features.x86_vpclmulqdq) printf(" vpclmulqdq");
  printf("\n");

  cpuinfo_usage_t usage;

  err = cpuinfo_sample(info, &usage);
  assert(err == 0);

  printf(
    "usage: compute %.1f%%, memory %llu / %llu MiB\n",
    usage.compute < 0 ? 0.0 : usage.compute * 100.0,
    (unsigned long long) (usage.memory_used / (1024 * 1024)),
    (unsigned long long) (usage.memory_total / (1024 * 1024))
  );

  // The context takes a baseline sample at initialization, so the first sample
  // reports a real reading in [0, 1] rather than the "unavailable" sentinel,
  // even when almost no time has elapsed since initialization. A successful
  // sample is never negative; the sentinel is reserved for a platform that
  // cannot measure utilization at all.
  assert(usage.compute >= 0.0 && usage.compute <= 1.0);

  // A second sample taken immediately after has a near-zero interval. It must
  // still read as a valid fraction rather than falling back to the sentinel.
  cpuinfo_usage_t immediate;

  err = cpuinfo_sample(info, &immediate);
  assert(err == 0);
  assert(immediate.compute >= 0.0 && immediate.compute <= 1.0);

  size_t cores = cpuinfo_core_count(info);

  // The individually addressable cores are a subset of the logical cores: the
  // two counts are derived independently and can differ when processors are
  // offline or split across more than one processor group.
  assert(cores <= cpu.logical_cores);

  printf("cores: %zu\n", cores);

  for (size_t i = 0; i < cores; i++) {
    cpuinfo_usage_t core;

    err = cpuinfo_core_usage(info, i, &core);
    assert(err == 0);

    // A present core reports a fraction in [0, 1]; only a core that was offline
    // across the interval reads as the negative sentinel, never any other
    // out-of-range value.
    assert(core.compute <= 1.0);
    assert(core.compute >= 0.0 || core.compute == -1.0);

    printf(
      "  [%zu] %s %llu MHz, l1d %llu KiB l2 %llu KiB l3 %llu KiB, compute %.1f%%\n",
      i,
      type_name(cpuinfo_core_type(info, i)),
      (unsigned long long) (cpuinfo_core_frequency(info, i) / 1000000),
      (unsigned long long) (cpuinfo_core_cache(info, i, cpuinfo_cache_l1d) / 1024),
      (unsigned long long) (cpuinfo_core_cache(info, i, cpuinfo_cache_l2) / 1024),
      (unsigned long long) (cpuinfo_core_cache(info, i, cpuinfo_cache_l3) / 1024),
      core.compute < 0 ? 0.0 : core.compute * 100.0
    );
  }

  // Out-of-range access must fail rather than crash.
  cpuinfo_usage_t core;
  assert(cpuinfo_core_usage(info, cores, &core) < 0);

  cpuinfo_destroy(info);

  return 0;
}
