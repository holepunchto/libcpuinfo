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
 *
 * Each value is named for the architecture it belongs to and is only ever set
 * on that architecture. A capability that exists in spirit on both, such as
 * hardware AES, still has a distinct bit per architecture because the
 * underlying instructions differ; portable callers that only care whether the
 * capability is present should test both.
 */
typedef uint64_t cpuinfo_feature_t;

/**
 * Arm.
 */
#define cpuinfo_feature_arm_neon    (UINT64_C(1) << 0)  // Advanced SIMD.
#define cpuinfo_feature_arm_aes     (UINT64_C(1) << 1)  // AES acceleration.
#define cpuinfo_feature_arm_pmull   (UINT64_C(1) << 2)  // Polynomial multiply, for GHASH/GCM.
#define cpuinfo_feature_arm_sha1    (UINT64_C(1) << 3)  // SHA-1 acceleration.
#define cpuinfo_feature_arm_sha2    (UINT64_C(1) << 4)  // SHA-256 acceleration.
#define cpuinfo_feature_arm_sha512  (UINT64_C(1) << 5)  // SHA-512 acceleration.
#define cpuinfo_feature_arm_sha3    (UINT64_C(1) << 6)  // SHA-3 acceleration.
#define cpuinfo_feature_arm_crc32   (UINT64_C(1) << 7)  // CRC-32 checksum acceleration.
#define cpuinfo_feature_arm_atomics (UINT64_C(1) << 8)  // Large System Extensions (LSE) atomics.
#define cpuinfo_feature_arm_dotprod (UINT64_C(1) << 9)  // Integer dot product.
#define cpuinfo_feature_arm_fp16    (UINT64_C(1) << 10) // Half-precision floating point.
#define cpuinfo_feature_arm_sve     (UINT64_C(1) << 11) // Scalable Vector Extension.
#define cpuinfo_feature_arm_sve2    (UINT64_C(1) << 12) // Scalable Vector Extension 2.

/**
 * x86.
 */
#define cpuinfo_feature_x86_sse2            (UINT64_C(1) << 13)
#define cpuinfo_feature_x86_sse3            (UINT64_C(1) << 14)
#define cpuinfo_feature_x86_ssse3           (UINT64_C(1) << 15)
#define cpuinfo_feature_x86_sse4_1          (UINT64_C(1) << 16)
#define cpuinfo_feature_x86_sse4_2          (UINT64_C(1) << 17)
#define cpuinfo_feature_x86_avx             (UINT64_C(1) << 18)
#define cpuinfo_feature_x86_avx2            (UINT64_C(1) << 19)
#define cpuinfo_feature_x86_fma             (UINT64_C(1) << 20)
#define cpuinfo_feature_x86_bmi             (UINT64_C(1) << 21)
#define cpuinfo_feature_x86_bmi2            (UINT64_C(1) << 22)
#define cpuinfo_feature_x86_avx512f         (UINT64_C(1) << 23)
#define cpuinfo_feature_x86_avx512cd        (UINT64_C(1) << 24)
#define cpuinfo_feature_x86_avx512vl        (UINT64_C(1) << 25)
#define cpuinfo_feature_x86_avx512bitalg    (UINT64_C(1) << 26)
#define cpuinfo_feature_x86_avx512vpopcntdq (UINT64_C(1) << 27)
#define cpuinfo_feature_x86_aes             (UINT64_C(1) << 28) // AES-NI.
#define cpuinfo_feature_x86_pclmulqdq       (UINT64_C(1) << 29) // Carry-less multiply, for GHASH/GCM.
#define cpuinfo_feature_x86_sha             (UINT64_C(1) << 30) // SHA-1 and SHA-256 acceleration.
#define cpuinfo_feature_x86_popcnt          (UINT64_C(1) << 31) // Population count.
#define cpuinfo_feature_x86_rdrand          (UINT64_C(1) << 32) // On-chip random number generator.
#define cpuinfo_feature_x86_rdseed          (UINT64_C(1) << 33) // Seed-grade random number generator.
#define cpuinfo_feature_x86_adx             (UINT64_C(1) << 34) // Multi-precision add-carry, for bignum arithmetic.
#define cpuinfo_feature_x86_f16c            (UINT64_C(1) << 35) // Half-precision float conversion.
#define cpuinfo_feature_x86_vaes            (UINT64_C(1) << 36) // Vectorized AES.
#define cpuinfo_feature_x86_vpclmulqdq      (UINT64_C(1) << 37) // Vectorized carry-less multiply.

