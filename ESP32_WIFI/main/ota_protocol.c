#include "ota_protocol.h"
#include <string.h>

uint32_t ota_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    while (len--)
    {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; bit++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    }
    return crc;
}
uint32_t ota_read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
void ota_write_be32(uint8_t *p, uint32_t n)
{
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
void ota_stream_init(ota_stream_t *s, uint32_t size, uint32_t chunks, uint32_t crc)
{
    *s = (ota_stream_t){.size = size, .chunks = chunks, .crc = UINT32_MAX, .expected_crc = crc};
}
stream_result_t ota_stream_check(const ota_stream_t *s, uint32_t index, const uint8_t *data,
                                 size_t len)
{
    if (!data || !len || len > UINT32_MAX)
    {
        return STREAM_LENGTH;
    }
    if (s->next && index == s->next - 1)
    {
        return len == s->last_length && ota_crc32(UINT32_MAX, data, len) == s->last_crc
                   ? STREAM_DUPLICATE
                   : STREAM_SEQUENCE;
    }
    if (index != s->next || index >= s->chunks)
    {
        return STREAM_SEQUENCE;
    }
    if (len > s->size - s->bytes)
    {
        return STREAM_LENGTH;
    }
    return STREAM_OK;
}
void ota_stream_commit(ota_stream_t *s, const uint8_t *data, size_t len)
{
    s->crc = ota_crc32(s->crc, data, len);
    s->last_crc = ota_crc32(UINT32_MAX, data, len);
    s->last_length = len;
    s->bytes += len;
    s->next++;
}
bool ota_stream_complete(const ota_stream_t *s)
{
    return s->next == s->chunks && s->bytes == s->size && (s->crc ^ UINT32_MAX) == s->expected_crc;
}

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static int nibble(char c)
{
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
bool parse_relay_mask(const uint8_t *data, size_t len, uint8_t *mask)
{
    if (!data || !mask || !len)
    {
        return false;
    }
    if (len == 1)
    {
        int n = nibble(data[0]);
        *mask = n >= 0 ? n : data[0];
        return true;
    }
    if (len == 4 && !data[0] && !data[1] && !data[2])
    {
        *mask = data[3];
        return true;
    }
    if (len >= 12)
    {
        return false;
    }
    char text[12] = {0};
    memcpy(text, data, len);
    char *p = text, *end = text + strlen(text);
    while (space(*p))
    {
        p++;
    }
    while (end > p && space(end[-1]))
    {
        *--end = 0;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
    }
    if (!*p)
    {
        return false;
    }
    unsigned value = 0;
    while (*p)
    {
        int n = nibble(*p++);
        if (n < 0)
        {
            return false;
        }
        value = (value << 4) | n;
        if (value > 255)
        {
            return false;
        }
    }
    *mask = value;
    return true;
}
