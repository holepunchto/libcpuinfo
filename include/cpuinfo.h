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
typedef struct cpuinfo_features_s cpuinfo_features_t;

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
 * support, each reported as a single bit within the struct. Intended to replace
 * compile-time feature guards with a runtime check.
 *
 * Each field is named for the architecture it belongs to and is only ever set
 * on that architecture. A capability that exists in spirit on both, such as
 * hardware AES, still has a distinct field per architecture because the
 * underlying instructions differ; portable callers that only care whether the
 * capability is present should test both.
 */
struct cpuinfo_features_s {
  // Arm
  bool arm_neon : 1;    // Advanced SIMD
  bool arm_aes : 1;     // AES acceleration
  bool arm_pmull : 1;   // Polynomial multiply, for GHASH/GCM
  bool arm_sha1 : 1;    // SHA-1 acceleration
  bool arm_sha2 : 1;    // SHA-256 acceleration
  bool arm_sha512 : 1;  // SHA-512 acceleration
  bool arm_sha3 : 1;    // SHA-3 acceleration
  bool arm_crc32 : 1;   // CRC-32 checksum acceleration
  bool arm_atomics : 1; // Large System Extensions (LSE) atomics
  bool arm_dotprod : 1; // Integer dot product
  bool arm_fp16 : 1;    // Half-precision floating point
  bool arm_sve : 1;     // Scalable Vector Extension
  bool arm_sve2 : 1;    // Scalable Vector Extension 2

  // x86
  bool x86_sse2 : 1;            // Streaming SIMD Extensions 2
  bool x86_sse3 : 1;            // Streaming SIMD Extensions 3
  bool x86_ssse3 : 1;           // Supplemental Streaming SIMD Extensions 3
  bool x86_sse4_1 : 1;          // Streaming SIMD Extensions 4.1
  bool x86_sse4_2 : 1;          // Streaming SIMD Extensions 4.2
  bool x86_avx : 1;             // Advanced Vector Extensions
  bool x86_avx2 : 1;            // Advanced Vector Extensions 2
  bool x86_fma : 1;             // Fused multiply-add
  bool x86_bmi : 1;             // Bit-manipulation instructions
  bool x86_bmi2 : 1;            // Bit-manipulation instructions 2
  bool x86_avx512f : 1;         // AVX-512 foundation
  bool x86_avx512cd : 1;        // AVX-512 conflict detection
  bool x86_avx512vl : 1;        // AVX-512 vector length extensions
  bool x86_avx512bitalg : 1;    // AVX-512 bit algorithms
  bool x86_avx512vpopcntdq : 1; // AVX-512 vectorized population count
  bool x86_aes : 1;             // AES-NI
  bool x86_pclmulqdq : 1;       // Carry-less multiply, for GHASH/GCM
  bool x86_sha : 1;             // SHA-1 and SHA-256 acceleration
  bool x86_popcnt : 1;          // Population count
  bool x86_rdrand : 1;          // On-chip random number generator
  bool x86_rdseed : 1;          // Seed-grade random number generator
  bool x86_adx : 1;             // Multi-precision add-carry, for bignum arithmetic
  bool x86_f16c : 1;            // Half-precision float conversion
  bool x86_vaes : 1;            // Vectorized AES
  bool x86_vpclmulqdq : 1;      // Vectorized carry-less multiply
};

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
   * The instruction set extensions supported by the CPU.
   */
  cpuinfo_features_t features;

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
   * across all logical cores since the previous call to `cpuinfo_sample()`,
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
cpuinfo_query(const cpuinfo_t *info, cpuinfo_cpu_t *result);

/**
 * Get the instruction set extensions supported by the CPU.
 *
 * Returns `0` on success or a negative value on failure.
 */
int
cpuinfo_features(const cpuinfo_t *info, cpuinfo_features_t *result);

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
cpuinfo_sample(cpuinfo_t *info, cpuinfo_usage_t *result);

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
 * Unlike `cpuinfo_sample()`, this does not sample the CPU itself; it reports
 * the per-core figures captured by the most recent `cpuinfo_sample()` call.
 * Call `cpuinfo_sample()` first to refresh the snapshot; before the first
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
