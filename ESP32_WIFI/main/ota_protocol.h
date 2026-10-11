#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
    uint32_t size, chunks, next, bytes, crc, expected_crc;
    uint32_t last_length, last_crc;
} ota_stream_t;
typedef enum
{
    STREAM_OK,
    STREAM_DUPLICATE,
    STREAM_SEQUENCE,
    STREAM_LENGTH
} stream_result_t;
uint32_t ota_crc32(uint32_t crc, const uint8_t *data, size_t len);
void ota_stream_init(ota_stream_t *s, uint32_t size, uint32_t chunks, uint32_t crc);
stream_result_t ota_stream_check(const ota_stream_t *s, uint32_t index, const uint8_t *data,
                                 size_t len);
void ota_stream_commit(ota_stream_t *s, const uint8_t *data, size_t len);
bool ota_stream_complete(const ota_stream_t *s);
bool parse_relay_mask(const uint8_t *data, size_t len, uint8_t *mask);
uint32_t ota_read_be32(const uint8_t *p);
void ota_write_be32(uint8_t *p, uint32_t n);
