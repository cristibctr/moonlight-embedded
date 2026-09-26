#include <Limelight.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "video.h"
#include "vidaa_codec.h"
#include "vidaa_bitstream.h"
#include <sps.h>

typedef struct {
  uint32_t player_id;
  uint32_t app_type;
  uint32_t data_type;
  uint32_t streaming_type;
} HMPCreateParam;

typedef struct {
  uint32_t data_type;
  uint32_t stream_type;
  uint32_t codec;
  uint32_t drm_type;
  uint32_t video_mode;
} HMPOpenParam;

typedef struct {
  uint32_t player_id;
  uint32_t app_type;
  uint32_t data_type;
} HSCreateParam;

typedef struct {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
  uint32_t flags;
} HMPRectangle;

typedef struct {
  const char* client_name;
  uint32_t reserved[23];
} MISysInitParam;

typedef struct {
  uint8_t reserved;
} MIInjplayInitParams;

typedef struct {
  uint8_t* name;
  uint32_t stream_type;
} MIInjplayOpenParams;

typedef struct {
  uint32_t display_handle;
  uint32_t audio_output_handle;
} MIInjplayStartParams;

typedef struct {
  uint32_t mode;
  uint16_t pcr_pid;
  uint16_t reserved;
} MIInjplaySyncMode;

/* Optional RAM-only settings for controlled A/B tests. No file means baseline. */
typedef struct {
  int sync_mode;
  int min_frame_gap;
  int decode_order;
  int trace_seconds;
  int fd_mask_delay;
  int h264_timing;
  int hevc_filler;
  int reference_invalidation;
  /* Drain the display manager queue when its reported delay (ms) reaches
   * this value. 0 disables. */
  int dms_drain_ms;
  int direct_submit;
} VideoTuning;

static VideoTuning read_video_tuning(void) {
  /* dms_drain_ms defaults to 14: with an empty DMS FIFO the reported delay
   * stays below ~10 ms; each queued frame adds a frame period. */
  VideoTuning tuning = {-1, -1, 0, 0, -1, 0, 0, 1, 14, 0};
  const char* drain_env = getenv("MOONLIGHT_VIDAA_DMS_DRAIN_MS");
  if (drain_env != NULL)
    tuning.dms_drain_ms = atoi(drain_env);
  const char* direct_env = getenv("MOONLIGHT_VIDAA_DIRECT_SUBMIT");
  if (direct_env != NULL)
    tuning.direct_submit = atoi(direct_env) != 0;
  FILE* file = fopen("/tmp/moonlight-video-tuning.conf", "r");
  if (file == NULL)
    return tuning;
  char line[128], key[64], extra;
  int value;
  while (fgets(line, sizeof(line), file) != NULL) {
    if (line[0] == '#' || line[0] == '\n')
      continue;
    if (sscanf(line, "%63[^=]=%d %c", key, &value, &extra) != 2) {
      fprintf(stderr, "VIDAA tuning: ignored invalid line\n");
      continue;
    }
    if (strcmp(key, "sync_mode") == 0 &&
        (value == -1 || value == 0 || value == 6))
      tuning.sync_mode = value;
    else if (strcmp(key, "min_frame_gap") == 0 && value >= -1 && value <= 4)
      tuning.min_frame_gap = value;
    else if (strcmp(key, "decode_order") == 0 && (value == 0 || value == 1))
      tuning.decode_order = value;
    else if (strcmp(key, "trace_seconds") == 0 && value >= 0 && value <= 30)
      tuning.trace_seconds = value;
    else if (strcmp(key, "fd_mask_delay") == 0 && value >= -1 && value <= 4)
      tuning.fd_mask_delay = value;
    else if (strcmp(key, "h264_timing") == 0 && (value == 0 || value == 1))
      tuning.h264_timing = value;
    else if (strcmp(key, "hevc_filler") == 0 && (value == 0 || value == 1))
      tuning.hevc_filler = value;
    else if (strcmp(key, "reference_invalidation") == 0 && (value == 0 || value == 1))
      tuning.reference_invalidation = value;
    else if (strcmp(key, "dms_drain_ms") == 0 && value >= 0 && value <= 200)
      tuning.dms_drain_ms = value;
    else if (strcmp(key, "direct_submit") == 0 && (value == 0 || value == 1))
      tuning.direct_submit = value;
    else
      fprintf(stderr, "VIDAA tuning: ignored unsupported setting %s\n", key);
  }
  fclose(file);
  return tuning;
}

static FILE* trace_video;
static FILE* trace_csv;
static uint64_t trace_deadline;
static size_t trace_bytes;
static int h264_timing_fps;
static bool hevc_filler_enabled;

static uint64_t monotonic_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void close_video_trace(void) {
  if (trace_video != NULL) fclose(trace_video);
  if (trace_csv != NULL) fclose(trace_csv);
  trace_video = trace_csv = NULL;
}

static void record_video_trace(PDECODE_UNIT unit, const void* bytes, size_t length,
                               uint64_t submit_relative, uint64_t submit_absolute,
                               uint64_t complete_relative) {
  if (trace_video == NULL || trace_csv == NULL) return;
  if (submit_absolute >= trace_deadline || trace_bytes + length > 64 * 1024 * 1024) {
    close_video_trace();
    fprintf(stderr, "VIDAA trace: completed %zu bytes\n", trace_bytes);
    return;
  }
  if (fwrite(bytes, 1, length, trace_video) != length) {
    close_video_trace();
    return;
  }
  /* LiGetMicroseconds has a per-process origin and can use MONOTONIC_RAW.
   * Map each short receive/submit interval onto CLOCK_MONOTONIC locally. */
  uint64_t received = submit_absolute - (submit_relative - unit->receiveTimeUs);
  uint64_t queued = submit_absolute - (submit_relative - unit->enqueueTimeUs);
  fprintf(trace_csv, "%d,%d,%llu,%llu,%llu,%llu,%llu,%zu,%zu\n",
          unit->frameNumber, unit->frameType,
          (unsigned long long)received, (unsigned long long)queued,
          (unsigned long long)submit_absolute,
          (unsigned long long)(submit_absolute + complete_relative - submit_relative),
          (unsigned long long)unit->presentationTimeUs, trace_bytes, length);
  trace_bytes += length;
}

typedef struct {
  uint8_t* name;
} MIQueryHandleParams;

typedef struct {
  uint8_t is_input;
  uint8_t reserved[3];
  uint32_t module;
  uint32_t show_type;
} MIDispConnectedConditions;

typedef struct {
  uint32_t show_type;
  uint32_t source_type;
} MIDispConnectInputParams;

typedef struct {
  uint8_t set_min_frame_gap;
  uint8_t set_fd_mask_delay_count;
  uint8_t set_auto_exhaust_es;
  uint8_t set_auto_drop_display_queue;
  uint8_t set_decode_order_display;
  uint8_t set_min_decode_es_level;
  uint8_t set_parser_fifo_threshold;
  uint8_t reserved[5];
  uint32_t min_frame_gap;
  uint32_t auto_exhaust_es;
  uint32_t min_decode_es_level;
  uint32_t auto_drop_display_queue;
  uint32_t fd_mask_delay_count;
  uint32_t parser_fifo_threshold;
} MIVideoLatencyControl;

typedef struct {
  uint8_t enable_video_level_control;
  uint8_t padding0[3];
  uint32_t video_watermark_ms;
  uint32_t video_watermark_bytes;
  uint8_t enable_audio_level_control;
  uint8_t padding1[3];
  uint32_t audio_watermark_ms;
  uint32_t audio_watermark_bytes;
} MIInjectEsBufferLevelControl;

typedef struct {
  uint32_t buffer_size;
} MIInjectQueryBufferParams;

typedef struct {
  uint8_t* buffer_address;
  uint32_t buffer_size;
  uint64_t pts;
  uint8_t end_of_stream;
  uint8_t picture_start;
  uint8_t broken_by_us;
  uint8_t padding[5];
} MIInjectBufferParams;

extern int HMPInit(void);
extern int HMPCreate(HMPCreateParam* param, void** handle);
extern int HMPRegisterCallback(void* handle,
                               void (*callback)(void*, uint32_t, void*),
                               void* user_data);
extern int HMPOpen(void* handle, void* hdr_metadata, void* codec_info,
                   HMPOpenParam param);
extern int HMPPushData(void* handle, uint32_t stream_type, uint32_t codec,
                       uint32_t stream_generation, double pts, double duration,
                       uint32_t length, const unsigned char* data,
                       void* extra1, void* extra2, void* extra3);
extern int HMPStart(void* handle);
extern int HMPDestroy(void* handle);
extern int HMPPause(void* handle);
extern int HMPResume(void* handle);
extern int HMPSetSpeed(void* handle, uint32_t speed);
extern int HMPGetState(void* handle, uint32_t* state);
extern int HMPGetSpeed(void* handle, uint32_t* speed);
extern int HMPGetPts(void* handle, void* pts_info);
extern int HMPGetVideoInfo(void* handle, void* video_info);
extern int HMPSetFirstPresentInfo(void* handle, double delay_ms);
extern int HMPSetDisplayRect(void* handle, HMPRectangle display,
                             HMPRectangle source, uint32_t flags);
extern int HMPSetComponentMuteState(void* handle, uint32_t stream_type,
                                    bool muted);

enum {
  HMP_DATA_AUDIO_VIDEO = 3,
  HMP_DATA_AUDIO = 1,
  HMP_DATA_VIDEO = 2,
  HMP_STREAM_AUDIO = 1,
  HMP_STREAM_VIDEO = 2,
  HMP_STREAMING_MSE = 16,
  HMP_CODEC_AAC = 1,
  HMP_CODEC_MP3 = 2,
  HMP_CODEC_PCM = 3,
  /* HS/HMP API enum. This is not the browser backend's internal Opus value
   * (5). This TV does not implement the HUI Opus decoder, so Opus cannot be
   * the hardware presentation clock. */
  HMP_CODEC_OPUS = 26,
  HMP_CODEC_H264 = 17,
  HMP_CODEC_HEVC = 18,
};

static void* player;
static unsigned char* packet;
static size_t packet_capacity;
static uint32_t codec;
static int source_width;
static int source_height;
static double frame_duration;
static double first_source_pts;
static bool have_first_source_pts;
static bool started;
static bool video_queued;
static bool audio_open;
static bool audio_queued;
static bool audio_header_sent;
static unsigned char* audio_packet;
static size_t audio_packet_capacity;
static double audio_pts;
static double audio_duration;
static double audio_clock_accumulator;
static unsigned int submitted_audio_packets;
static pthread_mutex_t player_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool presentation_resumed;
static bool debug_events;
static unsigned int submitted_frames;
static unsigned int display_apply_attempts;
static const char* presentation_mode;
static int (*mi_sys_init)(void* param);
static int (*mi_aout_init)(void* param);
static int (*mi_audio_init)(void* param);
static int (*mi_pcm_init)(void* param);
static int (*mi_video_init)(void* param);
static int (*mi_disp_init)(void* param);
static int (*mi_disp_get_handle)(const MIQueryHandleParams* params,
                                 uint32_t* handle);
static int (*mi_disp_get_connected_num)(
    uint32_t handle, const MIDispConnectedConditions* conditions,
    uint32_t* count);
static int (*mi_disp_get_connected)(
    uint32_t handle, const MIDispConnectedConditions* conditions,
    uint32_t count, uint32_t* connected_handles);
static int (*mi_disp_disconnect_input)(uint32_t display_handle,
                                       uint32_t input_handle);
static int (*mi_disp_connect_input)(
    uint32_t display_handle, uint32_t input_handle,
    const MIDispConnectInputParams* params);
static int (*mi_disp_get_picture_param)(uint32_t handle, uint32_t type,
                                        const void* input, void* output);
static int (*mi_disp_set_picture_param)(uint32_t handle, uint32_t type,
                                        const void* input);
static int (*mi_disp_get_mute_status)(uint32_t handle, uint32_t flag,
                                      uint8_t* muted);
