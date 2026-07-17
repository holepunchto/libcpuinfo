#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include <powrprof.h>

#include "../include/cpuinfo.h"
#include "x86.h"

// The processor-feature identifiers accepted by `IsProcessorFeaturePresent`.
// They are defined here with fallbacks so that the Arm feature probes build
// against older SDK headers; querying an identifier the running kernel does not
// recognize simply returns `FALSE`.
#ifndef PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE
#define PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE 30
#endif
#ifndef PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE
#define PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE 31
#endif
#ifndef PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE
#define PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE 34
#endif
#ifndef PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE
#define PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE 43
#endif
#ifndef PF_ARM_SHA3_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SHA3_INSTRUCTIONS_AVAILABLE 64
#endif
#ifndef PF_ARM_SHA512_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SHA512_INSTRUCTIONS_AVAILABLE 65
#endif

// `SystemProcessorPerformanceInformation` and its result structure are not
// declared in the public SDK headers, so they are reproduced here. The call is
// resolved from `ntdll` at runtime.
#define CPUINFO_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION 8

typedef struct {
  LARGE_INTEGER IdleTime;
  LARGE_INTEGER KernelTime; // Includes idle time.
  LARGE_INTEGER UserTime;
  LARGE_INTEGER DpcTime;
  LARGE_INTEGER InterruptTime;
  ULONG InterruptCount;
} cpuinfo_processor_performance_t;

// `PROCESSOR_POWER_INFORMATION` is documented but was omitted from the SDK
// headers, so its layout is reproduced here. `CallNtPowerInformation()` fills
// one entry per logical processor with, among others, the maximum frequency in
// megahertz.
typedef struct {
  ULONG Number;
  ULONG MaxMhz;
  ULONG CurrentMhz;
  ULONG MhzLimit;
  ULONG MaxIdleState;
  ULONG CurrentIdleState;
} cpuinfo_processor_power_t;

typedef LONG(WINAPI *cpuinfo_query_t)(ULONG, PVOID, ULONG, PULONG);

// The static per-core detail captured once at initialization.
typedef struct {
  cpuinfo_core_type_t type;
  uint64_t frequency;
  uint64_t cache[CPUINFO_CACHE_LEVELS];
} cpuinfo_core_t;

struct cpuinfo_s {
  cpuinfo_cpu_t info;

  cpuinfo_query_t query;

  // The number of logical processors actually reported by the kernel.
  unsigned cores;

  // The capacity of the per-core arrays, sized to the advertised logical
  // processor count so that they never need to grow.
  unsigned capacity;

  // The cumulative busy and total 100-nanosecond intervals per core at the
  // previous sample, used to derive utilization as a delta.
  uint64_t *prev_busy;
  uint64_t *prev_total;

  // The per-core compute utilization captured by the most recent
  // `cpuinfo_sample()` call, negative until the first such call.
  double *core_compute;

  // The static per-core detail, indexed by logical processor.
  cpuinfo_core_t *core;

  // The system-wide memory usage captured by the most recent
  // `cpuinfo_sample()` call, or `-1` if it could not be determined.
  int64_t memory_used;
};

static cpuinfo_arch_t
cpuinfo__arch(void) {
  SYSTEM_INFO system;

  GetNativeSystemInfo(&system);

  switch (system.wProcessorArchitecture) {
  case PROCESSOR_ARCHITECTURE_AMD64:
    return cpuinfo_arch_x86_64;
  case PROCESSOR_ARCHITECTURE_INTEL:
    return cpuinfo_arch_x86;
  case PROCESSOR_ARCHITECTURE_ARM64:
    return cpuinfo_arch_arm64;
  case PROCESSOR_ARCHITECTURE_ARM:
    return cpuinfo_arch_arm;
  }

  return cpuinfo_arch_unknown;
}

static cpuinfo_features_t
cpuinfo__features(void) {
#if defined(CPUINFO_X86)
  return cpuinfo__cpuid_features();
#else
  cpuinfo_features_t features = {0};

  // Advanced SIMD is mandatory on the ARM64 systems Windows supports.
  features.arm_neon = true;

  // The Armv8 cryptographic extension is reported as a single coarse flag that
  // implies the AES, PMULL, SHA-1, and SHA-256 instructions together.
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE)) {
    features.arm_aes = true;
    features.arm_pmull = true;
    features.arm_sha1 = true;
    features.arm_sha2 = true;
  }

  if (IsProcessorFeaturePresent(PF_ARM_SHA512_INSTRUCTIONS_AVAILABLE)) features.arm_sha512 = true;
  if (IsProcessorFeaturePresent(PF_ARM_SHA3_INSTRUCTIONS_AVAILABLE)) features.arm_sha3 = true;
  if (IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE)) features.arm_crc32 = true;
  if (IsProcessorFeaturePresent(PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE)) features.arm_atomics = true;
  if (IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE)) features.arm_dotprod = true;

  return features;
