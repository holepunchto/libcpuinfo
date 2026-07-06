#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/cpuinfo.h"
#include "x86.h"

// The static per-core detail captured once at initialization.
typedef struct {
  cpuinfo_core_type_t type;
  uint64_t frequency;
  uint64_t cache[CPUINFO_CACHE_LEVELS];
} cpuinfo_core_t;

struct cpuinfo_s {
  cpuinfo_cpu_t info;

  // The number of logical processors reported by the most recent sample, which
  // is the addressable core count. It never exceeds `capacity`.
  natural_t cores;

  // The capacity of the `core_compute` and `core` arrays, fixed at the count
  // observed when the context was initialized. A later sample that reports more
  // processors is clamped to this so the fixed-size arrays are never overrun.
  natural_t capacity;

  // The cumulative busy and total CPU ticks per core at the previous sample,
  // used to derive utilization as a delta.
  uint64_t *prev_busy;
  uint64_t *prev_total;

  // The per-core compute utilization captured by the most recent
  // `cpuinfo_sample()` call, negative until the first such call.
  double *core_compute;

  // The static per-core detail, indexed by logical processor.
  cpuinfo_core_t *core;

  // The system-wide memory usage captured by the most recent
  // `cpuinfo_sample()` call.
  uint64_t memory_used;
};

static bool
cpuinfo__sysctl_string(const char *name, char *dst, size_t cap) {
  size_t len = cap;

  if (sysctlbyname(name, dst, &len, NULL, 0) != 0) {
    dst[0] = '\0';

    return false;
  }

  dst[cap - 1] = '\0';

  return true;
}

static bool
cpuinfo__sysctl_uint(const char *name, uint64_t *result) {
  // Zero-initialize so that a 32-bit result leaves the upper bytes clear on the
  // little-endian architectures macOS runs on.
  uint64_t value = 0;

  size_t len = sizeof(value);

  if (sysctlbyname(name, &value, &len, NULL, 0) != 0) return false;

  *result = value;

  return true;
}

static cpuinfo_arch_t
cpuinfo__arch(void) {
  uint64_t type = 0;

  if (!cpuinfo__sysctl_uint("hw.cputype", &type)) return cpuinfo_arch_unknown;

  // The 64-bit ABI flag from <mach/machine.h>, masked in for 64-bit variants.
  bool is_64 = (type & 0x01000000) != 0;

  switch (type & ~((uint64_t) 0x01000000)) {
  case 7: // CPU_TYPE_X86
    return is_64 ? cpuinfo_arch_x86_64 : cpuinfo_arch_x86;
  case 12: // CPU_TYPE_ARM
    return is_64 ? cpuinfo_arch_arm64 : cpuinfo_arch_arm;
  }

  return cpuinfo_arch_unknown;
}

// Report whether an optional-feature sysctl exists and is set. Absent keys
// return `ENOENT`, which `cpuinfo__sysctl_uint()` reports as failure.
static bool
cpuinfo__sysctl_present(const char *name) {
  uint64_t value = 0;

  return cpuinfo__sysctl_uint(name, &value) && value != 0;
}

static cpuinfo_features_t
cpuinfo__features(void) {
#if defined(CPUINFO_X86)
  return cpuinfo__cpuid_features();
#else
  cpuinfo_features_t features = {0};

  // Advanced SIMD is mandatory on AArch64 and present on the ARMv7 cores that
  // macOS has historically supported.
  features.arm_neon = true;

  // The optional Arm extensions are reported one sysctl per feature. Recent
  // systems use the architectural `FEAT_` names; older ones use ad-hoc names,
  // tried as a fallback. SVE is not implemented on Apple silicon.
#define CPUINFO__SYSCTL_PRESENT(modern, legacy) \
  (cpuinfo__sysctl_present(modern) || ((legacy) != NULL && cpuinfo__sysctl_present(legacy)))

  features.arm_aes = CPUINFO__SYSCTL_PRESENT("hw.optional.arm.FEAT_AES", NULL);
  features.arm_pmull = CPUINFO__SYSCTL_PRESENT("hw.optional.arm.FEAT_PMULL", NULL);
  features.arm_sha1 = CPUINFO__SYSCTL_PRESENT("hw.optional.arm.FEAT_SHA1", NULL);
  features.arm_sha2 = CPUINFO__SYSCTL_PRESENT("hw.optional.arm.FEAT_SHA256", NULL);
  features.arm_sha512 = CPUINFO__SYSCTL_PRESENT("hw.optional.arm.FEAT_SHA512", "hw.optional.armv8_2_sha512");
  features.arm_sha3 = CPUINFO__SYSCTL_PRESENT("hw.optional.arm.FEAT_SHA3", "hw.optional.armv8_2_sha3");
  features.arm_crc32 = CPUINFO__SYSCTL_PRESENT("hw.optional.arm.FEAT_CRC32", "hw.optional.armv8_crc32");
  features.arm_atomics = CPUINFO__SYSCTL_PRESENT("hw.optional.arm.FEAT_LSE", "hw.optional.armv8_1_atomics");
  features.arm_dotprod = CPUINFO__SYSCTL_PRESENT("hw.optional.arm.FEAT_DotProd", NULL);
  features.arm_fp16 = CPUINFO__SYSCTL_PRESENT("hw.optional.arm.FEAT_FP16", "hw.optional.neon_fp16");

#undef CPUINFO__SYSCTL_PRESENT

  return features;
#endif
}