static int (*mi_disp_set_mute)(uint32_t handle, uint32_t flag);
static int (*mi_disp_set_unmute)(uint32_t handle, uint32_t flag);
static int (*mi_video_get_handle)(const MIQueryHandleParams* params,
                                  uint32_t* handle);
static int (*mi_video_set_attr)(uint32_t handle, uint32_t attr_type,
                                const void* params);
static int (*mi_video_get_attr)(uint32_t handle, uint32_t attr_type,
                                const void* input, void* output);
static int (*mi_injplay_init)(const MIInjplayInitParams* params);
static int (*mi_injplay_deinit)(void);
static int (*mi_injplay_open)(const MIInjplayOpenParams* params,
                              uint32_t* handle);
static int (*mi_injplay_close)(uint32_t handle);
static int (*mi_injplay_set_video_codec)(
    uint32_t handle, MIInjplayVideoCodecData* codec_data);
static int (*mi_injplay_start)(uint32_t handle,
                               const MIInjplayStartParams* params);
static int (*mi_injplay_stop)(uint32_t handle);
static int (*mi_injplay_set_attr)(uint32_t handle, uint32_t attr_type,
                                  const void* params);
static int (*mi_injplay_get_attr)(uint32_t handle, uint32_t attr_type,
                                  const void* input, void* output);
static int (*mi_injplay_set_debug_level)(uint32_t debug_level);
static int (*mi_injplay_query_buffer)(
    uint32_t handle, MIInjectQueryBufferParams* query,
    MIInjectBufferParams* buffer);
static int (*mi_injplay_write_complete)(uint32_t handle,
                                        MIInjectBufferParams* buffer);
static uint32_t injplay_handle;
static uint32_t injplay_decoder_handle;
static uint32_t injplay_display_handle;
static uint8_t injplay_original_game_mode;
static bool injplay_game_mode_changed;
static uint8_t injplay_original_window_mute;
static bool injplay_window_mute_saved;
static bool injplay_window_unmuted;
static bool injplay_window_checked;
static uint32_t displaced_display_inputs[16];
static uint32_t displaced_display_input_count;
static bool direct_injplay;
/* Display manager (DMS) queue drain. MI_DISP_GetAttr case 6 word 4 is the
 * DMS video delay in ms; it grows by one frame period for every frame that
 * waits in the DMS FIFO. The FIFO never drains by itself at equal in/out
 * rates, so a startup burst or one late frame adds permanent delay. */
static int (*mi_disp_get_attr)(uint32_t handle, uint32_t type,
                               const void* input, void* output);
static int dms_drain_ms;
/* 3 (default): drop one displayed frame with a short 2x speed burst.
 * 0: drop frames until the next IDR (fallback). */
static int dms_drain_mode;
/* Mode 3: drop exactly one displayed frame by running the decoder at 2x
 * (E_MI_VIDEO_FAST_SPEED_2X) until its drop counter advances. Every frame
 * is still decoded, so references stay intact. */
static int (*mi_video_set_speed)(uint32_t handle, uint32_t speed);
static bool speed_drop_active;
static unsigned int speed_drop_start_frame;
static uint32_t speed_drop_start_count;

static unsigned int dms_over_count;
static unsigned int dms_last_drain_frame;
static unsigned int dms_drains;
static uint32_t dms_delay_ms;
static int (*hs_get_state)(int handle, uint32_t* state);

static bool owns_display_input(void) {
  const MIDispConnectedConditions conditions = {
      .is_input = 1, .module = 199, .show_type = 1};
  uint32_t count = 0, input = 0;
  return injplay_display_handle != 0 && injplay_decoder_handle != 0 &&
      mi_disp_get_connected_num != NULL && mi_disp_get_connected != NULL &&
      mi_disp_get_connected_num(injplay_display_handle, &conditions, &count) == 0 &&
      count == 1 &&
      mi_disp_get_connected(injplay_display_handle, &conditions, 1, &input) == 0 &&
      input == injplay_decoder_handle;
}

static void show_ready_window(void) {
  if (injplay_window_checked || !injplay_window_mute_saved ||
      mi_disp_set_unmute == NULL || mi_video_get_attr == NULL)
    return;
  uint8_t ready = 0, muted = 0;
  if (mi_video_get_attr(injplay_decoder_handle, 0x105, NULL, &ready) != 0 ||
      ready != 1 || !owns_display_input() ||
      mi_disp_get_mute_status(injplay_display_handle, 4, &muted) != 0)
    return;
  if (muted == 0) {
    injplay_window_checked = true;
    return;
  }
  /* INJPLAY leaves SET_WINDOW set after the valid frame/rectangle arrives.
   * Clear only this layout flag; never touch USER, VIDEO, rating or CA flags. */
  int result = mi_disp_set_unmute(injplay_display_handle, 4);
  if (result == 0) injplay_window_unmuted = true;
  uint8_t actual = 255;
  int verify = mi_disp_get_mute_status(injplay_display_handle, 4, &actual);
  fprintf(stderr, "VIDAA INJPLAY: ready window unmute=%d verify=%d muted=%u\n",
          result, verify, actual);
  injplay_window_checked = result == 0 && verify == 0 && actual == 0;
}

static void restore_window_mute(void) {
  if (injplay_window_unmuted && injplay_original_window_mute == 1 &&
      mi_disp_set_mute != NULL && owns_display_input()) {
    int result = mi_disp_set_mute(injplay_display_handle, 4);
    uint8_t actual = 255;
    int verify = mi_disp_get_mute_status(injplay_display_handle, 4, &actual);
    fprintf(stderr, "VIDAA INJPLAY: restore window mute=%d verify=%d muted=%u\n",
            result, verify, actual);
  }
  injplay_window_unmuted = false;
}

static void restore_game_mode(void) {
  if (injplay_game_mode_changed && mi_disp_set_picture_param != NULL) {
    /* Get writes one byte; use a zero-extended 32-bit Set payload,
     * as used by the firmware's HUI_VOutSetGameMode caller. */
    uint32_t original = injplay_original_game_mode;
    int result = mi_disp_set_picture_param(
        injplay_display_handle, 9, &original);
    fprintf(stderr, "VIDAA INJPLAY: restore game mode=%u result=%d\n",
            injplay_original_game_mode, result);
    /* This setting is applied by the live display pipeline. Wait briefly for
     * it before destroying that pipeline; Set after Stop can be a no-op. */
    if (result == 0 && mi_disp_get_picture_param != NULL) {
      uint8_t actual = 255;
      int verify = -1;
      for (int attempt = 0; attempt < 10; attempt++) {
        usleep(20000);
        verify = mi_disp_get_picture_param(injplay_display_handle, 9, NULL, &actual);
        if (verify == 0 && actual == injplay_original_game_mode)
          break;
      }
      fprintf(stderr, "VIDAA INJPLAY: restore game mode verify=%d value=%u\n",
              verify, actual);
    }
    injplay_game_mode_changed = false;
  }
}

/* The DMS "flip trigger" programs MVOP as soon as a frame is flipped into an
 * empty display queue, instead of waiting for the next vsync ISR. It is a
 * global driver debug switch (default off after boot), so enable it only
 * while streaming. */
static bool dms_flip_trigger_set;

static bool write_dms_command(const char* command) {
  FILE* file = fopen("/proc/utopia_mdb/dms", "w");
  if (file == NULL)
    return false;
  bool ok = fputs(command, file) >= 0;
  ok = fclose(file) == 0 && ok;
  return ok;
}

static void set_dms_flip_trigger(bool enable) {
  if (!enable && !dms_flip_trigger_set)
    return;
  bool ok = write_dms_command(enable ? "FlipTrigEvent ON\n" : "FlipTrigEvent OFF\n");
  dms_flip_trigger_set = enable && ok;
  fprintf(stderr, "VIDAA DMS flip trigger %s: %s\n", enable ? "on" : "off",
          ok ? "ok" : "failed");
}

static void restore_displaced_display_inputs(void) {
  if (injplay_display_handle == 0 || mi_disp_connect_input == NULL)
    return;
  MIDispConnectInputParams params = {
      .show_type = 1,
      .source_type = 0,
  };
  for (uint32_t i = 0; i < displaced_display_input_count; i++) {
    int result = mi_disp_connect_input(
        injplay_display_handle, displaced_display_inputs[i], &params);
    fprintf(stderr, "VIDAA INJPLAY: restore display input 0x%08x=%d\n",
            displaced_display_inputs[i], result);
  }
  displaced_display_input_count = 0;
}
static int (*hs_get_start_done)(int handle, uint32_t stream_type, bool* done);
static int (*hs_get_video_info)(int handle, void* video_info);
static int (*hs_get_pts)(int handle, void* pts_info);
static int (*hs_set_first_present_info)(int handle, long long delay_ms);
static int (*hs_init)(void);
static int (*hs_create)(HSCreateParam* param);
static int (*hs_open)(int handle, HMPOpenParam* param, void* codec_info);
static int (*hs_push_data)(int handle, uint32_t stream_type, uint32_t codec,
                           uint32_t stream_generation, double pts,
                           double duration, uint32_t length,
                           const unsigned char* data, void* extra1,
                           void* extra2, void* extra3);
static int (*hs_start)(int handle, uint32_t stream_type);
static int (*hs_play)(int handle);
static int (*hs_close)(int handle);
static int (*hs_deinit)(void);
static int (*hs_set_speed)(int handle, uint32_t speed);
static int (*hs_get_speed)(int handle, uint32_t* speed);
static int (*hs_set_display_rect)(int handle, HMPRectangle display,
                                  HMPRectangle source, uint32_t flags);
static int (*hs_set_component_mute_state)(int handle, uint32_t stream_type,
                                          bool muted);
static bool direct_hs_av;
static bool hs_streams_started;
static unsigned int start_attempts;
static unsigned int next_start_audio_packet;
static int (*hui_write_data)(uint32_t handle, const void* data,
                             uint32_t length, uint64_t pts);
static int (*hui_get_pts)(uint32_t handle, uint64_t* pts);
static int (*hui_resume)(uint32_t handle);
static int (*hui_flush)(uint32_t handle);
static int (*hui_sync_adjust)(uint32_t handle, int64_t adjustment_ms);
static int (*hui_set_interdevice_sync)(uint32_t handle, bool enabled);
static bool hui_playing;
static bool sync_adjusted;
static int64_t requested_sync_adjust;
static bool live_rebased;
static double live_pts_shift;
static bool live_flush_started;
static volatile bool live_flush_ready;
static bool live_catchup_started;
static bool live_catchup_done;

#define VIDAA_VIDEO_INJECTOR_HANDLE UINT32_C(0xffffff00)

static bool audio_video_mode(void) {
  return presentation_mode != NULL && strcmp(presentation_mode, "av") == 0;
}

static bool injplay_mode(void) {
  return presentation_mode != NULL &&
         strcmp(presentation_mode, "injplay") == 0;
}

