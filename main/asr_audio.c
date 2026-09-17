#include "asr_audio.h"

#include <stdint.h>
#include <string.h>

static void put_u16_le(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32_le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

void asr_wav_build_header(uint8_t out[ASR_WAV_HEADER_BYTES], uint32_t pcm_bytes)
{
    const uint16_t block_align = ASR_SAMPLE_CHANNELS * (ASR_SAMPLE_BITS / 8u);
    const uint32_t byte_rate = ASR_SAMPLE_RATE_HZ * block_align;

    memcpy(out + 0, "RIFF", 4);
    put_u32_le(out + 4, pcm_bytes + 36u);
    memcpy(out + 8, "WAVEfmt ", 8);
    put_u32_le(out + 16, 16u);
    put_u16_le(out + 20, 1u);
    put_u16_le(out + 22, ASR_SAMPLE_CHANNELS);
    put_u32_le(out + 24, ASR_SAMPLE_RATE_HZ);
    put_u32_le(out + 28, byte_rate);
    put_u16_le(out + 32, block_align);
    put_u16_le(out + 34, ASR_SAMPLE_BITS);
    memcpy(out + 36, "data", 4);
    put_u32_le(out + 40, pcm_bytes);
}

size_t asr_multipart_length(size_t prefix_bytes, size_t wav_bytes,
                            size_t suffix_bytes)
{
    if (prefix_bytes > SIZE_MAX - wav_bytes) return SIZE_MAX;
    size_t total = prefix_bytes + wav_bytes;
    if (total > SIZE_MAX - suffix_bytes) return SIZE_MAX;
    return total + suffix_bytes;
}
