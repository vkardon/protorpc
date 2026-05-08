#ifndef __RING_BUFFER_HPP__
#define __RING_BUFFER_HPP__

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sys/uio.h>
#include <vector>
#include <cassert>

namespace gen {

class RingBuffer
{
public:
    explicit RingBuffer(size_t capacity = 64 * 1024);
    ~RingBuffer() = default;

    struct BufferRegion
    {
        void* ptr{nullptr};
        size_t len{0};
    };

    struct BufferRegions
    {
        BufferRegion regions[2];
        int count{0};
    };

    size_t Size() const { return mSize; }
    size_t Capacity() const { return mCapacity; }
    size_t FreeSpace() const { return mCapacity - mSize; }
    bool Full() const { return mSize == mCapacity; }
    bool Empty() const { return mSize == 0; }

    // Returns 1 or 2 contiguous segments of memory representing the requested data
    BufferRegions GetReadRegions(size_t offset, size_t len) const;
    BufferRegions GetWriteRegions();

    void Consume(size_t len);
    void CommitWrite(size_t len);
    void Write(const unsigned char* src, size_t len);

    // Copies 'len' bytes into 'dst' WITHOUT removing them from the buffer.
    // This handles the wrap-around logic internally.
    bool Peek(void* dst, size_t len) const;

    // Copies 'len' bytes into 'dst' and consumes them.
    // This is the high-performance workhorse for strings and arrays.
    inline bool Read(void* dst, size_t len);

    // Reads a POD(Plain Old Data) type from the buffer.
    // Handles wrap - around and consumption automatically.
    template <typename T>
    bool Read(T& val);

    // Copies 'len' bytes into a contiguous container (std::string or std::vector).
    // Automatically handles resizing and memory management.
    template <typename Container>
    bool Read(Container& cont, size_t len);

    // Searches for a specific byte in the buffer.
    // Returns the index relative to Head, or -1 if not found.
    ssize_t Find(unsigned char target) const;

    unsigned char operator[](size_t index) const;

private:
    size_t RoundUpToPowerOfTwo(size_t val);

