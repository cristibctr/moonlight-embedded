#ifndef VIDAA_BITSTREAM_H
#define VIDAA_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A legal HEVC filler-data NAL terminates the last VCL NAL without ending
 * the sequence or changing reference pictures. Layer 0, temporal ID 0.
 * This is an opt-in parser-latency experiment, not a proven improvement. */
static inline size_t vidaa_append_hevc_filler(uint8_t *packet, size_t length,
                                             size_t capacity) {
  static const uint8_t filler[] = {0, 0, 0, 1, 0x4c, 1, 0x80};
  if (!packet || !length || length > capacity ||
      capacity - length < sizeof(filler))
    return 0;
  memcpy(packet + length, filler, sizeof(filler));
  return length + sizeof(filler);
}

#endif