static void
cpuinfo__fill_vendor(cpuinfo_cpu_t *cpu) {
  char vendor[CPUINFO_NAME_MAX];

  if (cpuinfo__sysctl_string("machdep.cpu.vendor", vendor, sizeof(vendor)) && vendor[0] != '\0') {
    const char *name = vendor;

    if (strcmp(vendor, "GenuineIntel") == 0) name = "Intel";
    else if (strcmp(vendor, "AuthenticAMD") == 0) name = "AMD";

    strncpy(cpu->vendor, name, sizeof(cpu->vendor) - 1);

    cpu->vendor[sizeof(cpu->vendor) - 1] = '\0';

    return;
  }

  // The vendor string is not exposed on Apple silicon.
  if (cpu->arch == cpuinfo_arch_arm64 || cpu->arch == cpuinfo_arch_arm) {
    strcpy(cpu->vendor, "Apple");
  } else {
    cpu->vendor[0] = '\0';
  }
}

// Sample the cumulative busy and total CPU ticks per core. On success the
// caller owns `*busy` and `*total`, which must be released with `free()`, and
// `*cores` holds their length.
static int
cpuinfo__sample(uint64_t **busy, uint64_t **total, natural_t *cores) {
  natural_t count;
  processor_cpu_load_info_t load;
  mach_msg_type_number_t info_count;

  kern_return_t status = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, (processor_info_array_t *) &load, &info_count);

  if (status != KERN_SUCCESS) return -1;

  uint64_t *b = malloc(count * sizeof(uint64_t));
  uint64_t *t = malloc(count * sizeof(uint64_t));

  if (b == NULL || t == NULL) {
    free(b);
    free(t);

    vm_deallocate(mach_task_self(), (vm_address_t) load, info_count * sizeof(integer_t));

    return -1;
  }

  for (natural_t i = 0; i < count; i++) {
    uint64_t user = load[i].cpu_ticks[CPU_STATE_USER];
    uint64_t system = load[i].cpu_ticks[CPU_STATE_SYSTEM];
    uint64_t idle = load[i].cpu_ticks[CPU_STATE_IDLE];
    uint64_t nice = load[i].cpu_ticks[CPU_STATE_NICE];

    b[i] = user + system + nice;
    t[i] = user + system + nice + idle;
  }

  vm_deallocate(mach_task_self(), (vm_address_t) load, info_count * sizeof(integer_t));

  *busy = b;
  *total = t;
  *cores = count;

  return 0;
}

static void
cpuinfo__memory(uint64_t total, uint64_t *used) {
  vm_size_t page_size = 0;

  if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS || page_size == 0) {
    *used = 0;

    return;
  }

  vm_statistics64_data_t vm;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t) &vm, &count) != KERN_SUCCESS) {
    *used = 0;

    return;
  }

  // Mirror the "total minus available" definition the Linux and Windows backends
  // use, so the figure is comparable across platforms. The memory available for
  // a new allocation without paging is the free pages plus the reclaimable
  // pools: the file-backed cache the kernel can evict (which already accounts
  // for speculative read-ahead) and the purgeable pages it can discard. What
  // remains - anonymous, wired, and compressed memory - is counted as in use.
  uint64_t available = (uint64_t) vm.free_count + vm.external_page_count + vm.purgeable_count;

  uint64_t available_bytes = available * (uint64_t) page_size;

  *used = available_bytes < total ? total - available_bytes : 0;
}

// Read a per-perflevel cache size sysctl, such as "hw.perflevel0.l2cachesize".
static uint64_t
cpuinfo__perflevel_cache(unsigned level, const char *leaf) {
  char name[64];

  snprintf(name, sizeof(name), "hw.perflevel%u.%s", level, leaf);

  uint64_t value = 0;

  return cpuinfo__sysctl_uint(name, &value) ? value : 0;
}

