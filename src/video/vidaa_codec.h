#ifndef MOONLIGHT_VIDAA_CODEC_H
#define MOONLIGHT_VIDAA_CODEC_H

#include <stdint.h>
#include <string.h>

typedef struct {
  uint32_t codec_type;
  uint8_t codec_attributes[20];
} MIInjplayVideoCodecData;

/* MI_VIDEO_CodecParams_t has a larger union than the INJPLAY structure.
 * Firmware DWARF and MI_VIDEO_SetAttr(CODEC_DATA) both specify 48 bytes.
 * HEVC/H.264 use the private-data layout in the first 20 union bytes. */
typedef struct {
  uint32_t codec_type;
  uint8_t codec_attributes[44];
} MIVideoCodecParams;

_Static_assert(sizeof(MIVideoCodecParams) == 48, "MI_VIDEO codec ABI");
_Static_assert(sizeof(MIInjplayVideoCodecData) == 24, "INJPLAY codec ABI");

static inline void vidaa_video_codec_params(
    MIVideoCodecParams* output, const MIInjplayVideoCodecData* input) {
  memset(output, 0, sizeof(*output));
  output->codec_type = input->codec_type;
  memcpy(output->codec_attributes, input->codec_attributes,
         sizeof(input->codec_attributes));
}

#endif
