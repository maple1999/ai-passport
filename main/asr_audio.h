// Pure helpers for the Voice ASR WAV container and multipart request sizing.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define ASR_SAMPLE_RATE_HZ 16000u
#define ASR_SAMPLE_BITS    16u
#define ASR_SAMPLE_CHANNELS 1u
#define ASR_WAV_HEADER_BYTES 44u

// Build a canonical PCM WAV header for signed 16-bit mono samples.
void asr_wav_build_header(uint8_t out[ASR_WAV_HEADER_BYTES], uint32_t pcm_bytes);

// Return the complete multipart body length; SIZE_MAX means arithmetic overflow.
size_t asr_multipart_length(size_t prefix_bytes, size_t wav_bytes,
                            size_t suffix_bytes);
