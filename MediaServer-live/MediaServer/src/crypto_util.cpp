#include "crypto_util.h"
#include <openssl/sha.h>
#include <openssl/rand.h>

static std::string toHex(const unsigned char* data, int len)
{
    static const char hexChars[] = "0123456789abcdef";
    std::string hex;
    hex.resize(len * 2);
    for (int i = 0; i < len; ++i)
    {
        hex[i * 2]     = hexChars[(data[i] >> 4) & 0xF];
        hex[i * 2 + 1] = hexChars[data[i] & 0xF];
    }
    return hex;
}

std::string genSalt()
{
    unsigned char buf[16];
    RAND_bytes(buf, sizeof(buf));
    return toHex(buf, sizeof(buf));
}

std::string sha256Hex(const std::string& input)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()), input.size(), digest);
    return toHex(digest, SHA256_DIGEST_LENGTH);
}
