#pragma once

#include "JuceIncludes.h"

namespace BA
{

    class RingBuffer 
    {
    public:
        RingBuffer(const size_t bufferSize = 12,
                const size_t readDistance = 6);

        ~RingBuffer() = default;

        bool push (const MemoryBlock& data);
        bool pop  (MemoryBlock& data);

    private:
        const size_t bufferSize; // Total number of slots in the buffer
        const size_t readDistance; // Safe distance between read and write indices
        std::vector<MemoryBlock> buffer;
        std::atomic<size_t> readIndex;
        std::atomic<size_t> writeIndex;

        // Calculate the safe index, ensuring circular buffer correctness
        size_t safeIndex(size_t index) const {
            return index % bufferSize;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RingBuffer)
    };

}; // namespace BA