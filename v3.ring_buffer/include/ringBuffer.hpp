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
    explicit RingBuffer(size_t capacity = 64 * 1024)
        : mBuffer(capacity), mCapacity(capacity) {}

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
    void Write(const uint8_t* src, size_t len);
    // void CopyTo(uint8_t* dst, size_t len) const;

    uint8_t operator[](size_t index) const;

private:
    std::vector<uint8_t> mBuffer;
    size_t mCapacity{0};
    size_t mSize{0};
    size_t mHead{0};
    size_t mTail{0};
};

inline uint8_t RingBuffer::operator[](size_t index) const
{
    return mBuffer[(mHead + index) % mCapacity];
}

inline void RingBuffer::Consume(size_t len)
{
    size_t toConsume = std::min(len, mSize);
    mHead = (mHead + toConsume) % mCapacity;
    mSize -= toConsume;
}

// Returns 1 or 2 contiguous segments of memory representing the requested data
inline RingBuffer::BufferRegions RingBuffer::GetReadRegions(size_t offset, size_t len) const
{
    // Safety check: ensure the requested range is within available data
    if(len == 0 || (offset + len) > mSize)
        return BufferRegions{};

    size_t actualStart = (mHead + offset) % mCapacity;
    size_t firstPart = std::min(len, mCapacity - actualStart);

    // const_cast allows this to be stored in the void* for system calls
    BufferRegions rr;
    rr.regions[0] = {const_cast<uint8_t*>(&mBuffer[actualStart]), firstPart};
    rr.count = 1;

    if(len > firstPart)
    {
        rr.regions[1] = {const_cast<uint8_t*>(&mBuffer[0]), len - firstPart};
        rr.count = 2;
    }
    return rr;
}

inline RingBuffer::BufferRegions RingBuffer::GetWriteRegions()
{
    if(Full())
        return BufferRegions{};

    BufferRegions wr;
    if(mTail >= mHead)
    {
        // Region from Tail to the physical end of the vector
        wr.regions[0] = {&mBuffer[mTail], mCapacity - mTail};
        wr.count = 1;

        // Region from start of vector back to the Head (the wrap-around)
        // Only add if there's actually space and Head isn't 0
        if(mHead > 0)
        {
            wr.regions[1] = {&mBuffer[0], mHead};
            wr.count = 2;
        }
    }
    else
    {
        // Tail has already wrapped, so we only have one contiguous block
        // available until we hit the Head
        wr.regions[0] = {&mBuffer[mTail], mHead - mTail};
        wr.count = 1;
    }
    return wr;
}

inline void RingBuffer::CommitWrite(size_t len)
{
    mTail = (mTail + len) % mCapacity;
    mSize += len;
}

// inline void RingBuffer::CopyTo(uint8_t* dst, size_t len) const
// {
//     size_t toCopy = std::min(len, mSize);
//     size_t firstPart = std::min(toCopy, mCapacity - mHead);
//     std::memcpy(dst, &mBuffer[mHead], firstPart);
//     if(toCopy > firstPart)
//     {
//         std::memcpy(dst + firstPart, &mBuffer[0], toCopy - firstPart);
//     }
// }

inline void RingBuffer::Write(const uint8_t* src, size_t len)
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

} // namespace gen

#endif // __RING_BUFFER_HPP__