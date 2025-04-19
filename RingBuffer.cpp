#include "RingBuffer.h"

using namespace BA;

RingBuffer::RingBuffer (const size_t bufferSize,
                        const size_t readDistance)
: 
bufferSize (bufferSize),
readDistance (readDistance),
readIndex(0), 
writeIndex(0) 
{
    buffer.resize (bufferSize); // Resize the buffer vector to the capacity
    for (auto& block : buffer)  {
        block.setSize (128000, true); // Preallocate each block
    }
}

bool RingBuffer::push (const MemoryBlock& data)  
{
    size_t currentWriteIndex = writeIndex.load(std::memory_order_relaxed);
    size_t nextWriteIndex = (currentWriteIndex + 1) % bufferSize;

    // Ensure write does not catch up to the readIndex minus the readDistance
    size_t expectedReadIndex = (readIndex.load(std::memory_order_acquire) + bufferSize - readDistance) % bufferSize;
    if (nextWriteIndex != expectedReadIndex) {
        buffer[currentWriteIndex].replaceAll(data.getData(), data.getSize());
        writeIndex.store(nextWriteIndex, std::memory_order_release);
        return true;
    }
    return false; // Buffer full
}

bool RingBuffer::pop (juce::MemoryBlock& data) 
{
    size_t currentReadIndex = readIndex.load(std::memory_order_relaxed);
    size_t currentWriteIndex = writeIndex.load(std::memory_order_acquire);

    // Allow read if there is enough data written ahead of the current read index
    if (currentReadIndex != currentWriteIndex) {
        data = buffer[currentReadIndex];
        readIndex.store((currentReadIndex + 1) % bufferSize, std::memory_order_release);
        
        return true;
    }
    return false; // No data available to read
}