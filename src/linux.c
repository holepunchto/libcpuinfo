// Required for `sched_setaffinity()` and the `cpu_set_t` macros, used to pin the
// per-core type probe to each logical processor in turn.
#define _GNU_SOURCE

#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__arm__) || defined(__aarch64__)
#include <sys/auxv.h>
#endif

#include "../include/cpuinfo.h"
#include "x86.h"

#if defined(__aarch64__)
// The AArch64 hardware capability bits reported through the auxiliary vector.
// The values are a stable part of the kernel ABI; they are defined here rather
// than relying on <asm/hwcap.h> so that the newer bits are available even when
// building against older toolchain headers.
#define CPUINFO_HWCAP_AES     (1ul << 3)
#define CPUINFO_HWCAP_PMULL   (1ul << 4)
#define CPUINFO_HWCAP_SHA1    (1ul << 5)
#define CPUINFO_HWCAP_SHA2    (1ul << 6)
#define CPUINFO_HWCAP_CRC32   (1ul << 7)
#define CPUINFO_HWCAP_ATOMICS (1ul << 8)
#define CPUINFO_HWCAP_ASIMDHP (1ul << 10)
#define CPUINFO_HWCAP_SHA3    (1ul << 17)
#define CPUINFO_HWCAP_ASIMDDP (1ul << 20)
#define CPUINFO_HWCAP_SHA512  (1ul << 21)
#define CPUINFO_HWCAP_SVE     (1ul << 22)

#define CPUINFO_HWCAP2_SVE2 (1ul << 1)
#endif

// The static per-core detail captured once at initialization.
typedef struct {
  cpuinfo_core_type_t type;
  uint64_t frequency;
  uint64_t cache[CPUINFO_CACHE_LEVELS];
} cpuinfo_core_t;

struct cpuinfo_s {
  cpuinfo_cpu_t info;

  // The number of logical processors observed in `/proc/stat`, i.e. the number
  // of populated entries in the per-core arrays below.
  unsigned cores;

  // The capacity of the per-core arrays, sized to the maximum number of
  // configured processors so that they never need to grow.
  unsigned capacity;

  // The cumulative busy and total CPU ticks per core at the previous sample,
  // used to derive utilization as a delta.
  uint64_t *prev_busy;
  uint64_t *prev_total;

  // The per-core compute utilization captured by the most recent
  // `cpuinfo_cpu_usage()` call, negative until the first such call.
  double *core_compute;

  // The static per-core detail, indexed by logical processor.
  cpuinfo_core_t *core;

  // The system-wide memory usage captured by the most recent
  // `cpuinfo_cpu_usage()` call.
  uint64_t memory_used;
};

static bool
cpuinfo__read_file(const char *path, char *buf, size_t cap) {
  FILE *file = fopen(path, "r");

  if (file == NULL) return false;

  size_t len = fread(buf, 1, cap - 1, file);

  fclose(file);

  buf[len] = '\0';

  return true;
}

// Extract the value of a `key: value` field from the given `/proc/cpuinfo`
// style content, copying the trimmed value into `dst`.
static bool
cpuinfo__proc_field(const char *content, const char *key, char *dst, size_t cap) {
  size_t key_len = strlen(key);

  const char *line = content;

  while (line != NULL && *line != '\0') {
    if (strncmp(line, key, key_len) == 0) {
      const char *value = strchr(line, ':');

      if (value != NULL) {
        value++;

        while (*value == ' ' || *value == '\t')
          value++;

        const char *end = strchr(value, '\n');

        size_t len = end != NULL ? (size_t) (end - value) : strlen(value);

        if (len >= cap) len = cap - 1;

        memcpy(dst, value, len);

        dst[len] = '\0';

        return true;
      }
    }

    line = strchr(line, '\n');

    if (line != NULL) line++;
  }

  return false;
}

static uint64_t
cpuinfo__meminfo_bytes(const char *content, const char *key) {
  char value[64];

  if (!cpuinfo__proc_field(content, key, value, sizeof(value))) return 0;

  // The values in `/proc/meminfo` are reported in kibibytes.
  return strtoull(value, NULL, 10) * 1024;
}

static cpuinfo_arch_t
cpuinfo__arch(void) {
#if defined(__x86_64__)
  return cpuinfo_arch_x86_64;
#elif defined(__i386__)
  return cpuinfo_arch_x86;
#elif defined(__aarch64__)
  return cpuinfo_arch_arm64;
#elif defined(__arm__)
  return cpuinfo_arch_arm;
#else
  return cpuinfo_arch_unknown;
#endif
}

