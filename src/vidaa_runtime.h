#ifndef MOONLIGHT_VIDAA_RUNTIME_H
#define MOONLIGHT_VIDAA_RUNTIME_H

/* OpenSSL 1.1.1 crypto/arm_arch.h: ARMV7_TICK is bit 1. This optional
 * cycle-counter read faults in the retail TV runtime after its startup probe.
 * Keep NEON and every cryptographic accelerator bit. OpenSSL falls back to
 * clock_gettime for additional timer data; OS entropy remains unchanged. */
static inline unsigned int vidaa_openssl_armcap(unsigned int detected) {
  return detected & ~(1u << 1);
}

#endif
