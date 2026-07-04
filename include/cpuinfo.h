#ifndef CPUINFO_H
#define CPUINFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * The maximum length, including the terminating NUL byte, of the various
 * human-readable name strings reported by the library.
 */
#define CPUINFO_NAME_MAX 256

typedef struct cpuinfo_s cpuinfo_t;
typedef struct cpuinfo_cpu_s cpuinfo_cpu_t;
typedef struct cpuinfo_usage_s cpuinfo_usage_t;
typedef struct cpuinfo_core_times_s cpuinfo_core_times_t;

/**
 * The instruction set architecture of the CPU.
 */
typedef enum {
  cpuinfo_arch_unknown = 0,
  cpuinfo_arch_x86,
  cpuinfo_arch_x86_64,
  cpuinfo_arch_arm,
  cpuinfo_arch_arm64,
} cpuinfo_arch_t;

/**
 * The optional instruction set extensions, or "features", that a CPU may
 * support. Reported as a bitmask so that support for several extensions can be
 * queried at once. Intended to replace compile-time feature guards with a
 * runtime check.
 */
typedef enum {
  /**
   * Arm.
   */
  cpuinfo_feature_neon = 1ull << 0,

  /**
   * Intel.
   */
  cpuinfo_feature_sse2 = 1ull << 1,
  cpuinfo_feature_sse3 = 1ull << 2,
  cpuinfo_feature_ssse3 = 1ull << 3,
  cpuinfo_feature_sse4_1 = 1ull << 4,
  cpuinfo_feature_sse4_2 = 1ull << 5,
  cpuinfo_feature_avx = 1ull << 6,
  cpuinfo_feature_avx2 = 1ull << 7,
  cpuinfo_feature_fma = 1ull << 8,
  cpuinfo_feature_bmi = 1ull << 9,
  cpuinfo_feature_bmi2 = 1ull << 10,
  cpuinfo_feature_avx512f = 1ull << 11,
  cpuinfo_feature_avx512cd = 1ull << 12,
  cpuinfo_feature_avx512vl = 1ull << 13,
  cpuinfo_feature_avx512bitalg = 1ull << 14,
  cpuinfo_feature_avx512vpopcntdq = 1ull << 15,
} cpuinfo_feature_t;

/**
 * A snapshot of the CPU installed in the system. The values are static for the
 * lifetime of the process and describe the hardware rather than its current
 * load; see `cpuinfo_usage_t` for runtime utilization.
 */
struct cpuinfo_cpu_s {
  /**
   * The human-readable model name of the CPU, NUL-terminated. Empty if unknown.
   */
  char name[CPUINFO_NAME_MAX];

  /**
   * The human-readable vendor name of the CPU, NUL-terminated. Empty if
   * unknown.
   */
  char vendor[CPUINFO_NAME_MAX];

  /**
   * The instruction set architecture of the CPU.
   */
  cpuinfo_arch_t arch;

  /**
   * A bitmask of the `cpuinfo_feature_t` extensions supported by the CPU.
   */
  uint64_t features;

  /**
   * The number of physical cores.
   */
  uint32_t physical_cores;

  /**
   * The number of logical cores, i.e. hardware threads.
   */
  uint32_t logical_cores;

  /**
   * The nominal frequency of the CPU, in hertz. `0` if unknown, as is the case
   * on platforms that do not report a fixed frequency.
   */
  uint64_t frequency;

  /**
   * The total amount of installed physical memory, in bytes.
   */
  uint64_t memory;
};

/**
 * A snapshot of the runtime utilization of the CPU, sampled at the time the
 * enclosing query returns.
 */
struct cpuinfo_usage_s {
  /**
   * The fraction of compute capacity in use, in the range `[0, 1]`, averaged
   * across all logical cores since the previous call to `cpuinfo_cpu_usage()`,
   * or since `cpuinfo_init()` for the first call. A negative value indicates
   * that compute utilization could not be determined on this platform.
   */
  double compute;