static int start_player_locked(void) {
  if (started)
    return 0;
  if (!video_queued || (audio_video_mode() && (!audio_open || !audio_queued)))
    return 0;
  if (audio_video_mode() && start_attempts != 0 &&
      submitted_audio_packets < next_start_audio_packet)
    return 0;

  start_attempts++;
  next_start_audio_packet = submitted_audio_packets + 8;

  int result;
  if (direct_hs_av) {
    if (!hs_streams_started) {
      int audio_result = hs_start(getpid(), HMP_STREAM_AUDIO);
      int video_result = hs_start(getpid(), HMP_STREAM_VIDEO);
      int play_result = hs_play(getpid());
      hs_streams_started = audio_result == 0 && video_result == 0 &&
                           play_result == 0;
      result = audio_result != 0 ? audio_result
                                 : (video_result != 0 ? video_result
                                                      : play_result);
      fprintf(stderr,
              "VIDAA: lower player start: audio=%d video=%d play=%d\n",
              audio_result, video_result, play_result);
    } else {
      result = 0;
    }
  } else {
    result = HMPStart(player);
  }
  if (result != 0) {
    fprintf(stderr, "VIDAA: player start failed: %d\n", result);
    return result;
  }

  if (audio_video_mode() && hs_get_start_done != NULL) {
    bool audio_done = false;
    bool video_done = false;
    int audio_done_result =
        hs_get_start_done(getpid(), HMP_STREAM_AUDIO, &audio_done);
    int video_done_result =
        hs_get_start_done(getpid(), HMP_STREAM_VIDEO, &video_done);
    if (audio_done_result == 0 && video_done_result == 0 &&
        (!audio_done || !video_done)) {
      if (start_attempts <= 8 || (start_attempts % 10) == 0)
        fprintf(stderr,
                "VIDAA: start pending (attempt=%u audio=%u video=%u)\n",
                start_attempts, audio_done ? 1u : 0u,
                video_done ? 1u : 0u);
      return 0;
    }
  }

  started = true;
  if (!direct_hs_av) {
    HMPRectangle display = {0, 0, 3840, 2160, 0};
    HMPRectangle source = {0, 0, source_width, source_height, 0};
    result = HMPSetDisplayRect(player, display, source, 0);
    if (result != 0)
      fprintf(stderr, "VIDAA: full-screen rectangle kept at TV default: %d\n",
              result);

    result = HMPSetComponentMuteState(player, HMP_STREAM_VIDEO, false);
    if (result != 0)
      fprintf(stderr, "VIDAA: video unmute request was not required: %d\n",
              result);
  }

  int audio_mute_result =
      direct_hs_av && hs_set_component_mute_state != NULL
          ? hs_set_component_mute_state(getpid(), HMP_STREAM_AUDIO, true)
          : HMPSetComponentMuteState(player, HMP_STREAM_AUDIO, true);
  if (audio_mute_result != 0)
    fprintf(stderr, "VIDAA: silent clock mute request failed: %d\n",
            audio_mute_result);

  fprintf(stderr, "VIDAA: playback started%s\n",
          audio_video_mode() ? " with silent MP3 clock" : "");
  return 0;
}

static void hmp_event(void* handle, uint32_t event, void* user_data) {
  (void)handle;
  (void)user_data;
  if (event == 0x11)
    hui_playing = true;
  if (event == 0x14 && live_flush_started)
    live_flush_ready = true;
  if (debug_events)
    fprintf(stderr, "VIDAA HMP event: 0x%x\n", event);
}