/**
 * The role a logical core plays on a hybrid CPU. `unknown` is reported for a
 * homogeneous CPU, or when the role could not be determined.
 */
typedef enum {
  cpuinfo_core_type_unknown = 0,
  cpuinfo_core_type_performance,
  cpuinfo_core_type_efficiency,
} cpuinfo_core_type_t;

/**
 * A cache level, used to select which cache `cpuinfo_core_cache()` reports. The
 * two level 1 caches are distinguished as the data and instruction caches; the
 * level 2 and level 3 caches are unified.
 */
typedef enum {
  cpuinfo_cache_l1d = 0,
  cpuinfo_cache_l1i,
  cpuinfo_cache_l2,
  cpuinfo_cache_l3,
} cpuinfo_cache_level_t;

/**
 * The number of distinct cache levels reported by `cpuinfo_cache_level_t`.
 */
#define CPUINFO_CACHE_LEVELS 4

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
   * On a hybrid CPU with more than one type of core, the number of physical
   * high-performance ("P") and energy-efficient ("E") cores, respectively.
   * Both are `0` on a homogeneous CPU, or when the split cannot be determined;
   * in that case treat all `physical_cores` as equivalent.
   */
  uint32_t performance_cores;
  uint32_t efficiency_cores;

  /**
   * The nominal frequency of the CPU, in hertz. `0` if unknown, as is the case
   * on platforms that do not report a fixed frequency.
   */
  uint64_t frequency;

  /**
   * The size, in bytes, of a cache line. `0` if unknown.
   */
  uint32_t cache_line;

  /**
   * The size, in bytes, of the shared last-level (level 3) cache, or `0` if it
   * is absent or could not be determined. The level 1 and level 2 caches are
   * private to a core and differ between core types on a hybrid CPU, so they
   * are reported per core by `cpuinfo_core_cache()` rather than here.
   */
  uint64_t l3_cache;

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
 * `cpuinfo_core_usage()`. This is usually equal to `logical_cores`, but may be
 * smaller; on Windows only the first processor group is sampled, so a system
 * with more than 64 logical processors reports at most 64 here.
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
 * Get the type of the logical core at the given index, where `index` is in the
 * range `[0, cpuinfo_core_count())`. Returns `cpuinfo_core_type_unknown` for an
 * out-of-range index, a homogeneous CPU, or a platform that does not expose the
 * distinction.
 *
 * Like the other static properties, the per-core detail is captured once at
 * `cpuinfo_init()` and does not change over the lifetime of the context.
 */
cpuinfo_core_type_t
cpuinfo_core_type(const cpuinfo_t *info, size_t index);

/**
 * Get the nominal maximum frequency, in hertz, of the logical core at the given
 * index, where `index` is in the range `[0, cpuinfo_core_count())`. On a hybrid
 * CPU the performance and efficiency cores typically differ. Returns `0` for an
 * out-of-range index or when the per-core frequency is not reported, such as on
 * Apple silicon.
 */
uint64_t
cpuinfo_core_frequency(const cpuinfo_t *info, size_t index);

/**
 * Get the size, in bytes, of the given cache `level` for the logical core at
 * the given index, where `index` is in the range `[0, cpuinfo_core_count())`.
 * Returns `0` for an out-of-range index or level, or when the cache is absent
 * or could not be determined.
 */
uint64_t
cpuinfo_core_cache(const cpuinfo_t *info, size_t index, cpuinfo_cache_level_t level);

#ifdef __cplusplus
}
#endif

#endif // CPUINFO_H