#endif
}

// Count physical cores across all processor groups and, on a hybrid CPU, split
// them into performance and efficiency tiers by their efficiency class (higher
// is more performant). `GetLogicalProcessorInformationEx` returns variable-length
// records that must be walked by their `Size` field, and unlike the non-`Ex`
// form is not limited to the 64 processors of a single group.
static uint32_t
cpuinfo__physical_cores(cpuinfo_cpu_t *cpu) {
  DWORD length = 0;

  GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &length);

  if (length == 0) return 0;

  BYTE *buffer = malloc(length);

  if (buffer == NULL) return 0;

  uint32_t count = 0;

  if (GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) buffer, &length)) {
    BYTE *end = buffer + length;

    // Find the highest efficiency class present; those cores are the
    // performance tier and any below them the efficiency tier.
    BYTE top = 0;

    for (BYTE *ptr = buffer; ptr < end;) {
      SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *record = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) ptr;

      if (record->Relationship == RelationProcessorCore && record->Processor.EfficiencyClass > top) {
        top = record->Processor.EfficiencyClass;
      }

      ptr += record->Size;
    }

    uint32_t performance = 0;
    uint32_t efficiency = 0;

    for (BYTE *ptr = buffer; ptr < end;) {
      SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *record = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) ptr;

      if (record->Relationship == RelationProcessorCore) {
        count++;

        if (record->Processor.EfficiencyClass < top) efficiency++;
        else performance++;
      }

      ptr += record->Size;
    }

    // Only report a split when the cores actually differ; otherwise the CPU is
    // homogeneous and the tiers are left at zero.
    if (efficiency > 0) {
      cpu->performance_cores = performance;
      cpu->efficiency_cores = efficiency;
    }
  }

  free(buffer);

  return count;
}

// Fill in the coherency line size by walking the cache relationships. The
// per-core cache sizes, including the last-level cache, are captured separately
// by `cpuinfo__detail()` and reported per core rather than aggregated here.
static void
cpuinfo__cache(cpuinfo_cpu_t *cpu) {
  DWORD length = 0;

  GetLogicalProcessorInformationEx(RelationCache, NULL, &length);

  if (length == 0) return;

  BYTE *buffer = malloc(length);

  if (buffer == NULL) return;

  if (GetLogicalProcessorInformationEx(RelationCache, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) buffer, &length)) {
    BYTE *end = buffer + length;

    for (BYTE *ptr = buffer; ptr < end;) {
      SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *record = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) ptr;

      if (record->Relationship == RelationCache) {
        CACHE_RELATIONSHIP *cache = &record->Cache;

        if (cpu->cache_line == 0) cpu->cache_line = cache->LineSize;
      }

      ptr += record->Size;
    }
  }

  free(buffer);
}

static void
cpuinfo__fill_static(cpuinfo_cpu_t *cpu) {
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

  DWORD name_size = sizeof(cpu->name);

  RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", RRF_RT_REG_SZ, NULL, cpu->name, &name_size);
#endif

  // Count logical processors across all groups; `GetSystemInfo` reports only
  // those in the calling thread's group, capping at 64.
  cpu->logical_cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);

  uint32_t physical = cpuinfo__physical_cores(cpu);

  cpu->physical_cores = physical > 0 ? physical : cpu->logical_cores;

  cpuinfo__cache(cpu);

  // The advertised frequency is reported in megahertz by the registry.
  DWORD mhz = 0;
  DWORD size = sizeof(mhz);

  if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz", RRF_RT_REG_DWORD, NULL, &mhz, &size) == ERROR_SUCCESS) {
    cpu->frequency = (uint64_t) mhz * 1000000;
  } else {
    cpu->frequency = 0;
  }

  MEMORYSTATUSEX memory;
  memory.dwLength = sizeof(memory);

  if (GlobalMemoryStatusEx(&memory)) {
    cpu->memory = (int64_t) memory.ullTotalPhys;
  } else {
    cpu->memory = -1;
  }
}