static int create_player_locked(void) {
  if (injplay_mode()) {
    mi_injplay_init = (int (*)(const MIInjplayInitParams*))dlsym(
        RTLD_DEFAULT, "MI_INJPLAY_Init");
    mi_injplay_deinit =
        (int (*)(void))dlsym(RTLD_DEFAULT, "MI_INJPLAY_DeInit");
    mi_injplay_open =
        (int (*)(const MIInjplayOpenParams*, uint32_t*))dlsym(
            RTLD_DEFAULT, "MI_INJPLAY_Open");
    mi_injplay_close =
        (int (*)(uint32_t))dlsym(RTLD_DEFAULT, "MI_INJPLAY_Close");
    mi_injplay_set_video_codec =
        (int (*)(uint32_t, MIInjplayVideoCodecData*))dlsym(
            RTLD_DEFAULT, "MI_INJPLAY_SetVideoCodecData");
    mi_injplay_start =
        (int (*)(uint32_t, const MIInjplayStartParams*))dlsym(
            RTLD_DEFAULT, "MI_INJPLAY_Start");
    mi_injplay_stop =
        (int (*)(uint32_t))dlsym(RTLD_DEFAULT, "MI_INJPLAY_Stop");
    mi_injplay_set_attr =
        (int (*)(uint32_t, uint32_t, const void*))dlsym(
            RTLD_DEFAULT, "MI_INJPLAY_SetAttr");
    mi_injplay_set_debug_level =
        (int (*)(uint32_t))dlsym(RTLD_DEFAULT,
                                 "MI_INJPLAY_SetDebugLevel");
    mi_injplay_query_buffer =
        (int (*)(uint32_t, MIInjectQueryBufferParams*,
                 MIInjectBufferParams*))dlsym(
            RTLD_DEFAULT, "MI_INJPLAY_QueryFreeBuffer");
    mi_injplay_write_complete =
        (int (*)(uint32_t, MIInjectBufferParams*))dlsym(
            RTLD_DEFAULT, "MI_INJPLAY_WriteBufferComplete");
    mi_disp_get_handle =
        (int (*)(const MIQueryHandleParams*, uint32_t*))dlsym(
            RTLD_DEFAULT, "MI_DISP_GetHandle");
    mi_disp_get_connected_num =
        (int (*)(uint32_t, const MIDispConnectedConditions*, uint32_t*))
            dlsym(RTLD_DEFAULT, "MI_DISP_GetConnectedNum");
    mi_disp_get_connected =
        (int (*)(uint32_t, const MIDispConnectedConditions*, uint32_t,
                 uint32_t*))dlsym(RTLD_DEFAULT, "MI_DISP_GetConnected");
    mi_disp_disconnect_input =
        (int (*)(uint32_t, uint32_t))dlsym(
            RTLD_DEFAULT, "MI_DISP_DisconnectInput");
    mi_disp_connect_input =
        (int (*)(uint32_t, uint32_t, const MIDispConnectInputParams*))dlsym(
            RTLD_DEFAULT, "MI_DISP_ConnectInput");
    mi_disp_get_mute_status =
        (int (*)(uint32_t, uint32_t, uint8_t*))dlsym(
            RTLD_DEFAULT, "MI_DISP_GetMuteStatus");
    mi_disp_set_mute =
        (int (*)(uint32_t, uint32_t))dlsym(RTLD_DEFAULT, "MI_DISP_SetMute");
    mi_disp_set_unmute =
        (int (*)(uint32_t, uint32_t))dlsym(RTLD_DEFAULT, "MI_DISP_SetUnMute");
    mi_video_get_handle =
        (int (*)(const MIQueryHandleParams*, uint32_t*))dlsym(
            RTLD_DEFAULT, "MI_VIDEO_GetHandle");
    mi_video_set_attr =
        (int (*)(uint32_t, uint32_t, const void*))dlsym(
            RTLD_DEFAULT, "MI_VIDEO_SetAttr");
    mi_video_get_attr =
        (int (*)(uint32_t, uint32_t, const void*, void*))dlsym(
            RTLD_DEFAULT, "MI_VIDEO_GetAttr");
    if (mi_injplay_init == NULL || mi_injplay_deinit == NULL ||
        mi_injplay_open == NULL || mi_injplay_close == NULL ||
        mi_injplay_set_video_codec == NULL || mi_injplay_start == NULL ||
        mi_injplay_stop == NULL || mi_injplay_set_attr == NULL ||
        mi_injplay_query_buffer == NULL || mi_disp_get_handle == NULL ||
        mi_disp_get_connected_num == NULL || mi_disp_get_connected == NULL ||
        mi_disp_disconnect_input == NULL || mi_disp_connect_input == NULL ||
        mi_video_get_handle == NULL || mi_video_set_attr == NULL ||
        mi_video_get_attr == NULL ||
        mi_injplay_write_complete == NULL) {
      fprintf(stderr, "VIDAA: MI_INJPLAY symbols are missing\n");
      return -4;
    }

    MIInjplayInitParams init = {0};
    int result = mi_injplay_init(&init);
    fprintf(stderr, "VIDAA INJPLAY: init=%d\n", result);
    if (result != 0)
      return -4;
    mi_injplay_get_attr =
        (int (*)(uint32_t, uint32_t, const void*, void*))dlsym(
            RTLD_DEFAULT, "MI_INJPLAY_GetAttr");
    if (mi_injplay_set_debug_level != NULL) {
      /* Keep sparse client metrics separate from server per-packet tracing.
       * The server debug level persists beyond this client connection. */
      int debug_result = mi_injplay_set_debug_level(
          getenv("MOONLIGHT_VIDAA_VENDOR_DEBUG") != NULL ? 255 : 0);
      fprintf(stderr, "VIDAA INJPLAY: debug level=%d\n", debug_result);
    }

    if (getenv("MOONLIGHT_VIDAA_INJPLAY_RECOVER") != NULL) {
      /* The first API probe opened this exact Moonlight-owned handle and
       * exited before it could close it. Recover that one known slot only. */
      int recover_result = mi_injplay_close(UINT32_C(0x59000000));
      fprintf(stderr, "VIDAA INJPLAY: recover handle 0x59000000=%d\n",
              recover_result);
      usleep(250000);
    }

    char injplay_name[32];
    snprintf(injplay_name, sizeof(injplay_name), "moonlight-%d", getpid());
    MIInjplayOpenParams open = {
        .name = (uint8_t*)injplay_name,
        .stream_type = 3, /* E_MI_INJECT_STREAM_VES */
    };
    result = mi_injplay_open(&open, &injplay_handle);
    fprintf(stderr, "VIDAA INJPLAY: open=%d handle=0x%08x\n", result,
            injplay_handle);
    if (result != 0) {
      mi_injplay_deinit();
      return -4;
    }

    MIInjplayVideoCodecData video_codec;
    memset(&video_codec, 0, sizeof(video_codec));
    video_codec.codec_type =
        codec == HMP_CODEC_HEVC ? 0x10 : 0x0b;
    uint16_t coded_width = (uint16_t)source_width;
    uint16_t coded_height = (uint16_t)source_height;
    uint32_t coded_frame_rate =
        frame_duration > 0.0 ? (uint32_t)(1.0 / frame_duration + 0.5) : 60;
    uint32_t coded_frame_rate_base = 1;
    memcpy(video_codec.codec_attributes, &coded_width, sizeof(coded_width));
    memcpy(video_codec.codec_attributes + 2, &coded_height,
           sizeof(coded_height));
    memcpy(video_codec.codec_attributes + 4, &coded_frame_rate,
           sizeof(coded_frame_rate));
    memcpy(video_codec.codec_attributes + 8, &coded_frame_rate_base,
           sizeof(coded_frame_rate_base));
    result = mi_injplay_set_video_codec(injplay_handle, &video_codec);
    fprintf(stderr, "VIDAA INJPLAY: set codec=%d\n", result);
    if (result != 0) {
      mi_injplay_close(injplay_handle);
      mi_injplay_deinit();
      return -6;
    }

    /* This firmware connects the video pipeline before INJPLAY starts the
     * decoder. Publish the complete codec parameters before connection.
     * Passing only the codec enum makes the driver read 44 unrelated bytes. */
    uint32_t decoder_handle = 0;
    MIQueryHandleParams decoder_query = {
        .name = (uint8_t*)injplay_name,
    };
    result = mi_video_get_handle(&decoder_query, &decoder_handle);
    fprintf(stderr, "VIDAA INJPLAY: decoder=%d handle=0x%08x\n", result,
            decoder_handle);
    if (result != 0) {
      mi_injplay_close(injplay_handle);
      mi_injplay_deinit();
      return -6;
    }
    MIVideoCodecParams decoder_codec;
    vidaa_video_codec_params(&decoder_codec, &video_codec);
    result = mi_video_set_attr(decoder_handle, 1, &decoder_codec);
    fprintf(stderr, "VIDAA INJPLAY: decoder codec attr=%d\n", result);
    if (result != 0) {
      mi_injplay_close(injplay_handle);
      mi_injplay_deinit();
      return -6;
    }
    uint32_t verified_codec = 0;
    int verify_result =
        mi_video_get_attr(decoder_handle, 0x108, NULL, &verified_codec);
    fprintf(stderr,
            "VIDAA INJPLAY: decoder codec verify=%d codec=0x%x\n",
            verify_result, verified_codec);
    if (verify_result != 0 || verified_codec != video_codec.codec_type) {
      mi_injplay_close(injplay_handle);
      mi_injplay_deinit();
      return -6;
    }
    injplay_decoder_handle = decoder_handle;

    uint32_t timestamp_type = 1; /* E_MI_INJPLAY_TIMESTAMP_PTS */
    result = mi_injplay_set_attr(injplay_handle, 9, &timestamp_type);
    fprintf(stderr, "VIDAA INJPLAY: timestamp type PTS=%d\n", result);
    if (result != 0) {
      mi_injplay_close(injplay_handle);
      mi_injplay_deinit();
      return -6;
    }

    VideoTuning tuning = read_video_tuning();
    h264_timing_fps = tuning.h264_timing && codec == HMP_CODEC_H264
        ? (int)(1.0 / frame_duration + 0.5) : 0;
    hevc_filler_enabled = tuning.hevc_filler && codec == HMP_CODEC_HEVC;
    if (hevc_filler_enabled)
      fprintf(stderr, "VIDAA tuning: HEVC filler boundary enabled\n");
    fprintf(stderr, "VIDAA tuning: sync_mode=%d min_frame_gap=%d decode_order=%d\n",
            tuning.sync_mode, tuning.min_frame_gap, tuning.decode_order);
    dms_drain_ms = tuning.dms_drain_ms;
    const char* drain_mode_env = getenv("MOONLIGHT_VIDAA_DMS_DRAIN_MODE");
    dms_drain_mode = drain_mode_env != NULL ? atoi(drain_mode_env) : 3;
    mi_video_set_speed = (int (*)(uint32_t, uint32_t))dlsym(
        RTLD_DEFAULT, "MI_VIDEO_SetSpeed");
    speed_drop_active = false;

    dms_over_count = 0;
    dms_last_drain_frame = 0;
    dms_drains = 0;
    dms_delay_ms = 0;
    mi_disp_get_attr = (int (*)(uint32_t, uint32_t, const void*, void*))dlsym(
        RTLD_DEFAULT, "MI_DISP_GetAttr");
    fprintf(stderr, "VIDAA tuning: dms_drain_ms=%d direct_submit=%d\n",
            dms_drain_ms, tuning.direct_submit);
    if (tuning.sync_mode >= 0) {
      MIInjplaySyncMode sync = {.mode = (uint32_t)tuning.sync_mode,
                               .pcr_pid = 0x1fff};
      result = mi_injplay_set_attr(injplay_handle, 3, &sync);
      fprintf(stderr, "VIDAA INJPLAY: sync mode %d=%d\n", tuning.sync_mode, result);
      if (result != 0) {
        mi_injplay_close(injplay_handle);
        mi_injplay_deinit();
        return -6;
      }
    }
    MIVideoLatencyControl latency;
    memset(&latency, 0, sizeof(latency));
    latency.set_auto_exhaust_es = 1;
    latency.set_auto_drop_display_queue = 1;
    latency.set_min_decode_es_level = 1;
    latency.auto_exhaust_es = 1;
    latency.auto_drop_display_queue = 1;
    latency.min_decode_es_level = 1;
    if (tuning.min_frame_gap >= 0) {
      latency.set_min_frame_gap = 1;
      latency.min_frame_gap = (uint32_t)tuning.min_frame_gap;
    }
    latency.set_decode_order_display = (uint8_t)tuning.decode_order;
    if (tuning.fd_mask_delay >= 0) {
      latency.set_fd_mask_delay_count = 1;
      latency.fd_mask_delay_count = (uint32_t)tuning.fd_mask_delay;
      fprintf(stderr, "VIDAA tuning: fd_mask_delay=%d\n", tuning.fd_mask_delay);
    }
    result = mi_injplay_set_attr(injplay_handle, 2, &latency);
    fprintf(stderr, "VIDAA INJPLAY: latency control=%d\n", result);
    if (result != 0) {
      mi_injplay_close(injplay_handle);
      mi_injplay_deinit();
      return -6;
    }

    MIInjectEsBufferLevelControl level;
    memset(&level, 0, sizeof(level));
    level.enable_video_level_control = 1;
    level.video_watermark_ms = 1;
    result = mi_injplay_set_attr(injplay_handle, 8, &level);
    fprintf(stderr, "VIDAA INJPLAY: ES watermark=%d\n", result);
    /* The VES injector on this firmware can reject the optional watermark
     * attribute. The VDEC latency control above remains active. */

    uint32_t display_handle = 0;
    const char* display_name = getenv("MOONLIGHT_VIDAA_DISPLAY");
    if (display_name == NULL || display_name[0] == '\0')
      display_name = "MI_DISP_HD0";
    MIQueryHandleParams display_query = {
        .name = (uint8_t*)display_name,
    };
    result = mi_disp_get_handle(&display_query, &display_handle);
    fprintf(stderr,
            "VIDAA INJPLAY: display=%s result=%d handle=0x%08x\n",
            display_name, result, display_handle);
    if (result != 0) {
      mi_injplay_close(injplay_handle);
      mi_injplay_deinit();
      return -6;
    }

    if (mi_disp_get_mute_status != NULL && mi_disp_set_mute != NULL &&
        mi_disp_set_unmute != NULL) {
      int mute_result = mi_disp_get_mute_status(
          display_handle, 4, &injplay_original_window_mute);
      injplay_window_mute_saved = mute_result == 0 &&
          injplay_original_window_mute <= 1;
      fprintf(stderr, "VIDAA INJPLAY: saved window mute=%u result=%d\n",
              injplay_original_window_mute, mute_result);
    }
    MIDispConnectedConditions connected_conditions = {
        .is_input = 1,
        .module = 199, /* E_MI_MODULE_ID_VIDEO */
        .show_type = 1, /* on-screen */
    };
    uint32_t connected_count = 0;
    int connected_result = mi_disp_get_connected_num(
        display_handle, &connected_conditions, &connected_count);
    fprintf(stderr, "VIDAA INJPLAY: display inputs result=%d count=%u\n",
            connected_result, connected_count);
    if (connected_result == 0 && connected_count != 0) {
      uint32_t connected_handles[16] = {0};
      uint32_t query_count = connected_count;
      if (query_count > 16)
        query_count = 16;
      connected_result = mi_disp_get_connected(
          display_handle, &connected_conditions, query_count,
          connected_handles);
      fprintf(stderr, "VIDAA INJPLAY: display input handles result=%d",
              connected_result);
      for (uint32_t i = 0; i < query_count; i++)
        fprintf(stderr, " 0x%08x", connected_handles[i]);
      fprintf(stderr, "\n");
      if (connected_result == 0) {
        for (uint32_t i = 0; i < query_count; i++) {
          uint32_t input_handle = connected_handles[i];
          if ((input_handle & UINT32_C(0xff000000)) !=
                  UINT32_C(0xc7000000) ||
              input_handle == decoder_handle)
            continue;
          int disconnect_result =
              mi_disp_disconnect_input(display_handle, input_handle);
          fprintf(stderr,
                  "VIDAA INJPLAY: disconnect old video input 0x%08x=%d\n",
                  input_handle, disconnect_result);
          if (disconnect_result == 0 && displaced_display_input_count < 16)
            displaced_display_inputs[displaced_display_input_count++] =
                input_handle;
        }
      }
    }

    injplay_display_handle = display_handle;
    MIInjplayStartParams start = {
        .display_handle = display_handle,
        .audio_output_handle = UINT32_C(0x17000000),
    };
    result = mi_injplay_start(injplay_handle, &start);
    fprintf(stderr, "VIDAA INJPLAY: start=%d\n", result);
    if (result != 0) {
      mi_injplay_close(injplay_handle);
      mi_injplay_deinit();
      restore_displaced_display_inputs();
      return -6;
    }

    direct_injplay = true;
    if (getenv("MOONLIGHT_VIDAA_FLIP_TRIGGER") == NULL ||
        strcmp(getenv("MOONLIGHT_VIDAA_FLIP_TRIGGER"), "0") != 0)
      set_dms_flip_trigger(true);
    mi_disp_get_picture_param =
        (int (*)(uint32_t, uint32_t, const void*, void*))dlsym(
            RTLD_DEFAULT, "MI_DISP_GetPictureParam");
    mi_disp_set_picture_param =
        (int (*)(uint32_t, uint32_t, const void*))dlsym(
            RTLD_DEFAULT, "MI_DISP_SetPictureParam");
    if (mi_disp_get_picture_param != NULL && mi_disp_set_picture_param != NULL) {
      /* Get writes one byte; Set uses the vendor's 32-bit payload.
       * Preserve the shared display's original setting for stream cleanup. */
      int get_result = mi_disp_get_picture_param(
          display_handle, 9, NULL, &injplay_original_game_mode);
      const char* game_mode = getenv("MOONLIGHT_VIDAA_GAME_MODE");
      fprintf(stderr, "VIDAA INJPLAY: game mode get=%d original=%u\n",
              get_result, injplay_original_game_mode);
      if (get_result == 0 && injplay_original_game_mode == 0 &&
          (game_mode == NULL || strcmp(game_mode, "0") != 0)) {
        uint32_t enabled = 1;
        int set_result = mi_disp_set_picture_param(display_handle, 9, &enabled);
        injplay_game_mode_changed = set_result == 0;
        uint8_t verified = 0;
        int verify_result = mi_disp_get_picture_param(
            display_handle, 9, NULL, &verified);
        fprintf(stderr,
                "VIDAA INJPLAY: game mode enable=%d verify=%d value=%u\n",
                set_result, verify_result, verified);
      }
    }
    started = true;
    player = (void*)(uintptr_t)1;
    if (tuning.trace_seconds > 0) {
      trace_video = fopen("/tmp/moonlight-video-trace.bin", "wb");
      trace_csv = fopen("/tmp/moonlight-video-trace.csv", "w");
      if (trace_video != NULL && trace_csv != NULL) {
        setvbuf(trace_video, NULL, _IOFBF, 65536);
        fprintf(trace_csv, "frame_number,frame_type,receive_us,enqueue_us,submit_us,complete_us,pts_us,offset,length\n");
        trace_deadline = monotonic_us() + (uint64_t)tuning.trace_seconds * 1000000;
        trace_bytes = 0;
        fprintf(stderr, "VIDAA trace: enabled for %d seconds\n", tuning.trace_seconds);
      } else close_video_trace();
    }
    fprintf(stderr, "VIDAA: direct low-latency INJPLAY decoder ready\n");
    return 0;
  }

  if (audio_video_mode() && getenv("MOONLIGHT_VIDAA_LOWER") != NULL) {
    hs_init = (int (*)(void))dlsym(RTLD_DEFAULT, "HS_EsplayerInit");
    hs_create = (int (*)(HSCreateParam*))dlsym(
        RTLD_DEFAULT, "HS_EsplayerCreate");
    hs_open = (int (*)(int, HMPOpenParam*, void*))dlsym(
        RTLD_DEFAULT, "HS_EsplayerOpen");
    hs_push_data =
        (int (*)(int, uint32_t, uint32_t, uint32_t, double, double,
                 uint32_t, const unsigned char*, void*, void*, void*))dlsym(
            RTLD_DEFAULT, "HS_EsplayerPushData");
    hs_start = (int (*)(int, uint32_t))dlsym(
        RTLD_DEFAULT, "HS_EsplayerStart");
    hs_play = (int (*)(int))dlsym(RTLD_DEFAULT, "HS_EsplayerPlay");
    hs_close = (int (*)(int))dlsym(RTLD_DEFAULT, "HS_EsplayerClose");
    hs_deinit = (int (*)(void))dlsym(RTLD_DEFAULT, "HS_EsplayerDeInit");
    hs_set_speed = (int (*)(int, uint32_t))dlsym(
        RTLD_DEFAULT, "HS_EsplayerSetSpeed");
    hs_get_speed = (int (*)(int, uint32_t*))dlsym(
        RTLD_DEFAULT, "HS_EsplayerGetSpeed");
    hs_set_display_rect =
        (int (*)(int, HMPRectangle, HMPRectangle, uint32_t))dlsym(
            RTLD_DEFAULT, "HS_EsplayerSetDisplayRect");
    hs_set_component_mute_state =
        (int (*)(int, uint32_t, bool))dlsym(
            RTLD_DEFAULT, "HS_EsplayerSetComponentMuteState");
    hs_get_state = (int (*)(int, uint32_t*))dlsym(
        RTLD_DEFAULT, "HS_EsplayerGetState");
    hs_get_start_done =
        (int (*)(int, uint32_t, bool*))dlsym(
            RTLD_DEFAULT, "HS_EsplayerGetStartDoneState");
    hs_get_video_info = (int (*)(int, void*))dlsym(
        RTLD_DEFAULT, "HS_EsplayerGetVideoInfo");
    hs_get_pts = (int (*)(int, void*))dlsym(
        RTLD_DEFAULT, "HS_EsplayerGetPts");
    if (hs_init == NULL || hs_create == NULL || hs_open == NULL ||
        hs_push_data == NULL ||
        hs_start == NULL || hs_play == NULL || hs_set_speed == NULL) {
      fprintf(stderr, "VIDAA: lower AV player symbols are missing\n");
      return -4;
    }

    int result = hs_init();
    if (result != 0) {
      fprintf(stderr, "VIDAA: HS_EsplayerInit failed: %d\n", result);
      return -4;
    }

    const int player_id = getpid();
    HSCreateParam create = {(uint32_t)player_id, 0, HMP_DATA_AUDIO_VIDEO};
    fprintf(stderr, "VIDAA AV step: lower create\n");
    result = hs_create(&create);
    if (result != 0) {
      fprintf(stderr, "VIDAA: HS_EsplayerCreate failed: %d\n", result);
      return -4;
    }

    HMPOpenParam video_open = {
      HMP_DATA_AUDIO_VIDEO, HMP_STREAM_VIDEO, codec, 0, 0,
    };
    unsigned char codec_info[3] = {0, 0, 0};
    fprintf(stderr, "VIDAA AV step: lower open video\n");
    result = hs_open(player_id, &video_open, codec_info);
    if (result != 0) {
      fprintf(stderr, "VIDAA: HS_EsplayerOpen video failed: %d\n", result);
      return -6;
    }

    HMPOpenParam audio_open_param = {
      HMP_DATA_AUDIO_VIDEO, HMP_STREAM_AUDIO, HMP_CODEC_MP3, 0, 0,
    };
    unsigned char audio_codec_info[52] = {0};
    audio_codec_info[2] = 2;
    uint32_t audio_sample_rate = 48000;
    memcpy(audio_codec_info + 4, &audio_sample_rate,
           sizeof(audio_sample_rate));
    audio_codec_info[14] = 16;
    uint16_t audio_channels = 2;
    memcpy(audio_codec_info + 16, &audio_channels, sizeof(audio_channels));
    audio_codec_info[18] = 0;
    audio_codec_info[19] = 1;
    fprintf(stderr, "VIDAA AV step: lower open audio\n");
    result = hs_open(player_id, &audio_open_param, audio_codec_info);
    if (result != 0) {
      fprintf(stderr, "VIDAA: HS_EsplayerOpen audio failed: %d\n", result);
      return -6;
    }

    result = hs_set_speed(player_id, 100);
    if (result != 0)
      fprintf(stderr, "VIDAA: HS_EsplayerSetSpeed failed: %d\n", result);

    /* HUI rejects data written before StreamInjectStart. Start the two lower
     * streams now so the initial video key frame is not discarded. */
    int early_audio_result = hs_start(player_id, HMP_STREAM_AUDIO);
    int early_video_result = hs_start(player_id, HMP_STREAM_VIDEO);
    int early_play_result = hs_play(player_id);
    hs_streams_started = early_audio_result == 0 &&
                         early_video_result == 0 && early_play_result == 0;
    fprintf(stderr,
            "VIDAA: lower early start: audio=%d video=%d play=%d\n",
            early_audio_result, early_video_result, early_play_result);

    /* Do not expose the player to either network thread until both streams
     * are open. */
    direct_hs_av = true;
    player = (void*)(uintptr_t)1;
    fprintf(stderr, "VIDAA: lower shared AV player ready (%s)\n",
            codec == HMP_CODEC_HEVC ? "HEVC" : "H.264");
    return 0;
  }

  fprintf(stderr, "VIDAA AV step: create\n");
  HMPCreateParam create = {
    (uint32_t)getpid(),
    0,
    audio_video_mode() ? HMP_DATA_AUDIO_VIDEO : HMP_DATA_VIDEO,
    HMP_STREAMING_MSE,
  };
  int result = HMPCreate(&create, &player);
  if (result != 0 || player == NULL) {
    fprintf(stderr, "VIDAA: HMPCreate failed: %d\n", result);
    return -4;
  }

  fprintf(stderr, "VIDAA AV step: register\n");
  result = HMPRegisterCallback(player, hmp_event, NULL);
  if (result != 0) {
    fprintf(stderr, "VIDAA: HMPRegisterCallback failed: %d\n", result);
    return -5;
  }

  HMPOpenParam open = {
    audio_video_mode() ? HMP_DATA_AUDIO_VIDEO : HMP_DATA_VIDEO,
    HMP_STREAM_VIDEO,
    codec,
    0,
    0,
  };
  unsigned char codec_info[3] = {0, 0, 0};
  fprintf(stderr, "VIDAA AV step: open video\n");
  result = HMPOpen(player, NULL, codec_info, open);
  if (result != 0) {
    fprintf(stderr, "VIDAA: HMPOpen failed for codec %u: %d\n", codec, result);
    return -6;
  }

  if (audio_video_mode()) {
    HMPOpenParam audio_open_param = {
        HMP_DATA_AUDIO_VIDEO, HMP_STREAM_AUDIO, HMP_CODEC_MP3, 0, 0,
    };
    unsigned char audio_codec_info[52] = {0};
    audio_codec_info[2] = 2;
    uint32_t audio_sample_rate = 48000;
    memcpy(audio_codec_info + 4, &audio_sample_rate,
           sizeof(audio_sample_rate));
    audio_codec_info[14] = 16;
    uint16_t audio_channels = 2;
    memcpy(audio_codec_info + 16, &audio_channels, sizeof(audio_channels));
    audio_codec_info[18] = 0;
    audio_codec_info[19] = 1;
    fprintf(stderr, "VIDAA AV step: open audio\n");
    result = HMPOpen(player, NULL, audio_codec_info, audio_open_param);
    if (result != 0) {
      fprintf(stderr, "VIDAA: HMPOpen failed for MP3 clock: %d\n", result);
      return -6;
    }
  }

  fprintf(stderr, "VIDAA AV step: speed\n");
  result = HMPSetSpeed(player, 100);
  if (result != 0)
    fprintf(stderr, "VIDAA: HMPSetSpeed failed: %d\n", result);

  hs_get_state =
      (int (*)(int, uint32_t*))dlsym(RTLD_DEFAULT, "HS_EsplayerGetState");
  hs_get_start_done =
      (int (*)(int, uint32_t, bool*))dlsym(
          RTLD_DEFAULT, "HS_EsplayerGetStartDoneState");
  hs_set_first_present_info =
      (int (*)(int, long long))dlsym(
          RTLD_DEFAULT, "HS_EsplayerSetFirstPresentInfo");
  hui_write_data =
      (int (*)(uint32_t, const void*, uint32_t, uint64_t))dlsym(
          RTLD_DEFAULT, "HUI_StreamInjectWriteData");
  hui_get_pts = (int (*)(uint32_t, uint64_t*))dlsym(
      RTLD_DEFAULT, "HUI_StreamInjectGetPts");
  hui_resume = (int (*)(uint32_t))dlsym(
      RTLD_DEFAULT, "HUI_StreamInjectResume");
  hui_flush = (int (*)(uint32_t))dlsym(
      RTLD_DEFAULT, "HUI_StreamInjectFlush");
  hui_sync_adjust = (int (*)(uint32_t, int64_t))dlsym(
      RTLD_DEFAULT, "HUI_StreamInjectSyncAdjust");
  hui_set_interdevice_sync = (int (*)(uint32_t, bool))dlsym(
      RTLD_DEFAULT, "HUI_StreamInjectSetInterDeviceSync");

  /* The TV browser calls play before its MSE SourceBuffer appends samples.
   * Keep this as a separate diagnostic mode until it is verified on-screen. */
  if (!audio_video_mode() &&
      (strcmp(presentation_mode, "early") == 0 ||
       strcmp(presentation_mode, "direct-early") == 0)) {
    result = HMPStart(player);
    fprintf(stderr, "VIDAA: early playback start=%d\n", result);
    if (result == 0) {
      started = true;
      int resume_result = hui_resume != NULL
                              ? hui_resume(VIDAA_VIDEO_INJECTOR_HANDLE)
                              : -1;
      fprintf(stderr, "VIDAA: early lower resume=%d\n", resume_result);
    }
  }

  fprintf(stderr, "VIDAA: hardware decoder ready (%s, mode=%s, hui=%u)\n",
          codec == HMP_CODEC_HEVC ? "HEVC" : "H.264", presentation_mode,
          hui_write_data != NULL ? 1u : 0u);
  return 0;
}

