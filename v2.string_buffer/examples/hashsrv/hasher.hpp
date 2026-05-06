#ifndef __HASHER_HPP__
#define __HASHER_HPP__

#include <openssl/evp.h>

class Hasher
{
public:
    Hasher();
    ~Hasher();

    void Reset();
    void Update(const void* data, size_t len);
    const char* FinalizeHex(size_t& outLen);

private:
    EVP_MD_CTX* mCtx { nullptr };
    char mHexBuffer[EVP_MAX_MD_SIZE * 2 + 1];
};

inline Hasher::Hasher()
{
    mCtx = EVP_MD_CTX_new();
    Reset();
}

inline Hasher::~Hasher()
{
    if(mCtx)
        EVP_MD_CTX_free(mCtx);
}

inline void Hasher::Reset()
{
    EVP_DigestInit_ex(mCtx, EVP_sha256(), nullptr);
}

inline void Hasher::Update(const void* data, size_t len)
{
    EVP_DigestUpdate(mCtx, data, len);
}

inline const char* Hasher::FinalizeHex(size_t& outLen)
{
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(mCtx, hash, &len);

    // Reset immediately so the object is ready for the next hash
    Reset();

    // Convert to a human-readable hexadecimal string
    static const char* lut = "0123456789abcdef"; // Lookup table for hex digits

    // Tight loop using bit-shifting (fastest way to split a byte)
    const uint8_t* data = static_cast<const uint8_t*>(hash);
    char* dest = mHexBuffer;
    for(unsigned int i = 0; i < len; ++i)
    {
        *dest++ = lut[data[i] >> 4];   // High nibble
        *dest++ = lut[data[i] & 0x0f]; // Low nibble
    }

    outLen = len * 2;
    mHexBuffer[outLen++] = '\n';
    return mHexBuffer;
}

#endif // __HASHER_HPP__