static uint64_t
cpuinfo__features(void) {
#if defined(CPUINFO_X86)
  return cpuinfo__cpuid_features();
#elif defined(__aarch64__)
  // Advanced SIMD is mandatory on AArch64.
  uint64_t features = cpuinfo_feature_arm_neon;

  unsigned long hwcap = getauxval(AT_HWCAP);
  unsigned long hwcap2 = getauxval(AT_HWCAP2);

  if (hwcap & CPUINFO_HWCAP_AES) features |= cpuinfo_feature_arm_aes;
  if (hwcap & CPUINFO_HWCAP_PMULL) features |= cpuinfo_feature_arm_pmull;
  if (hwcap & CPUINFO_HWCAP_SHA1) features |= cpuinfo_feature_arm_sha1;
  if (hwcap & CPUINFO_HWCAP_SHA2) features |= cpuinfo_feature_arm_sha2;
  if (hwcap & CPUINFO_HWCAP_SHA512) features |= cpuinfo_feature_arm_sha512;
  if (hwcap & CPUINFO_HWCAP_SHA3) features |= cpuinfo_feature_arm_sha3;
  if (hwcap & CPUINFO_HWCAP_CRC32) features |= cpuinfo_feature_arm_crc32;
  if (hwcap & CPUINFO_HWCAP_ATOMICS) features |= cpuinfo_feature_arm_atomics;
  if (hwcap & CPUINFO_HWCAP_ASIMDDP) features |= cpuinfo_feature_arm_dotprod;
  if (hwcap & CPUINFO_HWCAP_ASIMDHP) features |= cpuinfo_feature_arm_fp16;
  if (hwcap & CPUINFO_HWCAP_SVE) features |= cpuinfo_feature_arm_sve;
  if (hwcap2 & CPUINFO_HWCAP2_SVE2) features |= cpuinfo_feature_arm_sve2;

  return features;
#elif defined(__arm__)
  return (getauxval(AT_HWCAP) & (1 << 12)) ? cpuinfo_feature_arm_neon : 0; // HWCAP_NEON
#else
  return 0;
#endif
}

// Read a single unsigned integer from a sysfs file, returning `0` if the file
// is absent or unreadable.
static uint64_t
cpuinfo__sysfs_uint(const char *path) {
  char buf[64];

  if (!cpuinfo__read_file(path, buf, sizeof(buf))) return 0;

  return strtoull(buf, NULL, 10);
}

// Read a sysfs size value such as "32K" or "8M" and return it in bytes.
static uint64_t
cpuinfo__sysfs_size(const char *path) {
  char buf[64];

  if (!cpuinfo__read_file(path, buf, sizeof(buf))) return 0;

  char *end;
  uint64_t value = strtoull(buf, &end, 10);

  switch (*end) {
  case 'K':
  case 'k':
    return value * 1024;
  case 'M':
  case 'm':
    return value * 1024 * 1024;
  case 'G':
  case 'g':
    return value * 1024 * 1024 * 1024;
  }

  return value;
}

// Read the cache sizes for a single logical processor into `cache`, indexed by
// `cpuinfo_cache_level_t`, and return its coherency line size, or `0` if none
// could be determined.
static uint32_t
cpuinfo__core_cache(unsigned cpu, uint64_t cache[CPUINFO_CACHE_LEVELS]) {
  char path[128];

  uint32_t line = 0;

  for (unsigned i = 0;; i++) {
    char level_buf[16];

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/level", cpu, i);

    // The cache index directories are contiguous, so a missing level marks the
    // end of the enumeration.
    if (!cpuinfo__read_file(path, level_buf, sizeof(level_buf))) break;

    unsigned level = (unsigned) strtoul(level_buf, NULL, 10);

    char type[16] = {0};

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/type", cpu, i);
    cpuinfo__read_file(path, type, sizeof(type));

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/size", cpu, i);
    uint64_t size = cpuinfo__sysfs_size(path);

    if (line == 0) {
      snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/coherency_line_size", cpu, i);
      line = (uint32_t) cpuinfo__sysfs_uint(path);
    }

    switch (level) {
    case 1:
      // A unified level 1 cache, if present, backs both roles.
      if (type[0] != 'I') cache[cpuinfo_cache_l1d] = size;
      if (type[0] != 'D') cache[cpuinfo_cache_l1i] = size;
      break;
    case 2:
      cache[cpuinfo_cache_l2] = size;
      break;
    case 3:
      cache[cpuinfo_cache_l3] = size;
      break;
    }
  }

  return line;
}