// Capture the static per-core detail. On a hybrid Apple silicon CPU the kernel
// numbers logical processors with the efficiency cores first, which is the
// reverse of the performance-level numbering, where level 0 is the performance
// cluster. Each contiguous index range is mapped to its cluster accordingly.
static void
cpuinfo__detail(cpuinfo_t *info) {
  natural_t cores = info->cores;

  // A per-core maximum frequency is only exposed on Intel Macs; Apple silicon
  // has no supported frequency sysctl, so it is left at zero there.
  uint64_t frequency = 0;

  if (!cpuinfo__sysctl_uint("hw.cpufrequency_max", &frequency)) {
    cpuinfo__sysctl_uint("hw.cpufrequency", &frequency);
  }

  for (natural_t i = 0; i < cores; i++) {
    info->core[i].frequency = frequency;
  }

  uint64_t levels = 0;

  if (cpuinfo__sysctl_uint("hw.nperflevels", &levels) && levels >= 2) {
    uint64_t efficiency = 0;

    cpuinfo__sysctl_uint("hw.perflevel1.logicalcpu", &efficiency);

    // Read each cluster's caches once, indexed by performance level.
    uint64_t cache[2][CPUINFO_CACHE_LEVELS];

    for (unsigned level = 0; level < 2; level++) {
      cache[level][cpuinfo_cache_l1d] = cpuinfo__perflevel_cache(level, "l1dcachesize");
      cache[level][cpuinfo_cache_l1i] = cpuinfo__perflevel_cache(level, "l1icachesize");
      cache[level][cpuinfo_cache_l2] = cpuinfo__perflevel_cache(level, "l2cachesize");
      cache[level][cpuinfo_cache_l3] = cpuinfo__perflevel_cache(level, "l3cachesize");
    }

    for (natural_t i = 0; i < cores; i++) {
      // The efficiency cores occupy the low indices; the rest are performance.
      unsigned level = i < efficiency ? 1 : 0;

      info->core[i].type = level == 1 ? cpuinfo_core_type_efficiency : cpuinfo_core_type_performance;

      for (unsigned l = 0; l < CPUINFO_CACHE_LEVELS; l++) {
        info->core[i].cache[l] = cache[level][l];
      }
    }
  } else {
    // A homogeneous CPU has no core-type distinction; the caches are the same
    // for every core and read from the top-level sysctls.
    uint64_t cache[CPUINFO_CACHE_LEVELS];

    uint64_t value = 0;
    cache[cpuinfo_cache_l1d] = cpuinfo__sysctl_uint("hw.l1dcachesize", &value) ? value : 0;
    cache[cpuinfo_cache_l1i] = cpuinfo__sysctl_uint("hw.l1icachesize", &value) ? value : 0;
    cache[cpuinfo_cache_l2] = cpuinfo__sysctl_uint("hw.l2cachesize", &value) ? value : 0;
    cache[cpuinfo_cache_l3] = cpuinfo__sysctl_uint("hw.l3cachesize", &value) ? value : 0;

    for (natural_t i = 0; i < cores; i++) {
      for (unsigned l = 0; l < CPUINFO_CACHE_LEVELS; l++) {
        info->core[i].cache[l] = cache[l];
      }
    }
  }
}

int
cpuinfo_init(cpuinfo_t **result) {
  cpuinfo_t *info = calloc(1, sizeof(cpuinfo_t));

  if (info == NULL) return -1;

  cpuinfo_cpu_t *cpu = &info->info;

  cpuinfo__sysctl_string("machdep.cpu.brand_string", cpu->name, sizeof(cpu->name));

  cpu->arch = cpuinfo__arch();

  cpuinfo__fill_vendor(cpu);

  cpu->features = cpuinfo__features();

  uint64_t value;

  cpu->physical_cores = cpuinfo__sysctl_uint("hw.physicalcpu", &value) ? (uint32_t) value : 0;
  cpu->logical_cores = cpuinfo__sysctl_uint("hw.logicalcpu", &value) ? (uint32_t) value : 0;
  cpu->frequency = cpuinfo__sysctl_uint("hw.cpufrequency", &value) ? value : 0;
  cpu->memory = cpuinfo__sysctl_uint("hw.memsize", &value) ? value : 0;

  // On a hybrid CPU the kernel exposes one performance level per core type,
  // ordered fastest first. Level 0 is the performance cluster and level 1 the
  // efficiency cluster; a homogeneous CPU reports a single level.
  if (cpuinfo__sysctl_uint("hw.nperflevels", &value) && value >= 2) {
    uint64_t performance = 0;
    uint64_t efficiency = 0;

    cpuinfo__sysctl_uint("hw.perflevel0.physicalcpu", &performance);
    cpuinfo__sysctl_uint("hw.perflevel1.physicalcpu", &efficiency);

    cpu->performance_cores = (uint32_t) performance;
    cpu->efficiency_cores = (uint32_t) efficiency;
  }

  cpu->cache_line = cpuinfo__sysctl_uint("hw.cachelinesize", &value) ? (uint32_t) value : 0;

  // Take a baseline sample so that the first utilization query measures the
  // interval since initialization.
  if (cpuinfo__sample(&info->prev_busy, &info->prev_total, &info->cores) != 0) {
    free(info);

    return -1;
  }

  // Size the fixed per-core arrays to the processor count observed now; a later
  // sample is clamped to this capacity rather than growing them.
  info->capacity = info->cores;

  info->core_compute = malloc(info->capacity * sizeof(double));
  info->core = calloc(info->capacity, sizeof(cpuinfo_core_t));

  if (info->core_compute == NULL || info->core == NULL) {
    free(info->prev_busy);
    free(info->prev_total);
    free(info->core_compute);
    free(info->core);
    free(info);

    return -1;
  }

  // No interval has elapsed yet, so per-core utilization is not yet available.
  for (natural_t i = 0; i < info->capacity; i++) {
    info->core_compute[i] = -1.0;
  }

  // Capture the static per-core detail now that the array is sized.
  cpuinfo__detail(info);

  cpuinfo__memory(cpu->memory, &info->memory_used);

  *result = info;

  return 0;
}

