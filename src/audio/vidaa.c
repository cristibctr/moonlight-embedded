#include "audio.h"

#include <stdio.h>

extern int vidaa_hmp_audio_setup(int sample_rate, int channels,
                                 int samples_per_frame);
extern void vidaa_hmp_audio_cleanup(void);
extern void vidaa_hmp_audio_submit(const char* data, int length);

static int vidaa_audio_init(int audio_configuration,
                            POPUS_MULTISTREAM_CONFIGURATION opus_config,
                            void* context, int flags) {
  (void)audio_configuration;
  (void)context;
  (void)flags;

  return vidaa_hmp_audio_setup(opus_config->sampleRate,
                               opus_config->channelCount,
                               opus_config->samplesPerFrame);
}

static void vidaa_audio_cleanup(void) {
  vidaa_hmp_audio_cleanup();
}

static void vidaa_audio_submit(char* data, int length) {
  if (data != NULL && length > 0)
    vidaa_hmp_audio_submit(data, length);
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_vidaa = {
  .init = vidaa_audio_init,
  .cleanup = vidaa_audio_cleanup,
  .decodeAndPlaySample = vidaa_audio_submit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT |
                  CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION,
};