// Classify each logical processor as a performance or efficiency core. On x86
// this reads the core type from `CPUID.1A` while pinned to each processor in
// turn; on Arm it compares the per-CPU capacity the kernel derives for
// heterogeneous systems. The types are left `unknown` on a homogeneous CPU.
static void
cpuinfo__core_types(cpuinfo_t *info) {
  unsigned capacity = info->capacity;

#if defined(CPUINFO_X86)
  if (!cpuinfo__cpuid_hybrid()) return;

  cpu_set_t original;

  if (sched_getaffinity(0, sizeof(original), &original) != 0) return;

  for (unsigned i = 0; i < capacity; i++) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(i, &set);

    // Pin to the target processor so that `CPUID.1A` reports its core type. A
    // processor that is offline or outside the affinity mask is skipped.
    if (sched_setaffinity(0, sizeof(set), &set) != 0) continue;

    uint32_t registers[4];
    cpuinfo__cpuid(0x1a, 0, registers);

    switch (registers[0] >> 24) { // Core type in EAX[31:24].
    case 0x20:                    // Intel Atom.
      info->core[i].type = cpuinfo_core_type_efficiency;
      break;
    case 0x40: // Intel Core.
      info->core[i].type = cpuinfo_core_type_performance;
      break;
    }
  }

  sched_setaffinity(0, sizeof(original), &original);
#else
  char path[128];

  uint64_t max_capacity = 0;
  bool have_capacity = false;

  for (unsigned i = 0; i < capacity; i++) {
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpu_capacity", i);

    uint64_t value = cpuinfo__sysfs_uint(path);

    if (value == 0) continue;

    have_capacity = true;

    if (value > max_capacity) max_capacity = value;
  }

  if (!have_capacity) return;

  bool heterogeneous = false;

  for (unsigned i = 0; i < capacity; i++) {
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpu_capacity", i);

    uint64_t value = cpuinfo__sysfs_uint(path);

    if (value == 0) continue;

    if (value < max_capacity) {
      info->core[i].type = cpuinfo_core_type_efficiency;

      heterogeneous = true;
    } else {
      info->core[i].type = cpuinfo_core_type_performance;
    }
  }

  // A single capacity across all cores is a homogeneous CPU, not a tier split.
  if (!heterogeneous) {
    for (unsigned i = 0; i < capacity; i++) {
      info->core[i].type = cpuinfo_core_type_unknown;
    }
  }
#endif
}

// Capture the static per-core detail and derive the aggregate figures on the
// CPU snapshot from it.
static void
cpuinfo__detail(cpuinfo_t *info) {
  cpuinfo_cpu_t *cpu = &info->info;

  unsigned capacity = info->capacity;

  uint32_t line = 0;

  for (unsigned i = 0; i < capacity; i++) {
    char path[128];

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", i);

    // The maximum frequency is reported in kilohertz.
    info->core[i].frequency = cpuinfo__sysfs_uint(path) * 1000;

    uint32_t core_line = cpuinfo__core_cache(i, info->core[i].cache);

    if (i == 0) line = core_line;
  }

  cpuinfo__core_types(info);

  // The first processor is representative of at least one core type; report its
  // caches as the aggregate figures.
  cpu->cache_line = line;
  cpu->l1d_cache = info->core[0].cache[cpuinfo_cache_l1d];
  cpu->l1i_cache = info->core[0].cache[cpuinfo_cache_l1i];
  cpu->l2_cache = info->core[0].cache[cpuinfo_cache_l2];
  cpu->l3_cache = info->core[0].cache[cpuinfo_cache_l3];

  // Count physical cores per type, tallying each core once at its
  // lowest-numbered hardware thread so that multithreading does not inflate it.
  uint32_t performance = 0;
  uint32_t efficiency = 0;

  for (unsigned i = 0; i < capacity; i++) {
    if (info->core[i].type == cpuinfo_core_type_unknown) continue;

    char siblings[128];
    char path[128];

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", i);

    if (cpuinfo__read_file(path, siblings, sizeof(siblings)) && (unsigned) strtoul(siblings, NULL, 10) != i) continue;

    if (info->core[i].type == cpuinfo_core_type_performance) performance++;
    else efficiency++;
  }

  cpu->performance_cores = performance;
  cpu->efficiency_cores = efficiency;
}