// Sample the cumulative busy and total 100-nanosecond intervals per core. On
// Windows `KernelTime` includes idle time, so busy time is the kernel and user
// time less the idle time.
//
// `SystemProcessorPerformanceInformation` reports only the processors in the
// calling thread's processor group, which caps at 64. Systems with more than
// one group (over 64 logical processors) are not supported for sampling: only
// the processors of the first group are reported, and `cpuinfo_core_count()`
// reflects that even though `logical_cores` still counts every group.
static int
cpuinfo__sample(cpuinfo_t *info, uint64_t *busy, uint64_t *total, unsigned *cores) {
  unsigned capacity = info->capacity;

  cpuinfo_processor_performance_t *performance = malloc(capacity * sizeof(cpuinfo_processor_performance_t));

  if (performance == NULL) return -1;

  ULONG length = 0;

  LONG status = info->query(CPUINFO_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION, performance, (ULONG) (capacity * sizeof(cpuinfo_processor_performance_t)), &length);

  if (status < 0) {
    free(performance);

    return -1;
  }

  unsigned n = length / sizeof(cpuinfo_processor_performance_t);

  if (n > capacity) n = capacity;

  for (unsigned i = 0; i < n; i++) {
    uint64_t idle = (uint64_t) performance[i].IdleTime.QuadPart;
    uint64_t kernel = (uint64_t) performance[i].KernelTime.QuadPart;
    uint64_t user = (uint64_t) performance[i].UserTime.QuadPart;

    total[i] = kernel + user;
    busy[i] = total[i] - idle;
  }

  free(performance);

  *cores = n;

  return 0;
}

static void
cpuinfo__memory(int64_t *used) {
  MEMORYSTATUSEX memory;
  memory.dwLength = sizeof(memory);

  if (GlobalMemoryStatusEx(&memory)) {
    *used = (int64_t) (memory.ullTotalPhys - memory.ullAvailPhys);
  } else {
    *used = -1;
  }
}