static int reserve_packet(size_t length) {
  if (packet_capacity >= length)
    return 0;

  unsigned char* resized = realloc(packet, length);
  if (resized == NULL)
    return -1;

  packet = resized;
  packet_capacity = length;
  return 0;
}

static int vidaa_setup(int video_format, int width, int height, int redraw_rate,
                       void* context, int dr_flags) {
  (void)context;
  (void)dr_flags;

  if (video_format & VIDEO_FORMAT_MASK_H264)
    codec = HMP_CODEC_H264;
  else if (video_format & VIDEO_FORMAT_MASK_H265)
    codec = HMP_CODEC_HEVC;
  else {
    fprintf(stderr, "VIDAA: unsupported video format 0x%x\n", video_format);
    return -1;
  }

  debug_events = getenv("MOONLIGHT_VIDAA_DEBUG") != NULL;
  frame_duration = redraw_rate > 0 ? 1.0 / redraw_rate : 1.0 / 60.0;
  first_source_pts = 0.0;
  have_first_source_pts = false;
  started = false;
  direct_hs_av = false;
  direct_injplay = false;
  injplay_handle = 0;
  injplay_decoder_handle = 0;
  injplay_display_handle = 0;
  injplay_original_game_mode = 0;
  injplay_game_mode_changed = false;
  injplay_original_window_mute = 0;
  injplay_window_mute_saved = false;
  injplay_window_unmuted = false;
  injplay_window_checked = false;
  displaced_display_input_count = 0;
  hs_streams_started = false;
  start_attempts = 0;
  next_start_audio_packet = 0;
  video_queued = false;
  audio_open = false;
  audio_queued = false;
  audio_header_sent = false;
  audio_pts = 0.001;
  audio_duration = 0.005;
  audio_clock_accumulator = 0.0;
  submitted_audio_packets = 0;
  presentation_resumed = false;
  submitted_frames = 0;
  display_apply_attempts = 0;
  hui_playing = false;
  sync_adjusted = false;
  requested_sync_adjust = 0;
  live_rebased = false;
  live_pts_shift = 0.0;
  live_flush_started = false;
  live_flush_ready = false;
  live_catchup_started = false;
  live_catchup_done = false;
  player = NULL;
  /* Limelight can initialize a custom renderer before it knows the negotiated
   * dimensions. A zero-sized clip rectangle makes the TV video plane black.
   * This launcher currently requests a fixed 1080p stream. */
  source_width = width > 0 ? width : 1920;
  source_height = height > 0 ? height : 1080;
  presentation_mode = getenv("MOONLIGHT_VIDAA_MODE");
  if (presentation_mode == NULL || *presentation_mode == '\0')
    presentation_mode = "normal";
  const char* sync_adjust_text = getenv("MOONLIGHT_VIDAA_SYNC_ADJUST");
  if (sync_adjust_text != NULL)
    requested_sync_adjust = strtoll(sync_adjust_text, NULL, 10);

  void* mi_library = dlopen("libmi.so", RTLD_NOW | RTLD_GLOBAL);
  if (mi_library == NULL)
    mi_library = dlopen("/vendor/lib/utopia/libmi.so", RTLD_NOW | RTLD_GLOBAL);
  if (mi_library == NULL) {
    fprintf(stderr, "VIDAA: cannot load libmi.so: %s\n", dlerror());
    return -2;
  }

  mi_sys_init = (int (*)(void*))dlsym(mi_library, "MI_SYS_Init");
  mi_aout_init = (int (*)(void*))dlsym(mi_library, "MI_AOUT_Init");
  mi_audio_init = (int (*)(void*))dlsym(mi_library, "MI_AUDIO_Init");
  mi_pcm_init = (int (*)(void*))dlsym(mi_library, "MI_PCM_Init");
  mi_video_init = (int (*)(void*))dlsym(mi_library, "MI_VIDEO_Init");
  mi_disp_init = (int (*)(void*))dlsym(mi_library, "MI_DISP_Init");
  if (mi_sys_init == NULL || mi_aout_init == NULL || mi_audio_init == NULL ||
      mi_pcm_init == NULL || mi_video_init == NULL || mi_disp_init == NULL) {
    fprintf(stderr, "VIDAA: cannot find required MI initialization functions\n");
    return -2;
  }

  MISysInitParam mi_init;
  memset(&mi_init, 0, sizeof(mi_init));
  mi_init.client_name = "moonlight";
  int result = mi_sys_init(&mi_init);
  if (result != 0) {
    fprintf(stderr, "VIDAA: MI_SYS_Init failed: %d\n", result);
    return -2;
  }

  result = mi_aout_init(NULL);
  fprintf(stderr, "VIDAA: MI_AOUT_Init=%d\n", result);
  if (result != 0) {
    fprintf(stderr, "VIDAA: MI_AOUT_Init failed: %d\n", result);
    return -2;
  }

  result = mi_audio_init(NULL);
  fprintf(stderr, "VIDAA: MI_AUDIO_Init=%d\n", result);
  if (result != 0) {
    fprintf(stderr, "VIDAA: MI_AUDIO_Init failed: %d\n", result);
    return -2;
  }

  result = mi_pcm_init(NULL);
  fprintf(stderr, "VIDAA: MI_PCM_Init=%d\n", result);
  if (result != 0)
    fprintf(stderr, "VIDAA: MI_PCM_Init unavailable: %d\n", result);

  result = mi_video_init(NULL);
  fprintf(stderr, "VIDAA: MI_VIDEO_Init=%d\n", result);
  if (result != 0) {
    fprintf(stderr, "VIDAA: MI_VIDEO_Init failed: %d\n", result);
    return -2;
  }

  result = mi_disp_init(NULL);
  fprintf(stderr, "VIDAA: MI_DISP_Init=%d\n", result);
  if (result != 0) {
    fprintf(stderr, "VIDAA: MI_DISP_Init failed: %d\n", result);
    return -2;
  }

  result = HMPInit();
  if (result != 0) {
    fprintf(stderr, "VIDAA: HMPInit failed: %d\n", result);
    return -3;
  }

  if (audio_video_mode()) {
    fprintf(stderr, "VIDAA: waiting for audio format before AV player create\n");
    return 0;
  }
  return create_player_locked();
}