static void
cpuinfo__fill_static(cpuinfo_cpu_t *cpu) {
  char content[8192];

  bool have_cpuinfo = cpuinfo__read_file("/proc/cpuinfo", content, sizeof(content));

  cpu->arch = cpuinfo__arch();
  cpu->features = cpuinfo__features();

#if defined(CPUINFO_X86)
  cpuinfo__cpuid_brand(cpu->name);

  char vendor[13];

  cpuinfo__cpuid_vendor(vendor);

  if (strcmp(vendor, "GenuineIntel") == 0) strcpy(cpu->vendor, "Intel");
  else if (strcmp(vendor, "AuthenticAMD") == 0) strcpy(cpu->vendor, "AMD");
  else {
    strncpy(cpu->vendor, vendor, sizeof(cpu->vendor) - 1);

    cpu->vendor[sizeof(cpu->vendor) - 1] = '\0';
  }
#else
  cpu->name[0] = '\0';
  cpu->vendor[0] = '\0';

  if (have_cpuinfo) {
    if (!cpuinfo__proc_field(content, "model name", cpu->name, sizeof(cpu->name))) {
      cpuinfo__proc_field(content, "Model", cpu->name, sizeof(cpu->name));
    }
  }
#endif

  cpu->logical_cores = (uint32_t) sysconf(_SC_NPROCESSORS_ONLN);

  // Derive the physical core count from the hyperthreading topology reported in
  // `/proc/cpuinfo`, falling back to the logical count when it is absent.
  cpu->physical_cores = cpu->logical_cores;

  if (have_cpuinfo) {
    char siblings[16], cores[16];

    if (cpuinfo__proc_field(content, "siblings", siblings, sizeof(siblings)) && cpuinfo__proc_field(content, "cpu cores", cores, sizeof(cores))) {
      unsigned long threads_per_package = strtoul(siblings, NULL, 10);
      unsigned long cores_per_package = strtoul(cores, NULL, 10);

      if (threads_per_package > 0 && cores_per_package > 0) {
        unsigned long packages = cpu->logical_cores / threads_per_package;

        if (packages == 0) packages = 1;

        cpu->physical_cores = (uint32_t) (packages * cores_per_package);
      }
    }
  }

  // A CPU-limited container can report fewer online logical processors than the
  // package advertises threads, which would otherwise over-count physical cores.
  if (cpu->physical_cores > cpu->logical_cores) cpu->physical_cores = cpu->logical_cores;

  // The maximum frequency is reported in kilohertz.
  char frequency[32];

  if (cpuinfo__read_file("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", frequency, sizeof(frequency))) {
    cpu->frequency = strtoull(frequency, NULL, 10) * 1000;
  } else {
    cpu->frequency = 0;
  }

  char meminfo[8192];

  if (cpuinfo__read_file("/proc/meminfo", meminfo, sizeof(meminfo))) {
    cpu->memory = cpuinfo__meminfo_bytes(meminfo, "MemTotal");
  } else {
    cpu->memory = 0;
  }
}