void
cpuinfo_destroy(cpuinfo_t *info) {
  if (info == NULL) return;

  free(info->prev_busy);
  free(info->prev_total);
  free(info->core_compute);
  free(info->core);
  free(info);
}

int
cpuinfo_query(const cpuinfo_t *info, cpuinfo_cpu_t *result) {
  *result = info->info;

  return 0;
}

int
cpuinfo_features(const cpuinfo_t *info, cpuinfo_features_t *result) {
  *result = info->info.features;

  return 0;
}

int
cpuinfo_sample(cpuinfo_t *info, cpuinfo_usage_t *result) {
  result->compute = -1.0;
  result->memory_total = info->info.memory;

  // `cpuinfo__memory()` writes the field on every path, including failure.
  cpuinfo__memory(info->info.memory, &result->memory_used);

  info->memory_used = result->memory_used;

  uint64_t *busy;
  uint64_t *total;
  natural_t cores;

  if (cpuinfo__sample(&busy, &total, &cores) != 0) return -1;

  // The processor count should be stable, but guard against a mismatch rather
  // than read past either array. `prev_busy`/`prev_total` are sized to the
  // previous sample, so the delta is bounded by it and the new count.
  natural_t n = cores < info->cores ? cores : info->cores;

  uint64_t busy_delta = 0;
  uint64_t total_delta = 0;

  for (natural_t i = 0; i < n; i++) {
    uint64_t core_busy = busy[i] - info->prev_busy[i];
    uint64_t core_total = total[i] - info->prev_total[i];

    // `core_compute` is only `capacity` entries wide, so any processors beyond
    // it are still counted in the aggregate but not addressable per core. A core
    // with a zero-length interval observed no activity and reads as `0`.
    if (i < info->capacity) {
      info->core_compute[i] = core_total > 0 ? (double) core_busy / (double) core_total : 0.0;
    }

    busy_delta += core_busy;
    total_delta += core_total;
  }

  // Sampling succeeded, so utilization is measurable on this platform; an
  // interval too short to observe any activity reads as `0` rather than the
  // "unavailable" sentinel.
  result->compute = total_delta > 0 ? (double) busy_delta / (double) total_delta : 0.0;

  free(info->prev_busy);
  free(info->prev_total);

  info->prev_busy = busy;
  info->prev_total = total;

  // Clamp the addressable count to the fixed array capacity so the per-core
  // getters, which bound-check against it, never index past `core`.
  info->cores = cores < info->capacity ? cores : info->capacity;

  return 0;
}

size_t
cpuinfo_core_count(const cpuinfo_t *info) {
  return info->cores;
}

int
cpuinfo_core_usage(const cpuinfo_t *info, size_t index, cpuinfo_usage_t *result) {
  if (index >= info->cores) return -1;

  result->compute = info->core_compute[index];
  result->memory_used = info->memory_used;
  result->memory_total = info->info.memory;

  return 0;
}

cpuinfo_core_type_t
cpuinfo_core_type(const cpuinfo_t *info, size_t index) {
  if (index >= info->cores) return cpuinfo_core_type_unknown;

  return info->core[index].type;
}

uint64_t
cpuinfo_core_frequency(const cpuinfo_t *info, size_t index) {
  if (index >= info->cores) return 0;

  return info->core[index].frequency;
}

uint64_t
cpuinfo_core_cache(const cpuinfo_t *info, size_t index, cpuinfo_cache_level_t level) {
  if (index >= info->cores || (unsigned) level >= CPUINFO_CACHE_LEVELS) return 0;

  return info->core[index].cache[level];
}