static void vidaa_cleanup(void) {
  close_video_trace();
  /* HMPStop blocks on this firmware if an elementary stream has no EOS marker.
   * Destroy the player directly so a failed test does not leave the hardware
   * injector allocated until the TV reboots. */
  if (player != NULL) {
    if (direct_injplay) {
      set_dms_flip_trigger(false);
      if (speed_drop_active && mi_video_set_speed != NULL)
        mi_video_set_speed(injplay_decoder_handle, 0);
      speed_drop_active = false;
      restore_game_mode();
      restore_window_mute();
      int stop_result =
          mi_injplay_stop != NULL ? mi_injplay_stop(injplay_handle) : -1;
      int close_result =
          mi_injplay_close != NULL ? mi_injplay_close(injplay_handle) : -1;
      int deinit_result =
          mi_injplay_deinit != NULL ? mi_injplay_deinit() : -1;
      fprintf(stderr, "VIDAA INJPLAY: stop=%d close=%d deinit=%d\n",
              stop_result, close_result, deinit_result);
      restore_displaced_display_inputs();
    } else if (direct_hs_av && hs_close != NULL) {
      int close_result = hs_close(getpid());
      fprintf(stderr, "VIDAA: lower player close=%d\n", close_result);
      if (hs_deinit != NULL) {
        int deinit_result = hs_deinit();
        fprintf(stderr, "VIDAA: lower player deinit=%d\n", deinit_result);
      }
    } else {
      int destroy_result = HMPDestroy(player);
      fprintf(stderr, "VIDAA: player destroy=%d\n", destroy_result);
    }
  }
  player = NULL;
  started = false;
  free(packet);
  packet = NULL;
  packet_capacity = 0;
  free(audio_packet);
  audio_packet = NULL;
  audio_packet_capacity = 0;
}

int vidaa_hmp_audio_setup(int sample_rate, int channels,
                          int samples_per_frame) {
  if (!audio_video_mode()) {
    /* Keep the Moonlight session alive while testing the independent video
     * player. Audio is restored after the video clock is confirmed. */
    fprintf(stderr, "VIDAA audio: disabled for video-only test\n");
    return 0;
  }

  pthread_mutex_lock(&player_mutex);
  int result = 0;
  if (player == NULL)
    result = create_player_locked();
  if (result != 0) {
    pthread_mutex_unlock(&player_mutex);
    return result;
  }
  if (result == 0) {
    audio_open = true;
    audio_duration = sample_rate > 0
                         ? (double)samples_per_frame / (double)sample_rate
                         : 0.005;
    fprintf(stderr,
            "VIDAA audio: silent MP3 clock ready (%d Hz, %d channels, %.1f ms input)\n",
            sample_rate, channels, audio_duration * 1000.0);
  }
  pthread_mutex_unlock(&player_mutex);
  return result;
}

void vidaa_hmp_audio_cleanup(void) {
  audio_open = false;
}

void vidaa_hmp_audio_submit(const char* data, int length) {
  if (player == NULL || !audio_open || data == NULL || length <= 0)
    return;

  /* One complete AAC-LC stereo access unit in MPEG-4 ADTS at 48 kHz. The
   * hardware audio component is muted; this frame supplies only the clock. */
  static const unsigned char clock_aac_frame[207] = {
      0xff, 0xf1, 0x4c, 0xa0, 0x19, 0xe0, 0x00, 0x21, 0x4d, 0xe2, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0x03, 0x98, 0x7d, 0x75, 0x20, 0x4b, 0xd4, 0xc2,
      0x5f, 0x41, 0x03, 0x28, 0x71, 0xce, 0x56, 0xa6, 0x47, 0x7f, 0xd9, 0x27,
      0x57, 0x08, 0x75, 0xe0, 0x5d, 0x2f, 0x01, 0xea, 0x54, 0x81, 0xe0, 0x86,
      0x82, 0xa7, 0xf0, 0xc8, 0x04, 0x7c, 0x3f, 0x44, 0x1b, 0xa3, 0x23, 0x36,
      0x01, 0x72, 0x23, 0x67, 0xe2, 0x53, 0xd4, 0x8d, 0x8c, 0x89, 0xf5, 0x1e,
      0xc6, 0x4b, 0x19, 0x9f, 0x21, 0x9e, 0xc8, 0x10, 0xad, 0x00, 0x80, 0xdc,
      0x42, 0x09, 0x72, 0x1a, 0xc9, 0xc0, 0x87, 0xed, 0xfc, 0xd7, 0xd5, 0x7d,
      0xe5, 0xa4, 0x3a, 0x43, 0xfa, 0x84, 0x66, 0xf1, 0xfa, 0xf7, 0x17, 0x95,
      0x8b, 0xa8, 0x52, 0x75, 0x4b, 0xc3, 0x39, 0x64, 0xb2, 0x3c, 0xca, 0x76,
      0x80, 0x47, 0x84, 0xd0, 0x25, 0xa9, 0xd6, 0x59, 0xe4, 0xeb, 0x89, 0x9c,
      0x76, 0x4f, 0x19, 0xb1, 0xdf, 0x91, 0x62, 0xb2, 0x24, 0x49, 0xaa, 0xac,
      0xbc, 0xa1, 0x2f, 0xcf, 0xa0, 0xe5, 0xf8, 0x16, 0xeb, 0x6f, 0x45, 0x99,
      0x3b, 0xbc, 0x73, 0x5a, 0xe5, 0xcd, 0x3f, 0x35, 0x2a, 0xc5, 0x64, 0x18,
      0xa4, 0x6d, 0xf4, 0x42, 0xab, 0xd7, 0xd4, 0xd4, 0xd7, 0x3f, 0x53, 0x3c,
      0x87, 0xf1, 0x93, 0x80, 0xaa, 0x70, 0xd4, 0x4b, 0x41, 0x0e, 0x1f, 0x07,
      0xc1, 0xf0, 0x73, 0xf4, 0x08, 0x75, 0xe0, 0x5d, 0x2f, 0x01, 0xea, 0x54,
      0x81, 0xe3, 0x80,
  };
  /* One silent MPEG-1 Layer III frame: 48 kHz, stereo, 192 kbit/s. The
   * remainder of this 576-byte frame is zero-filled by C. */
  static const unsigned char clock_mp3_frame[576] = {
      0xff, 0xfb, 0xb4, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37, 0x80,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0xf0, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xde, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b, 0xc0,
  };
  (void)clock_aac_frame;
  const double clock_frame_duration = 1152.0 / 48000.0;

  pthread_mutex_lock(&player_mutex);
  audio_clock_accumulator += audio_duration;
  if (submitted_audio_packets == 0 ||
      audio_clock_accumulator >= clock_frame_duration) {
    if (submitted_audio_packets != 0)
      audio_clock_accumulator -= clock_frame_duration;
    int result =
        direct_hs_av
            ? hs_push_data(getpid(), HMP_STREAM_AUDIO, HMP_CODEC_MP3, 0,
                           audio_pts, clock_frame_duration,
                           sizeof(clock_mp3_frame), clock_mp3_frame,
                           NULL, NULL, NULL)
            : HMPPushData(player, HMP_STREAM_AUDIO, HMP_CODEC_MP3, 0,
                          audio_pts, clock_frame_duration,
                          sizeof(clock_mp3_frame), clock_mp3_frame,
                          NULL, NULL, NULL);
    if (result == 0) {
      audio_queued = true;
      submitted_audio_packets++;
      if (debug_events && submitted_audio_packets <= 3)
        fprintf(stderr,
                "VIDAA MP3 clock packet %u: bytes=%zu pts=%.3f\n",
                submitted_audio_packets, sizeof(clock_mp3_frame), audio_pts);
      audio_pts += clock_frame_duration;
      start_player_locked();
    } else if (submitted_audio_packets < 3) {
      fprintf(stderr, "VIDAA audio: MP3 clock push failed: %d\n", result);
    }
  }
  pthread_mutex_unlock(&player_mutex);
}

/* Watches the DMS delay and drains its FIFO when a backlog forms. Returns -1
 * when the caller must drop frames until the next IDR (mode 0), else 0. */
