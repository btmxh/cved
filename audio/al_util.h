#pragma once

#include "../media/decode_thread.h"
#include "../utils/types.h"
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <libavcodec/codec_par.h>
#include <libswresample/swresample.h>

typedef struct {
  decode_context dc;
  i32 frame_size;
  AVChannelLayout out_layout;
  enum AVSampleFormat out_sample_format;
  ALenum al_format;
  SwrContext *swr;
} audio_playback_context;

bool audio_playback_context_init(audio_playback_context *c, decode_context dc);
void audio_playback_context_free(audio_playback_context *c);
i32 audio_playback_context_fill_buffer(audio_playback_context *c,
                                        ALuint buffer, i32 total_samples);
