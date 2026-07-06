#ifndef CPUINFO_X86_H
#define CPUINFO_X86_H

// Detection of x86 instruction set extensions via the `cpuid` instruction,
// shared across the platform backends. Including this header on a non-x86
// target leaves `CPUINFO_X86` undefined and defines nothing else.

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define CPUINFO_X86 1

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../include/cpuinfo.h"

#if defined(_MSC_VER)
#include <immintrin.h>
#include <intrin.h>
#else
#include <cpuid.h>
#endif

static inline void
cpuinfo__cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4]) {
#if defined(_MSC_VER)
  int registers[4];

  __cpuidex(registers, (int) leaf, (int) subleaf);

  out[0] = (uint32_t) registers[0];
  out[1] = (uint32_t) registers[1];
  out[2] = (uint32_t) registers[2];
  out[3] = (uint32_t) registers[3];
#else
  __cpuid_count(leaf, subleaf, out[0], out[1], out[2], out[3]);
#endif
}

// Read the extended control register that reports which register state the OS
// has enabled. Only valid to call once the OSXSAVE bit has been observed.
static inline uint64_t
cpuinfo__xgetbv(void) {
#if defined(_MSC_VER)
  return _xgetbv(0);
#else
  uint32_t eax, edx;

  __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));

  return ((uint64_t) edx << 32) | eax;
#endif
}

static inline cpuinfo_features_t
cpuinfo__cpuid_features(void) {
  cpuinfo_features_t features = {0};

  uint32_t registers[4];

  cpuinfo__cpuid(0, 0, registers);

  uint32_t max_leaf = registers[0];

  if (max_leaf < 1) return features;

  cpuinfo__cpuid(1, 0, registers);

  uint32_t ecx = registers[2];
  uint32_t edx = registers[3];

  if (edx & (1u << 26)) features.x86_sse2 = true;
  if (ecx & (1u << 0)) features.x86_sse3 = true;
  if (ecx & (1u << 9)) features.x86_ssse3 = true;
  if (ecx & (1u << 19)) features.x86_sse4_1 = true;
  if (ecx & (1u << 20)) features.x86_sse4_2 = true;

  // These operate on general-purpose or legacy SSE register state, which is
  // always enabled, so they do not depend on the OS having enabled the extended
  // AVX state inspected below.
  if (ecx & (1u << 23)) features.x86_popcnt = true;
  if (ecx & (1u << 25)) features.x86_aes = true;
  if (ecx & (1u << 1)) features.x86_pclmulqdq = true;
  if (ecx & (1u << 30)) features.x86_rdrand = true;

  // The AVX register state must also be enabled by the OS before the extensions
  // that use it can be reported, which requires inspecting XCR0 via `xgetbv`.
  bool osxsave = (ecx & (1u << 27)) != 0;

  uint64_t xcr0 = osxsave ? cpuinfo__xgetbv() : 0;

  bool avx_enabled = (xcr0 & 0x6) == 0x6;      // XMM and YMM state
  bool avx512_enabled = (xcr0 & 0xe6) == 0xe6; // XMM, YMM, and the AVX-512 opmask and ZMM state

  if (avx_enabled) {
    if (ecx & (1u << 28)) features.x86_avx = true;
    if (ecx & (1u << 12)) features.x86_fma = true;

    // Half-precision conversion is VEX-encoded and so requires the AVX state.
    if (ecx & (1u << 29)) features.x86_f16c = true;
  }

  if (max_leaf >= 7) {
    cpuinfo__cpuid(7, 0, registers);

    uint32_t ebx7 = registers[1];
    uint32_t ecx7 = registers[2];

    // These operate on general-purpose registers or legacy SSE state and so do
    // not depend on the extended AVX register state.
    if (ebx7 & (1u << 3)) features.x86_bmi = true;
    if (ebx7 & (1u << 8)) features.x86_bmi2 = true;
    if (ebx7 & (1u << 29)) features.x86_sha = true;
    if (ebx7 & (1u << 18)) features.x86_rdseed = true;
    if (ebx7 & (1u << 19)) features.x86_adx = true;

    if (avx_enabled) {
      if (ebx7 & (1u << 5)) features.x86_avx2 = true;

      // The vectorized AES and carry-less multiply instructions are enumerated
      // independently of AVX-512; their VEX-encoded forms need only the AVX
      // state, while the 512-bit EVEX forms additionally require AVX-512.
      if (ecx7 & (1u << 9)) features.x86_vaes = true;
      if (ecx7 & (1u << 10)) features.x86_vpclmulqdq = true;
    }

    if (avx512_enabled) {
      if (ebx7 & (1u << 16)) features.x86_avx512f = true;
      if (ebx7 & (1u << 28)) features.x86_avx512cd = true;
      if (ebx7 & (1u << 31)) features.x86_avx512vl = true;
      if (ecx7 & (1u << 12)) features.x86_avx512bitalg = true;
      if (ecx7 & (1u << 14)) features.x86_avx512vpopcntdq = true;
    }
  }

  return features;
}

// Report whether the CPU is a hybrid design with more than one type of core,
// such as Intel's performance and efficiency cores. The per-core-type counts
// themselves are obtained from the OS by the platform backends.
static inline bool
cpuinfo__cpuid_hybrid(void) {
  uint32_t registers[4];

  cpuinfo__cpuid(0, 0, registers);

  if (registers[0] < 7) return false;

  cpuinfo__cpuid(7, 0, registers);

  return (registers[3] & (1u << 15)) != 0; // Hybrid bit in EDX.
}

// Read the vendor identification string into `dst`, a buffer of at least 13
// bytes. Produces one of e.g. "GenuineIntel" or "AuthenticAMD".
static inline void
cpuinfo__cpuid_vendor(char dst[13]) {
  uint32_t registers[4];

  cpuinfo__cpuid(0, 0, registers);

  uint32_t vendor[3] = {registers[1], registers[3], registers[2]};

  memcpy(dst, vendor, 12);

  dst[12] = '\0';
}

// Read the processor brand string into `dst`, a buffer of at least 49 bytes.
// Returns `true` if the brand string is supported by the CPU.
static inline bool
cpuinfo__cpuid_brand(char dst[49]) {
  uint32_t registers[4];

  cpuinfo__cpuid(0x80000000, 0, registers);

  if (registers[0] < 0x80000004) {
    dst[0] = '\0';

    return false;
  }

  uint32_t brand[12];

  cpuinfo__cpuid(0x80000002, 0, &brand[0]);
  cpuinfo__cpuid(0x80000003, 0, &brand[4]);
  cpuinfo__cpuid(0x80000004, 0, &brand[8]);

  memcpy(dst, brand, 48);

  dst[48] = '\0';

  return true;
}

#endif

#endif // CPUINFO_X86_H