static int vidaa_dms_check(unsigned int frame) {
  if (dms_drain_mode == 3) {
    if (mi_disp_get_attr == NULL || mi_video_set_speed == NULL)
      return 0;
    if (speed_drop_active) {
      uint32_t counts[5] = {0};
      bool dropped = mi_video_get_attr(injplay_decoder_handle, 0x101, NULL, counts) == 0 &&
                     counts[3] != speed_drop_start_count;
      if (dropped || frame - speed_drop_start_frame >= 4) {
        int result = mi_video_set_speed(injplay_decoder_handle, 0);
        speed_drop_active = false;
        if (debug_events || dms_drains <= 20)
          fprintf(stderr, "VIDAA DMS drain %u done: frames=%u dropped=%u speed1x=%d\n",
                  dms_drains, frame - speed_drop_start_frame,
                  counts[3] - speed_drop_start_count, result);
      }
      return 0;
    }
    if (frame % 2 != 0)
      return 0;
    uint32_t delay_info[32];
    memset(delay_info, 0, sizeof(delay_info));
    if (mi_disp_get_attr(injplay_display_handle, 6, NULL, delay_info) == 0)
      dms_delay_ms = delay_info[4];
    if (dms_drain_ms <= 0 || frame <= 60)
      return 0;
    dms_over_count = dms_delay_ms >= (uint32_t)dms_drain_ms ? dms_over_count + 1 : 0;
    if (dms_over_count < 2 || frame - dms_last_drain_frame < 6)
      return 0;
    uint32_t counts[5] = {0};
    if (mi_video_get_attr(injplay_decoder_handle, 0x101, NULL, counts) != 0)
      return 0;
    int result = mi_video_set_speed(injplay_decoder_handle, 256);
    if (result != 0) {
      fprintf(stderr, "VIDAA DMS drain: 2x speed failed=%d; disabling\n", result);
      dms_drain_ms = 0;
      return 0;
    }
    dms_over_count = 0;
    dms_last_drain_frame = frame;
    dms_drains++;
    speed_drop_active = true;
    speed_drop_start_frame = frame;
    speed_drop_start_count = counts[3];
    if (debug_events || dms_drains <= 20)
      fprintf(stderr, "VIDAA DMS drain %u: frame=%u delay_ms=%u speed2x\n", dms_drains,
              frame, dms_delay_ms);
    return 0;
  }
  if (mi_disp_get_attr == NULL || frame % 15 != 0)
    return 0;
  uint32_t delay_info[32];
  memset(delay_info, 0, sizeof(delay_info));
  if (mi_disp_get_attr(injplay_display_handle, 6, NULL, delay_info) == 0)
    dms_delay_ms = delay_info[4];
  if (dms_drain_ms <= 0 || frame <= 120)
    return 0;
  dms_over_count = dms_delay_ms >= (uint32_t)dms_drain_ms ? dms_over_count + 1 : 0;
  if (dms_over_count < 2 || frame - dms_last_drain_frame < 120u)
    return 0;
  dms_over_count = 0;
  dms_last_drain_frame = frame;
  dms_drains++;
  if (debug_events || dms_drains <= 20)
    fprintf(stderr, "VIDAA DMS drain %u: frame=%u delay_ms=%u idr\n", dms_drains,
            frame, dms_delay_ms);
  return dms_drain_mode == 0 ? -1 : 0;
}