// Sample the cumulative busy and total CPU ticks per core from `/proc/stat`,
// writing up to `capacity` entries and reporting the highest core index seen.
static int
cpuinfo__sample(uint64_t *busy, uint64_t *total, unsigned capacity, unsigned *cores) {
  FILE *file = fopen("/proc/stat", "r");

  if (file == NULL) return -1;

  char line[512];

  unsigned max = 0;

  while (fgets(line, sizeof(line), file) != NULL) {
    // Match only the per-core `cpuN` lines, skipping the leading aggregate
    // `cpu` line whose label is not followed by a digit.
    if (strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9') continue;

    unsigned id;
    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;

    int n = sscanf(line + 3, "%u %llu %llu %llu %llu %llu %llu %llu %llu", &id, &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);

    if (n < 5 || id >= capacity) continue;

    uint64_t idle_all = idle + iowait;
    uint64_t busy_all = user + nice + system + irq + softirq + steal;

    busy[id] = busy_all;
    total[id] = busy_all + idle_all;

    if (id + 1 > max) max = id + 1;
  }

  fclose(file);

  *cores = max;

  return 0;
}

static void
cpuinfo__memory(uint64_t total, uint64_t *used) {
  char meminfo[8192];

  if (!cpuinfo__read_file("/proc/meminfo", meminfo, sizeof(meminfo))) {
    *used = 0;

    return;
  }

  uint64_t available = cpuinfo__meminfo_bytes(meminfo, "MemAvailable");

  // Older kernels do not expose `MemAvailable`; approximate it from the free,
  // buffer, and cache pools.
  if (available == 0) {
    available = cpuinfo__meminfo_bytes(meminfo, "MemFree") +
                cpuinfo__meminfo_bytes(meminfo, "Buffers") +
                cpuinfo__meminfo_bytes(meminfo, "Cached");
  }

  *used = available < total ? total - available : 0;
}

int
cpuinfo_init(cpuinfo_t **result) {
  cpuinfo_t *info = calloc(1, sizeof(cpuinfo_t));

  if (info == NULL) return -1;

  cpuinfo__fill_static(&info->info);

  long configured = sysconf(_SC_NPROCESSORS_CONF);

  info->capacity = configured > 0 ? (unsigned) configured : 1;

  info->prev_busy = malloc(info->capacity * sizeof(uint64_t));
  info->prev_total = malloc(info->capacity * sizeof(uint64_t));
  info->core_compute = malloc(info->capacity * sizeof(double));
  info->core = calloc(info->capacity, sizeof(cpuinfo_core_t));

  if (info->prev_busy == NULL || info->prev_total == NULL || info->core_compute == NULL || info->core == NULL) {
    free(info->prev_busy);
    free(info->prev_total);
    free(info->core_compute);
    free(info->core);
    free(info);

    return -1;
  }

  // Capture the static per-core detail now that the arrays are sized.
  cpuinfo__detail(info);

  // Take a baseline sample so that the first utilization query measures the
  // interval since initialization.
  if (cpuinfo__sample(info->prev_busy, info->prev_total, info->capacity, &info->cores) != 0) {
    free(info->prev_busy);
    free(info->prev_total);
    free(info->core_compute);
    free(info->core);
    free(info);

    return -1;
  }

  // No interval has elapsed yet, so per-core utilization is not yet available.
  for (unsigned i = 0; i < info->capacity; i++) {
    info->core_compute[i] = -1.0;
  }

  cpuinfo__memory(info->info.memory, &info->memory_used);

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
cpuinfo_cpu_info(const cpuinfo_t *info, cpuinfo_cpu_t *result) {
  *result = info->info;

  return 0;
}

uint64_t
cpuinfo_features(const cpuinfo_t *info) {
  return info->info.features;
}

bool
cpuinfo_has_feature(const cpuinfo_t *info, cpuinfo_feature_t feature) {
  return (info->info.features & (uint64_t) feature) != 0;
}

int
cpuinfo_cpu_usage(cpuinfo_t *info, cpuinfo_usage_t *result) {
  result->compute = -1.0;
  result->memory_used = 0;
  result->memory_total = info->info.memory;

  cpuinfo__memory(info->info.memory, &result->memory_used);

  info->memory_used = result->memory_used;

  uint64_t *busy = malloc(info->capacity * sizeof(uint64_t));
  uint64_t *total = malloc(info->capacity * sizeof(uint64_t));

  if (busy == NULL || total == NULL) {
    free(busy);
    free(total);

    return -1;
  }

  unsigned cores;

  if (cpuinfo__sample(busy, total, info->capacity, &cores) != 0) {
    free(busy);
    free(total);

    return -1;
  }

  unsigned n = cores < info->cores ? cores : info->cores;

  uint64_t busy_delta = 0;
  uint64_t total_delta = 0;

  for (unsigned i = 0; i < n; i++) {
    uint64_t core_busy = busy[i] - info->prev_busy[i];
    uint64_t core_total = total[i] - info->prev_total[i];

    info->core_compute[i] = core_total > 0 ? (double) core_busy / (double) core_total : -1.0;

    busy_delta += core_busy;
    total_delta += core_total;
  }

  if (total_delta > 0) {
    result->compute = (double) busy_delta / (double) total_delta;
  }

  memcpy(info->prev_busy, busy, info->capacity * sizeof(uint64_t));
  memcpy(info->prev_total, total, info->capacity * sizeof(uint64_t));

  info->cores = cores;

  free(busy);
  free(total);

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

int
cpuinfo_core_times(const cpuinfo_t *info, size_t index, cpuinfo_core_times_t *result) {
  if (index >= info->cores) return -1;

  FILE *file = fopen("/proc/stat", "r");

  if (file == NULL) return -1;

  long ticks_per_second = sysconf(_SC_CLK_TCK);

  uint64_t multiplier = ticks_per_second > 0 ? 1000ull / (uint64_t) ticks_per_second : 0;

  char line[512];

  int found = -1;

  while (fgets(line, sizeof(line), file) != NULL) {
    if (strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9') continue;

    unsigned id;
    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0;

    // Match the fields exposed by `uv_cpu_info()`, skipping iowait.
    int n = sscanf(line + 3, "%u %llu %llu %llu %llu %llu %llu", &id, &user, &nice, &system, &idle, &iowait, &irq);

    if (n < 5 || id != index) continue;

    result->user = (uint64_t) user * multiplier;
    result->nice = (uint64_t) nice * multiplier;
    result->system = (uint64_t) system * multiplier;
    result->idle = (uint64_t) idle * multiplier;
    result->irq = (uint64_t) irq * multiplier;

    found = 0;

    break;
  }

  fclose(file);

  return found;
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