  /**
   * The amount of physical memory currently in use, in bytes.
   */
  uint64_t memory_used;

  /**
   * The total amount of installed physical memory, in bytes.
   */
  uint64_t memory_total;
};

/**
 * The cumulative time, in milliseconds, that a logical core has spent in each
 * scheduling state since boot. These are the raw counters from which
 * utilization is derived; sample them at two points in time and compare to
 * measure load over the interval. The units and fields match those reported by
 * `uv_cpu_info()`.
 */
struct cpuinfo_core_times_s {
  uint64_t user;

  uint64_t nice;

  uint64_t system;

  uint64_t idle;

  /**
   * Time spent servicing interrupts. Always `0` on platforms that do not
   * account for it separately, such as macOS.
   */
  uint64_t irq;
};

/**
 * Initialize a query context. The context detects the static properties of the
 * CPU up front, and additionally retains the state needed to compute
 * utilization as a delta between successive samples.
 *
 * Returns `0` on success or a negative value on failure.
 */
int
cpuinfo_init(cpuinfo_t **result);

/**
 * Destroy a query context previously initialized with `cpuinfo_init()`.
 */
void
cpuinfo_destroy(cpuinfo_t *info);

/**
 * Get static information about the CPU installed in the system.
 *
 * Returns `0` on success or a negative value on failure.
 */
int
cpuinfo_cpu_info(const cpuinfo_t *info, cpuinfo_cpu_t *result);

/**
 * Get a bitmask of the `cpuinfo_feature_t` extensions supported by the CPU.
 */
uint64_t
cpuinfo_features(const cpuinfo_t *info);

/**
 * Check whether the CPU supports the given instruction set extension.
 */
bool
cpuinfo_has_feature(const cpuinfo_t *info, cpuinfo_feature_t feature);

/**
 * Sample the runtime utilization of the CPU. Compute utilization is reported as
 * the average load across all logical cores since the previous call, or since
 * `cpuinfo_init()` for the first call.
 *
 * This is the only call that advances the sampling state. It additionally
 * refreshes the per-core snapshot subsequently read by `cpuinfo_core_usage()`.
 *
 * Returns `0` on success or a negative value on failure.
 */
int
cpuinfo_cpu_usage(cpuinfo_t *info, cpuinfo_usage_t *result);

/**
 * Get the number of logical cores that can be sampled individually with
 * `cpuinfo_core_usage()`.
 */
size_t
cpuinfo_core_count(const cpuinfo_t *info);

/**
 * Read the runtime utilization of the logical core at the given index, where
 * `index` is in the range `[0, cpuinfo_core_count())`.
 *
 * Unlike `cpuinfo_cpu_usage()`, this does not sample the CPU itself; it reports
 * the per-core figures captured by the most recent `cpuinfo_cpu_usage()` call.
 * Call `cpuinfo_cpu_usage()` first to refresh the snapshot; before the first
 * such call the reported compute utilization is negative. The memory fields
 * carry the system-wide values, which are not partitioned per core.
 *
 * Returns `0` on success or a negative value on failure, such as when `index`
 * is out of range.
 */
int
cpuinfo_core_usage(const cpuinfo_t *info, size_t index, cpuinfo_usage_t *result);

/**
 * Read the cumulative scheduling times of the logical core at the given index,
 * where `index` is in the range `[0, cpuinfo_core_count())`.
 *
 * Unlike `cpuinfo_core_usage()`, this reads a fresh snapshot of the raw,
 * monotonically increasing counters on every call and does not touch the
 * sampling state used by `cpuinfo_cpu_usage()`. It is the stateless equivalent
 * of `uv_cpu_info()`, intended for callers that prefer to compute deltas
 * themselves.
 *
 * Returns `0` on success or a negative value on failure, such as when `index`
 * is out of range.
 */
int
cpuinfo_core_times(const cpuinfo_t *info, size_t index, cpuinfo_core_times_t *result);

#ifdef __cplusplus
}
#endif

#endif // CPUINFO_H