static int vidaa_submit_decode_unit(PDECODE_UNIT decode_unit) {
  uint64_t submit_start_us = LiGetMicroseconds();
  uint64_t submit_absolute_us = trace_video != NULL ? monotonic_us() : 0;
  if (player == NULL || reserve_packet((size_t)decode_unit->fullLength + 256) != 0)
    return DR_NEED_IDR;

  if (direct_injplay && vidaa_dms_check(submitted_frames + 1) < 0)
    return DR_NEED_IDR;

  size_t offset = 0;
  for (PLENTRY entry = decode_unit->bufferList; entry != NULL; entry = entry->next) {
    if (entry->length < 0 || reserve_packet(offset + (size_t)entry->length + 128) != 0)
      return DR_NEED_IDR;
    if (h264_timing_fps && entry->bufferType == BUFFER_TYPE_SPS) {
      int fixed_length = gs_sps_add_timing(entry, h264_timing_fps,
          packet + offset, entry->length + 128);
      fprintf(stderr, "VIDAA H264 timing: fps=%d input=%d output=%d\n",
              h264_timing_fps, entry->length, fixed_length);
      if (fixed_length > 0) {
        offset += (size_t)fixed_length;
        continue;
      }
    }
    memcpy(packet + offset, entry->data, (size_t)entry->length);
    offset += (size_t)entry->length;
  }

  if (direct_injplay && hevc_filler_enabled) {
    offset = vidaa_append_hevc_filler(packet, offset, packet_capacity);
    if (!offset)
      return DR_NEED_IDR;
  }

  double source_pts = decode_unit->presentationTimeUs / 1000000.0;
  if (!have_first_source_pts) {
    first_source_pts = source_pts;
    have_first_source_pts = true;
  }
  /* Match browser MSE timing, but start at 1 ms. Zero is also the firmware's
   * internal "first PTS not set" sentinel. */
  bool synthetic_timing = strcmp(presentation_mode, "synthetic") == 0 ||
                          strcmp(presentation_mode, "direct-synthetic") == 0 ||
                          strcmp(presentation_mode, "direct-miracast") == 0 ||
                          strcmp(presentation_mode, "direct-lowlatency") == 0 ||
                          strcmp(presentation_mode, "direct-apstream") == 0;
  double pts = synthetic_timing
                   ? (double)submitted_frames * frame_duration + 0.001
                   : source_pts - first_source_pts + 0.001;
  /* This HMP argument is the stream generation ("peek ID"), not a key-frame
   * flag. Changing it makes the player reset its codec. Keep it stable. */
  uint32_t stream_generation = 0;
  if (debug_events && submitted_frames < 3) {
    fprintf(stderr,
            "VIDAA packet %u: bytes=%zu idr=%u pts=%.3f head="
            "%02x %02x %02x %02x %02x %02x %02x %02x\n",
            submitted_frames, offset,
            decode_unit->frameType == FRAME_TYPE_IDR ? 1u : 0u, source_pts,
            offset > 0 ? packet[0] : 0, offset > 1 ? packet[1] : 0,
            offset > 2 ? packet[2] : 0, offset > 3 ? packet[3] : 0,
            offset > 4 ? packet[4] : 0, offset > 5 ? packet[5] : 0,
            offset > 6 ? packet[6] : 0, offset > 7 ? packet[7] : 0);
  }
  submitted_frames++;
  if (direct_injplay) {
    MIInjectQueryBufferParams query = {(uint32_t)offset};
    MIInjectBufferParams buffer;
    memset(&buffer, 0, sizeof(buffer));
    int query_result =
        mi_injplay_query_buffer(injplay_handle, &query, &buffer);
    if (query_result != 0 || buffer.buffer_address == NULL ||
        buffer.buffer_size < offset) {
      if (debug_events &&
          (submitted_frames <= 10 || submitted_frames % 120 == 0))
        fprintf(stderr,
                "VIDAA INJPLAY: no buffer frame=%u result=%d have=%u need=%zu\n",
                submitted_frames, query_result, buffer.buffer_size, offset);
      /* Losing an encoded reference frame also invalidates later P-frames. */
      return DR_NEED_IDR;
    }

    memcpy(buffer.buffer_address, packet, offset);
    buffer.buffer_size = (uint32_t)offset;
    /* Unlike HUI's millisecond API, MI_INJECT uses 90 kHz ticks.
     * The firmware divides u64FirstPts by 90 when printing milliseconds. */
    buffer.pts = (uint64_t)(pts * 90000.0 + 0.5);
    buffer.picture_start = 1;
    int write_result =
        mi_injplay_write_complete(injplay_handle, &buffer);
    uint64_t submit_end_us = LiGetMicroseconds();
    if (debug_events &&
        (submitted_frames <= 3 || submitted_frames % 120 == 0))
      fprintf(stderr,
              "VIDAA INJPLAY: frame=%u bytes=%zu pts_90k=%llu query=%d write=%d\n",
              submitted_frames, offset,
              (unsigned long long)buffer.pts, query_result, write_result);
    if (write_result != 0)
      return DR_NEED_IDR;
    record_video_trace(decode_unit, packet, offset, submit_start_us,
                       submit_absolute_us, submit_end_us);
    video_queued = true;

    if (!injplay_window_checked && submitted_frames <= 300 &&
        submitted_frames % 6 == 0)
      show_ready_window();

    if (submitted_frames % 120 == 0) {
      uint64_t display_pts = 0, parser_pts = 0;
      uint32_t counts[5] = {0}; /* MI_VIDEO_FrameCntInfo_t, 20 bytes */
      uint32_t buffers[7] = {0}; /* MI_INJPLAY_AvDecBufferInfo_t, 28 bytes */
      int pts_result = mi_video_get_attr(
          injplay_decoder_handle, 0x109, NULL, &display_pts);
      int parser_result = mi_video_get_attr(
          injplay_decoder_handle, 0x10a, NULL, &parser_pts);
      int count_result = mi_video_get_attr(
          injplay_decoder_handle, 0x101, NULL, counts);
      int buffer_result = mi_injplay_get_attr != NULL
          ? mi_injplay_get_attr(injplay_handle, 4, NULL, buffers) : -1;
      uint32_t network_rtt_ms = 0, network_variance_ms = 0;
      bool network_rtt_valid = LiGetEstimatedRttInfo(
          &network_rtt_ms, &network_variance_ms);
      fprintf(stderr,
          "VIDAA timing: frames=%u source_ms=%.1f submitted_ms=%.1f "
          "display_ms=%.1f pts_result=%d parser_ms=%.1f parser_result=%d "
          "host_ms=%.1f receive_ms=%.1f client_queue_ms=%.1f write_ms=%.1f "
          "count_result=%d decoded=%u errors=%u skipped=%u dropped=%u "
          "displayed=%u buffer_result=%d es_bytes=%u es_capacity=%u "
          "rtt_valid=%u rtt_ms=%u rtt_variance_ms=%u pending_frames=%d "
          "dms_delay_ms=%u dms_drains=%u\n",
          submitted_frames, (source_pts - first_source_pts) * 1000.0,
          buffer.pts / 90.0, display_pts / 90.0,
          pts_result, parser_pts / 90.0, parser_result,
          decode_unit->frameHostProcessingLatency / 10.0,
          (double)(decode_unit->enqueueTimeUs - decode_unit->receiveTimeUs) / 1000.0,
          (double)(submit_start_us - decode_unit->enqueueTimeUs) / 1000.0,
          (double)(submit_end_us - submit_start_us) / 1000.0,
          count_result, counts[0], counts[1], counts[2], counts[3], counts[4],
          buffer_result, buffers[1], buffers[0], network_rtt_valid ? 1u : 0u,
          network_rtt_ms, network_variance_ms, LiGetPendingVideoFrames(),
          dms_delay_ms, dms_drains);
    }

    return DR_OK;
  }

  bool direct_mode = strcmp(presentation_mode, "direct") == 0 ||
                     strcmp(presentation_mode, "direct-early") == 0 ||
                     strcmp(presentation_mode, "direct-synthetic") == 0 ||
                     strcmp(presentation_mode, "direct-nopts") == 0 ||
                     strcmp(presentation_mode, "direct-live") == 0 ||
                     strcmp(presentation_mode, "direct-flush") == 0 ||
                     strcmp(presentation_mode, "direct-miracast") == 0 ||
                     strcmp(presentation_mode, "direct-lowlatency") == 0 ||
                     strcmp(presentation_mode, "direct-apstream") == 0;
  double output_pts = pts;
  if (strcmp(presentation_mode, "direct-live") == 0 && started &&
      hui_playing && !live_catchup_done && hui_get_pts != NULL) {
    uint64_t decoder_pts = 0;
    int pts_result = hui_get_pts(VIDAA_VIDEO_INJECTOR_HANDLE, &decoder_pts);
    double decoder_seconds = (double)decoder_pts / 1000.0;
    double lag = pts - decoder_seconds;
    if (!live_catchup_started && pts_result == 0 && lag > 0.150) {
      int speed_result = HMPSetSpeed(player, 200);
      live_catchup_started = speed_result == 0;
      fprintf(stderr,
              "VIDAA: live catch-up start frame=%u lag=%.3f speed=%d\n",
              submitted_frames, lag, speed_result);
    }
    else if (live_catchup_started && pts_result == 0 && lag < 0.080) {
      int speed_result = HMPSetSpeed(player, 100);
      live_catchup_done = speed_result == 0;
      fprintf(stderr,
              "VIDAA: live catch-up stop frame=%u lag=%.3f speed=%d\n",
              submitted_frames, lag, speed_result);
    }
  }
  if (strcmp(presentation_mode, "direct-flush") == 0 && started &&
      hui_playing && !live_flush_started && hui_flush != NULL &&
      hui_get_pts != NULL) {
    uint64_t before_flush_pts = 0;
    int before_result = hui_get_pts(VIDAA_VIDEO_INJECTOR_HANDLE,
                                    &before_flush_pts);
    live_flush_started = true;
    live_flush_ready = false;
    int flush_result = hui_flush(VIDAA_VIDEO_INJECTOR_HANDLE);
    int resume_result = hui_resume != NULL
                            ? hui_resume(VIDAA_VIDEO_INJECTOR_HANDLE)
                            : -1;
    fprintf(stderr,
            "VIDAA: live flush frame=%u clock=%llu get=%d "
            "flush=%d resume=%d ready=%u\n",
            submitted_frames, (unsigned long long)before_flush_pts,
            before_result, flush_result, resume_result,
            live_flush_ready ? 1u : 0u);
    /* The flush discards the current access unit. Ask Sunshine for a new IDR
     * and do not submit more delta frames until the decoder reports ready. */
    return DR_NEED_IDR;
  }
  if (strcmp(presentation_mode, "direct-flush") == 0 &&
      live_flush_started && !live_rebased) {
    if (!live_flush_ready)
      return DR_OK;
    if (decode_unit->frameType != FRAME_TYPE_IDR)
      return DR_NEED_IDR;

    uint64_t decoder_pts = 0;
    int pts_result = hui_get_pts != NULL
                         ? hui_get_pts(VIDAA_VIDEO_INJECTOR_HANDLE,
                                       &decoder_pts)
                         : -1;
    double live_target = (double)decoder_pts / 1000.0 + 0.001;
    live_pts_shift = pts - live_target;
    live_rebased = true;
    fprintf(stderr,
            "VIDAA: live rebase frame=%u clock=%llu get=%d shift=%.3f\n",
            submitted_frames, (unsigned long long)decoder_pts, pts_result,
            live_pts_shift);
  }
  if (live_rebased)
    output_pts = pts - live_pts_shift;
  int result;
  if (direct_mode && started && hui_write_data != NULL) {
    uint64_t lower_pts = strcmp(presentation_mode, "direct-nopts") == 0
                             ? UINT64_MAX
                             : (uint64_t)(output_pts * 1000.0 + 0.5);
    result = hui_write_data(VIDAA_VIDEO_INJECTOR_HANDLE, packet,
                            (uint32_t)offset, lower_pts);
  } else {
    pthread_mutex_lock(&player_mutex);
    result = direct_hs_av
                 ? hs_push_data(getpid(), HMP_STREAM_VIDEO, codec,
                                stream_generation, pts, frame_duration,
                                (uint32_t)offset, packet, NULL, NULL, NULL)
                 : HMPPushData(player, HMP_STREAM_VIDEO, codec,
                               stream_generation, pts, frame_duration,
                               (uint32_t)offset, packet, NULL, NULL, NULL);
    pthread_mutex_unlock(&player_mutex);
  }
  if (result != 0) {
    fprintf(stderr, "VIDAA: video push failed: %d\n", result);
    return DR_NEED_IDR;
  }

  if ((hui_playing || submitted_frames >= 90) && !sync_adjusted &&
      requested_sync_adjust != 0 &&
      hui_sync_adjust != NULL) {
    int interdevice_result = hui_set_interdevice_sync != NULL
                                 ? hui_set_interdevice_sync(
                                       VIDAA_VIDEO_INJECTOR_HANDLE, true)
                                 : -1;
    int adjust_result = hui_sync_adjust(VIDAA_VIDEO_INJECTOR_HANDLE,
                                        requested_sync_adjust);
    fprintf(stderr,
            "VIDAA: sync adjust %lld ms interdevice=%d result=%d frame=%u\n",
            (long long)requested_sync_adjust, interdevice_result,
            adjust_result, submitted_frames);
    sync_adjusted = true;
  }

  video_queued = true;
  if (!started && !audio_video_mode()) {
    /* Avoid the video-only player's startup-underflow pause. Sunshine's first
     * IDR has PTS zero, while the next frame can begin several hundred
     * milliseconds later after session setup. */
    if (strcmp(presentation_mode, "buffered") == 0 &&
        submitted_frames < 30)
      return DR_OK;

    /* Match the TV browser's MSE path: push the first sample, then play. */
    /* MseStreamingPlayer inherits the unsupported HMP implementation for
     * this control. Call its underlying ES player directly. A value of 1
     * asks it to present the first decoded frame 1 ms after this call. */
    int first_present_result =
        hs_set_first_present_info != NULL
            ? hs_set_first_present_info(getpid(), 1)
            : -1;
    if (debug_events)
      fprintf(stderr, "VIDAA: first presentation timing=%d\n",
              first_present_result);
    result = HMPStart(player);
    if (result != 0) {
      fprintf(stderr, "VIDAA: HMPStart failed: %d\n", result);
      return DR_NEED_IDR;
    }

    /* Playback is active once HMPStart succeeds. This firmware can reject
     * display-control ioctls for an app-owned video plane even though the
     * decoder is already visible at the correct full-screen size. Do not turn
     * those optional controls into a decoder reset. */
    started = true;

    if (strcmp(presentation_mode, "resume") == 0 || direct_mode) {
      int resume_result = HMPResume(player);
      int lower_resume_result = hui_resume != NULL
                                    ? hui_resume(VIDAA_VIDEO_INJECTOR_HANDLE)
                                    : -1;
      fprintf(stderr, "VIDAA: initial resume: hmp=%d hui=%d\n",
              resume_result, lower_resume_result);
    }

    fprintf(stderr, "VIDAA: playback started\n");
  }
  else if (!started && audio_video_mode()) {
    pthread_mutex_lock(&player_mutex);
    result = start_player_locked();
    pthread_mutex_unlock(&player_mutex);
    if (result != 0)
      return DR_NEED_IDR;
  }

  /* The TV's video-only MSE path enters its initial buffering pause after the
   * first decoded frame. With no browser media clock, it never leaves that
   * pause by itself. Re-enter the public pause/resume state once one second of
   * video is queued. This also updates the ES-player's internal pause flags. */
  if (!audio_video_mode() && !direct_mode &&
      strcmp(presentation_mode, "early") != 0 && !presentation_resumed &&
      strcmp(presentation_mode, "buffered") != 0 &&
      !synthetic_timing &&
      submitted_frames >= 60) {
    int pause_result = HMPPause(player);
    int resume_result = HMPResume(player);
    fprintf(stderr, "VIDAA: presentation resume: pause=%d resume=%d\n",
            pause_result, resume_result);
    if (resume_result == 0)
      presentation_resumed = true;
  }

  /* The firmware cannot configure the display plane until the hardware
   * decoder has published its stream information. The old startup call ran
   * too early and failed with GET_DEC_INFO. Retry after video has flowed for
   * one second, then retry at two-second intervals if the plane is not ready. */
  if (started && submitted_frames >= 60 &&
      submitted_frames % 120 == 60 && display_apply_attempts < 5) {
    HMPRectangle display = {0, 0, 3840, 2160, 0};
    HMPRectangle source = {0, 0, source_width, source_height, 0};
    display_apply_attempts++;
    pthread_mutex_lock(&player_mutex);
    int rect_result = direct_hs_av && hs_set_display_rect != NULL
                          ? hs_set_display_rect(getpid(), display, source, 0)
                          : HMPSetDisplayRect(player, display, source, 0);
    int mute_result = direct_hs_av && hs_set_component_mute_state != NULL
                          ? hs_set_component_mute_state(
                                getpid(), HMP_STREAM_VIDEO, false)
                          : HMPSetComponentMuteState(
                                player, HMP_STREAM_VIDEO, false);
    pthread_mutex_unlock(&player_mutex);
    fprintf(stderr,
            "VIDAA: late display apply %u: rect=%d unmute=%d frame=%u\n",
            display_apply_attempts, rect_result, mute_result,
            submitted_frames);
    if (rect_result == 0)
      display_apply_attempts = 5;
  }

  if (debug_events && submitted_frames % 120 == 0) {
    uint32_t state = 0;
    uint32_t speed = 0;
    unsigned char pts_info[24] = {0};
    double current_pts = 0.0;
    double buffered_pts = 0.0;
    uint32_t info[6] = {0};
    uint32_t lower_state = UINT32_MAX;
    bool video_start_done = false;
    bool audio_start_done = false;
    uint64_t lower_pts = 0;
    int state_result = direct_hs_av && hs_get_state != NULL
                           ? hs_get_state(getpid(), &state)
                           : HMPGetState(player, &state);
    int speed_result = direct_hs_av && hs_get_speed != NULL
                           ? hs_get_speed(getpid(), &speed)
                           : HMPGetSpeed(player, &speed);
    int pts_result = direct_hs_av && hs_get_pts != NULL
                         ? hs_get_pts(getpid(), pts_info)
                         : HMPGetPts(player, pts_info);
    memcpy(&current_pts, pts_info, sizeof(current_pts));
    memcpy(&buffered_pts, pts_info + 8, sizeof(buffered_pts));
    uint32_t lower_info[13] = {0};
    int info_result = direct_hs_av && hs_get_video_info != NULL
                          ? hs_get_video_info(getpid(), lower_info)
                          : HMPGetVideoInfo(player, info);
    int lower_state_result = hs_get_state != NULL
                                 ? hs_get_state(getpid(), &lower_state)
                                 : -1;
    int video_start_done_result = hs_get_start_done != NULL
                                      ? hs_get_start_done(
                                            getpid(), HMP_STREAM_VIDEO,
                                            &video_start_done)
                                      : -1;
    int audio_start_done_result = hs_get_start_done != NULL
                                      ? hs_get_start_done(
                                            getpid(), HMP_STREAM_AUDIO,
                                            &audio_start_done)
                                      : -1;
    int hui_pts_result = hui_get_pts != NULL
                             ? hui_get_pts(VIDAA_VIDEO_INJECTOR_HANDLE,
                                           &lower_pts)
                             : -1;
    fprintf(stderr,
            "VIDAA status: frames=%u pts=%.3f bytes=%zu push=%d "
            "state_result=%d state=%u speed_result=%d speed=%u "
            "clock_result=%d clock=%.3f,%.3f info_result=%d "
            "video=%u,%u,%u,%u,%u,%u lower_result=%d lower=%u "
            "video_done_result=%d video_done=%u "
            "audio_done_result=%d audio_done=%u "
            "lower_video=%u,%u,%u,%u,%u,%u hui_pts_result=%d "
            "hui_pts=%llu mode=%s\n",
            submitted_frames, source_pts, offset, result, state_result, state,
            speed_result, speed, pts_result, current_pts, buffered_pts,
            info_result, info[0], info[1], info[2], info[3], info[4], info[5],
            lower_state_result, lower_state, video_start_done_result,
            video_start_done ? 1u : 0u, audio_start_done_result,
            audio_start_done ? 1u : 0u, lower_info[0], lower_info[1],
            lower_info[2], lower_info[3], lower_info[4], lower_info[5],
            hui_pts_result,
            (unsigned long long)lower_pts, presentation_mode);
  }

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_vidaa = {
  .setup = vidaa_setup,
  .cleanup = vidaa_cleanup,
  .submitDecodeUnit = vidaa_submit_decode_unit,
  .capabilities = CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC,
};

DECODER_RENDERER_CALLBACKS* vidaa_get_video_callbacks(void) {
  // Read before RTSP negotiation. setup() is too late to change the host's
  // reference-frame limit. Default is unchanged; the RAM-only test disables
  // RFI and lets the existing protocol request its conservative two-ref limit.
  VideoTuning tuning = read_video_tuning();
  decoder_callbacks_vidaa.capabilities &= ~CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC;
  if (tuning.reference_invalidation)
    decoder_callbacks_vidaa.capabilities |= CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC;
  /* Direct submit decodes on the receive thread and skips the decode-unit
   * queue handoff. */
  decoder_callbacks_vidaa.capabilities &= ~CAPABILITY_DIRECT_SUBMIT;
  if (tuning.direct_submit)
    decoder_callbacks_vidaa.capabilities |= CAPABILITY_DIRECT_SUBMIT;
  fprintf(stderr, "VIDAA: HEVC reference invalidation=%d direct submit=%d\n",
          tuning.reference_invalidation, tuning.direct_submit);
  return &decoder_callbacks_vidaa;
}