// Capture the static per-core detail: the core type, maximum frequency, and
// cache sizes, each keyed to the flat logical processor index used elsewhere.
// A processor group's mask is group-relative, so it is offset by the number of
// processors in the preceding groups to recover that flat index.
static void
cpuinfo__detail(cpuinfo_t *info) {
  unsigned capacity = info->capacity;

  WORD groups = GetActiveProcessorGroupCount();

  DWORD *base = malloc((groups > 0 ? groups : 1) * sizeof(DWORD));

  if (base == NULL) return;

  DWORD total = 0;

  for (WORD g = 0; g < groups; g++) {
    base[g] = total;
    total += GetActiveProcessorCount(g);
  }

  // Per-core type from the efficiency class, higher being more performant.
  DWORD length = 0;

  GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &length);

  BYTE *buffer = length > 0 ? malloc(length) : NULL;

  if (buffer != NULL && GetLogicalProcessorInformationEx(RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) buffer, &length)) {
    BYTE *end = buffer + length;

    BYTE top = 0;

    for (BYTE *ptr = buffer; ptr < end;) {
      SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *record = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) ptr;

      if (record->Relationship == RelationProcessorCore && record->Processor.EfficiencyClass > top) {
        top = record->Processor.EfficiencyClass;
      }

      ptr += record->Size;
    }

    // The type is left unknown on a homogeneous CPU, where every class is zero.
    if (top > 0) {
      for (BYTE *ptr = buffer; ptr < end;) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *record = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) ptr;

        if (record->Relationship == RelationProcessorCore) {
          cpuinfo_core_type_t type = record->Processor.EfficiencyClass < top ? cpuinfo_core_type_efficiency : cpuinfo_core_type_performance;

          for (WORD gi = 0; gi < record->Processor.GroupCount; gi++) {
            GROUP_AFFINITY mask = record->Processor.GroupMask[gi];

            if (mask.Group >= groups) continue;

            for (unsigned b = 0; b < sizeof(KAFFINITY) * 8; b++) {
              if ((mask.Mask >> b) & 1) {
                DWORD index = base[mask.Group] + b;

                if (index < capacity) info->core[index].type = type;
              }
            }
          }
        }

        ptr += record->Size;
      }
    }
  }

  free(buffer);

  // Per-core cache sizes. Each cache lists the processors that share it; the
  // single `GroupMask` covers one processor group, sufficient for the common
  // case of a system with a single group.
  length = 0;

  GetLogicalProcessorInformationEx(RelationCache, NULL, &length);

  buffer = length > 0 ? malloc(length) : NULL;

  if (buffer != NULL && GetLogicalProcessorInformationEx(RelationCache, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) buffer, &length)) {
    BYTE *end = buffer + length;

    for (BYTE *ptr = buffer; ptr < end;) {
      SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *record = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *) ptr;

      if (record->Relationship == RelationCache) {
        CACHE_RELATIONSHIP *cache = &record->Cache;

        GROUP_AFFINITY mask = cache->GroupMask;

        if (mask.Group < groups) {
          for (unsigned b = 0; b < sizeof(KAFFINITY) * 8; b++) {
            if (((mask.Mask >> b) & 1) == 0) continue;

            DWORD index = base[mask.Group] + b;

            if (index >= capacity) continue;

            uint64_t *c = info->core[index].cache;

            switch (cache->Level) {
            case 1:
              if (cache->Type != CacheInstruction) c[cpuinfo_cache_l1d] = cache->CacheSize;
              if (cache->Type != CacheData) c[cpuinfo_cache_l1i] = cache->CacheSize;
              break;
            case 2:
              c[cpuinfo_cache_l2] = cache->CacheSize;
              break;
            case 3:
              c[cpuinfo_cache_l3] = cache->CacheSize;
              break;
            }
          }
        }
      }

      ptr += record->Size;
    }
  }

  free(buffer);

  // Per-core maximum frequency. `CallNtPowerInformation()` reports only the
  // processors in the calling thread's group, so its group-relative numbers are
  // mapped back to flat indices; other groups are left without a frequency.
  cpuinfo_processor_power_t *power = malloc(capacity * sizeof(cpuinfo_processor_power_t));

  if (power != NULL) {
    PROCESSOR_NUMBER current;
    GetCurrentProcessorNumberEx(&current);

    DWORD in_group = current.Group < groups ? GetActiveProcessorCount(current.Group) : 0;

    if (in_group > 0 && CallNtPowerInformation(ProcessorInformation, NULL, 0, power, (ULONG) (in_group * sizeof(cpuinfo_processor_power_t))) == 0) {
      for (DWORD i = 0; i < in_group; i++) {
        DWORD index = base[current.Group] + power[i].Number;

        if (index < capacity) info->core[index].frequency = (uint64_t) power[i].MaxMhz * 1000000;
      }
    }
  }

  free(power);
  free(base);
}

int
cpuinfo_init(cpuinfo_t **result) {
  cpuinfo_t *info = calloc(1, sizeof(cpuinfo_t));

  if (info == NULL) return -1;

  HMODULE ntdll = GetModuleHandleA("ntdll.dll");

  info->query = ntdll != NULL ? (cpuinfo_query_t) (void *) GetProcAddress(ntdll, "NtQuerySystemInformation") : NULL;

  if (info->query == NULL) {
    free(info);

    return -1;
  }

  cpuinfo__fill_static(&info->info);

  info->capacity = info->info.logical_cores;

  if (info->capacity == 0) info->capacity = 1;

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
  if (cpuinfo__sample(info, info->prev_busy, info->prev_total, &info->cores) != 0) {
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

  cpuinfo__memory(&info->memory_used);

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
  result->memory_used = -1;
  result->memory_total = info->info.memory;

  cpuinfo__memory(&result->memory_used);

  info->memory_used = result->memory_used;

  uint64_t *busy = malloc(info->capacity * sizeof(uint64_t));
  uint64_t *total = malloc(info->capacity * sizeof(uint64_t));

  if (busy == NULL || total == NULL) {
    free(busy);
    free(total);

    return -1;
  }

  unsigned cores;

  if (cpuinfo__sample(info, busy, total, &cores) != 0) {
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

    // A core with a zero-length interval observed no activity and reads as `0`.
    info->core_compute[i] = core_total > 0 ? (double) core_busy / (double) core_total : 0.0;

    busy_delta += core_busy;
    total_delta += core_total;
  }

  // Sampling succeeded, so utilization is measurable on this platform; an
  // interval too short to observe any activity reads as `0` rather than the
  // "unavailable" sentinel.
  result->compute = total_delta > 0 ? (double) busy_delta / (double) total_delta : 0.0;

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