    std::vector<unsigned char> mBuffer;
    size_t mCapacity{0};
    size_t mMask{0};
    size_t mSize{0};
    size_t mHead{0};
    size_t mTail{0};
};

inline RingBuffer::RingBuffer(size_t capacity /*= 64 * 1024*/)
{
    // Note: mCapacity is enforced as a power of two.
    // This allows us to use bitwise masking (index & mMask) for
    // wrap-around logic instead of integer division (index % mCapacity).
    //
    // Performance:
    // - Integer division/modulo: ~15-40 CPU cycles
    // - Bitwise AND: 1 CPU cycle
    //
    // This optimization is critical for high-throughput systems where
    // the buffer is accessed in hot paths.

    // Round up to nearest power of two
    mCapacity = RoundUpToPowerOfTwo(capacity);
    mMask = mCapacity - 1;

    mBuffer.resize(mCapacity);

    // Runtime check: verify our logic worked
    assert(mCapacity > 0 && (mCapacity & (mCapacity - 1)) == 0);
}

inline unsigned char RingBuffer::operator[](size_t index) const
{
    return mBuffer[(mHead + index) & mMask];
}

inline void RingBuffer::Consume(size_t len)
{
    size_t toConsume = std::min(len, mSize);
    mHead = (mHead + toConsume) & mMask; // Fast wrap-around
    mSize -= toConsume;
}

// Returns 1 or 2 contiguous segments of memory representing the requested data
inline RingBuffer::BufferRegions RingBuffer::GetReadRegions(size_t offset, size_t len) const
{
    // Safety check: ensure the requested range is within available data
    if(len == 0 || (offset + len) > mSize)
        return BufferRegions{};

    size_t actualStart = (mHead + offset) & mMask;
    size_t firstPart = std::min(len, mCapacity - actualStart);

    // const_cast allows this to be stored in the void* for system calls
    BufferRegions rr;
    rr.regions[0] = {const_cast<unsigned char*>(&mBuffer[actualStart]), firstPart};
    rr.count = 1;

    if(len > firstPart)
    {
        rr.regions[1] = {const_cast<unsigned char*>(&mBuffer[0]), len - firstPart};
        rr.count = 2;
    }
    return rr;
}

inline RingBuffer::BufferRegions RingBuffer::GetWriteRegions()
{
    size_t freeSpace = FreeSpace();
    if(freeSpace == 0)
        return BufferRegions{};

    BufferRegions wr;
    // Length available from Tail to the end of the physical array
    size_t firstPart = std::min(freeSpace, mCapacity - mTail);

    wr.regions[0] = {&mBuffer[mTail], firstPart};
    wr.count = 1;

    if(freeSpace > firstPart)
    {
        // Wrap around: only the remaining free space
        wr.regions[1] = {&mBuffer[0], freeSpace - firstPart};
        wr.count = 2;
    }
    return wr;
}

inline void RingBuffer::CommitWrite(size_t len)
{
    // Never move tail further than the actual free space
    size_t actualLen = std::min(len, FreeSpace());
    mTail = (mTail + actualLen) & mMask;
    mSize += actualLen;
}

inline void RingBuffer::Write(const unsigned char* src, size_t len)
{
    // TODO: Safety check: ensure we don't overflow
    assert(FreeSpace() >= len);

    auto wr = GetWriteRegions();
    size_t bytesCopied = 0;

    for(int i = 0; i < wr.count && bytesCopied < len; ++i)
    {
        size_t toCopy = std::min(wr.regions[i].len, len - bytesCopied);
        std::memcpy(wr.regions[i].ptr, src + bytesCopied, toCopy);
        bytesCopied += toCopy;
    }

    CommitWrite(len);
}

inline size_t RingBuffer::RoundUpToPowerOfTwo(size_t val)
{
    if(val <= 1)
        return 1;

    // __builtin_clzl (Count Leading Zeros) is a single CPU instruction.
    // For a 64-bit size_t, 64 minus leading zeros gives the position
    // of the highest bit. We subtract 1 from val so that if val is already
    // a power of two, we don't jump to the next one.
    return 1UL << ((sizeof(size_t) * 8) - __builtin_clzl(val - 1));
}

// Reads a POD(Plain Old Data) type from the buffer.
// Handles wrap - around and consumption automatically.
template <typename T>
inline bool RingBuffer::Read(T& val)
{
    static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable for Read");
    return Read(reinterpret_cast<void*>(&val), sizeof(T));
}

// Copies 'len' bytes into a contiguous container (std::string or std::vector).
// Automatically handles resizing and memory management.
template <typename Container>
inline bool RingBuffer::Read(Container& cont, size_t len)
{
    if(len == 0 || len > mSize)
        return false;

    // Set the container size to the expected length
    cont.resize(len);

    // Use the raw pointer Read to fill the container's memory
    if(!Read(static_cast<void*>(cont.data()), len))
    {
        cont.clear();
        return false;
    }

    return true;
}

// Copies 'len' bytes into 'dst' WITHOUT removing them from the buffer.
// This handles the wrap-around logic internally.
inline bool RingBuffer::Peek(void* dst, size_t len) const
{
    if(len == 0 || len > mSize)
        return false;

    unsigned char* d = static_cast<unsigned char*>(dst);

    // Calculate how much is available in the first contiguous stretch
    size_t firstPart = std::min(len, mCapacity - mHead);

    // Copy the first (or only) part
    std::memcpy(d, &mBuffer[mHead], firstPart);

    // If we wrapped around, copy the remaining part from the start of the buffer
    if(len > firstPart)
    {
        std::memcpy(d + firstPart, &mBuffer[0], len - firstPart);
    }

    return true;
}

// Copies 'len' bytes into 'dst' and removes them from the buffer.
// This is the high-performance workhorse for strings and arrays.
inline bool RingBuffer::Read(void* dst, size_t len)
{
    if(Peek(dst, len))
    {
        Consume(len);
        return true;
    }
    return false;
}

// Searches for a specific byte in the buffer.
// Returns the index relative to Head, or -1 if not found.
inline ssize_t RingBuffer::Find(unsigned char target) const
{
    // Check first contiguous part
    size_t firstPartLen = std::min(mSize, mCapacity - mHead);
    const unsigned char* firstPartPtr = &mBuffer[mHead];

    // Note: memchr is significantly faster than a manual loop
    const void* res = std::memchr(firstPartPtr, target, firstPartLen);
    if(res)
        return static_cast<const unsigned char*>(res) - firstPartPtr;

    // Check second (wrapped) part
    if(mSize > firstPartLen)
    {
        size_t secondPartLen = mSize - firstPartLen;
        const unsigned char* secondPartPtr = &mBuffer[0];
        res = std::memchr(secondPartPtr, target, secondPartLen);
        if(res)
            return static_cast<const unsigned char*>(res) - secondPartPtr + firstPartLen;
    }

    return -1;
}

} // namespace gen

#endif // __RING_BUFFER_HPP__