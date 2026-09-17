#include "asr_audio.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EXPECT_TRUE(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); \
        return 1; \
    } \
} while (0)

static uint16_t get_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void)
{
    uint8_t header[ASR_WAV_HEADER_BYTES];
    asr_wav_build_header(header, 256000u);

    EXPECT_TRUE(memcmp(header, "RIFF", 4) == 0);
    EXPECT_TRUE(get_u32_le(header + 4) == 256036u);
    EXPECT_TRUE(memcmp(header + 8, "WAVEfmt ", 8) == 0);
    EXPECT_TRUE(get_u32_le(header + 16) == 16u);
    EXPECT_TRUE(get_u16_le(header + 20) == 1u);
    EXPECT_TRUE(get_u16_le(header + 22) == 1u);
    EXPECT_TRUE(get_u32_le(header + 24) == 16000u);
    EXPECT_TRUE(get_u32_le(header + 28) == 32000u);
    EXPECT_TRUE(get_u16_le(header + 32) == 2u);
    EXPECT_TRUE(get_u16_le(header + 34) == 16u);
    EXPECT_TRUE(memcmp(header + 36, "data", 4) == 0);
    EXPECT_TRUE(get_u32_le(header + 40) == 256000u);

    EXPECT_TRUE(asr_multipart_length(120u, 256044u, 24u) == 256188u);
    EXPECT_TRUE(asr_multipart_length(SIZE_MAX, 1u, 0u) == SIZE_MAX);
    EXPECT_TRUE(asr_multipart_length(SIZE_MAX - 2u, 1u, 2u) == SIZE_MAX);

    puts("asr_audio tests: PASS");
    return 0;
}
