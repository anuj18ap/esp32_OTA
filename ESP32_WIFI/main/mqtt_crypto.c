#include "app.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "ota_protocol.h"
#include <stdlib.h>
#include <string.h>

static const uint32_t key[4] = {0xA6E31F4B, 0xC904B27D, 0x51D8A3E9, 0x7B2F90C5};
static void block_encrypt(uint32_t *v0, uint32_t *v1)
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < 32; i++)
    {
        *v0 += (((*v1 << 4) ^ (*v1 >> 5)) + *v1) ^ (sum + key[sum & 3]);
        sum += 0x9E3779B9u;
        *v1 += (((*v0 << 4) ^ (*v0 >> 5)) + *v0) ^ (sum + key[(sum >> 11) & 3]);
    }
}
static void crypt(uint8_t *data, size_t len, uint32_t hi, uint32_t lo)
{
    uint32_t counter = 0;
    for (size_t offset = 0; offset < len; offset += 8, counter++)
    {
        uint32_t a = hi ^ counter, b = lo + counter;
        uint8_t stream[8];
        block_encrypt(&a, &b);
        ota_write_be32(stream, a);
        ota_write_be32(stream + 4, b);
        for (size_t i = 0; i < 8 && offset + i < len; i++)
        {
            data[offset + i] ^= stream[i];
        }
    }
}
char *crypto_encrypt(const char *text)
{
    size_t len = strlen(text), raw_len = len + 8, capacity = 4 * ((raw_len + 2) / 3) + 1, used = 0;
    uint8_t *raw = malloc(raw_len);
    char *out = malloc(capacity + 5);
    if (!raw || !out)
    {
        free(raw);
        free(out);
        return NULL;
    }
    uint32_t hi = esp_random(), lo = esp_random();
    ota_write_be32(raw, hi);
    ota_write_be32(raw + 4, lo);
    memcpy(raw + 8, text, len);
    crypt(raw + 8, len, hi, lo);
    memcpy(out, "ENC1:", 5);
    int err = mbedtls_base64_encode((unsigned char *)out + 5, capacity, &used, raw, raw_len);
    free(raw);
    if (err)
    {
        free(out);
        return NULL;
    }
    out[5 + used] = 0;
    return out;
}
bool crypto_decrypt(const uint8_t *data, size_t len, char **text)
{
    *text = NULL;
    if (len < 5 || memcmp(data, "ENC1:", 5))
    {
        *text = malloc(len + 1);
        if (!*text)
        {
            return false;
        }
        memcpy(*text, data, len);
        (*text)[len] = 0;
        return true;
    }
    size_t capacity = (len - 5) * 3 / 4 + 3, used = 0;
    uint8_t *raw = malloc(capacity + 1);
    if (!raw)
    {
        return false;
    }
    if (mbedtls_base64_decode(raw, capacity, &used, data + 5, len - 5) || used < 8)
    {
        free(raw);
        return false;
    }
    crypt(raw + 8, used - 8, ota_read_be32(raw), ota_read_be32(raw + 4));
    memmove(raw, raw + 8, used - 8);
    raw[used - 8] = 0;
    *text = (char *)raw;
    return true;
}
