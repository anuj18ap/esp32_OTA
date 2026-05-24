#include <Arduino.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <string.h>

#include "FirmwareApp.h"

static const char* ENCRYPTION_PREFIX = "ENC1:";
static const uint32_t XTEA_KEY[4] = {
    0xA6E31F4Bu, 0xC904B27Du, 0x51D8A3E9u, 0x7B2F90C5u
};

static void xteaEncryptBlock(uint32_t block[2])
{
    uint32_t v0 = block[0];
    uint32_t v1 = block[1];
    uint32_t sum = 0;
    const uint32_t delta = 0x9E3779B9u;

    for (uint8_t i = 0; i < 32; i++)
    {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + XTEA_KEY[sum & 3]);
        sum += delta;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + XTEA_KEY[(sum >> 11) & 3]);
    }

    block[0] = v0;
    block[1] = v1;
}

static void put32(uint8_t* out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint32_t get32(const uint8_t* in)
{
    return ((uint32_t)in[0] << 24) |
           ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) |
           (uint32_t)in[3];
}

static void applyXteaCtr(uint8_t* data, size_t length, uint32_t nonceHi, uint32_t nonceLo)
{
    uint32_t counter = 0;
    uint8_t stream[8];

    for (size_t offset = 0; offset < length; offset += sizeof(stream))
    {
        uint32_t block[2] = {nonceHi ^ counter, nonceLo + counter};
        xteaEncryptBlock(block);
        put32(stream, block[0]);
        put32(stream + 4, block[1]);

        size_t remaining = length - offset;
        size_t n = remaining < sizeof(stream) ? remaining : sizeof(stream);
        for (size_t i = 0; i < n; i++)
            data[offset + i] ^= stream[i];

        counter++;
    }
}

String encryptMqttPayload(const String& plainText)
{
    size_t plainLen = plainText.length();
    size_t rawLen = 8 + plainLen;
    uint8_t* raw = new uint8_t[rawLen];
    if (!raw) return plainText;

    uint32_t nonceHi = esp_random();
    uint32_t nonceLo = esp_random();
    put32(raw, nonceHi);
    put32(raw + 4, nonceLo);

    if (plainLen > 0)
    {
        memcpy(raw + 8, plainText.c_str(), plainLen);
        applyXteaCtr(raw + 8, plainLen, nonceHi, nonceLo);
    }

    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, raw, rawLen);
    unsigned char* b64 = new unsigned char[b64Len + 1];
    if (!b64)
    {
        delete[] raw;
        return plainText;
    }

    if (mbedtls_base64_encode(b64, b64Len + 1, &b64Len, raw, rawLen) != 0)
    {
        delete[] b64;
        delete[] raw;
        return plainText;
    }

    b64[b64Len] = '\0';
    String encrypted = ENCRYPTION_PREFIX;
    encrypted += (const char*)b64;

    delete[] b64;
    delete[] raw;
    return encrypted;
}

bool decryptMqttPayload(const byte* payload, unsigned int length, String& plainText)
{
    String input;
    input.reserve(length);
    for (unsigned int i = 0; i < length; i++)
        input += (char)payload[i];

    if (!input.startsWith(ENCRYPTION_PREFIX))
    {
        plainText = input;
        return true;
    }

    const char* b64 = input.c_str() + strlen(ENCRYPTION_PREFIX);
    size_t b64Len = strlen(b64);
    size_t rawLen = 0;
    mbedtls_base64_decode(nullptr, 0, &rawLen, (const unsigned char*)b64, b64Len);
    if (rawLen < 8) return false;

    uint8_t* raw = new uint8_t[rawLen];
    if (!raw) return false;

    if (mbedtls_base64_decode(raw, rawLen, &rawLen,
                              (const unsigned char*)b64, b64Len) != 0 ||
        rawLen < 8)
    {
        delete[] raw;
        return false;
    }

    uint32_t nonceHi = get32(raw);
    uint32_t nonceLo = get32(raw + 4);
    size_t cipherLen = rawLen - 8;
    applyXteaCtr(raw + 8, cipherLen, nonceHi, nonceLo);

    plainText = "";
    plainText.reserve(cipherLen);
    for (size_t i = 0; i < cipherLen; i++)
        plainText += (char)raw[8 + i];

    delete[] raw;
    return true;
}
