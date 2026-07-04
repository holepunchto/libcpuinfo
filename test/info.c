#include <assert.h>
#include <stdio.h>

#include "../include/cpuinfo.h"

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

  err = cpuinfo_cpu_info(info, &cpu);
  assert(err == 0);

  printf("cpu: %s (%s), %s\n", cpu.name, cpu.vendor, arch_name(cpu.arch));
  printf("cores: %u physical, %u logical\n", cpu.physical_cores, cpu.logical_cores);
  printf("memory: %llu MiB\n", (unsigned long long) (cpu.memory / (1024 * 1024)));

  // A logical core count of zero would indicate detection failed entirely.
  assert(cpu.logical_cores > 0);

  printf("features:");
  if (cpuinfo_has_feature(info, cpuinfo_feature_neon)) printf(" neon");
  if (cpuinfo_has_feature(info, cpuinfo_feature_sse2)) printf(" sse2");
  if (cpuinfo_has_feature(info, cpuinfo_feature_sse4_1)) printf(" sse4.1");
  if (cpuinfo_has_feature(info, cpuinfo_feature_sse4_2)) printf(" sse4.2");
  if (cpuinfo_has_feature(info, cpuinfo_feature_avx)) printf(" avx");
  if (cpuinfo_has_feature(info, cpuinfo_feature_avx2)) printf(" avx2");
  if (cpuinfo_has_feature(info, cpuinfo_feature_avx512f)) printf(" avx512f");
  if (cpuinfo_has_feature(info, cpuinfo_feature_bmi)) printf(" bmi");
  printf("\n");

  cpuinfo_usage_t usage;

  err = cpuinfo_cpu_usage(info, &usage);
  assert(err == 0);

  printf(
    "usage: compute %.1f%%, memory %llu / %llu MiB\n",
    usage.compute < 0 ? 0.0 : usage.compute * 100.0,
    (unsigned long long) (usage.memory_used / (1024 * 1024)),
    (unsigned long long) (usage.memory_total / (1024 * 1024))
  );

  size_t cores = cpuinfo_core_count(info);

  // Every logical core should be individually addressable.
  assert(cores == cpu.logical_cores);

  printf("cores: %zu\n", cores);

  for (size_t i = 0; i < cores; i++) {
    cpuinfo_usage_t core;

    err = cpuinfo_core_usage(info, i, &core);
    assert(err == 0);

    cpuinfo_core_times_t times;

    err = cpuinfo_core_times(info, i, &times);
    assert(err == 0);

    // The cumulative counters must have advanced since boot.
    assert(times.user + times.nice + times.system + times.idle > 0);

    printf(
      "  [%zu] compute %.1f%%, times user=%llu nice=%llu sys=%llu idle=%llu irq=%llu\n",
      i,
      core.compute < 0 ? 0.0 : core.compute * 100.0,
      (unsigned long long) times.user,
      (unsigned long long) times.nice,
      (unsigned long long) times.system,
      (unsigned long long) times.idle,
      (unsigned long long) times.irq
    );
  }

  // Out-of-range access must fail rather than crash.
  cpuinfo_usage_t core;
  assert(cpuinfo_core_usage(info, cores, &core) < 0);

  cpuinfo_core_times_t times;
  assert(cpuinfo_core_times(info, cores, &times) < 0);

  cpuinfo_destroy(info);

  return 0;
}
