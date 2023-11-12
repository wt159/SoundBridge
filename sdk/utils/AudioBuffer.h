#pragma once
#include <memory>

class AudioBuffer {
private:
    char *m_data;
    size_t m_size;

public:
    using AudioBufferPtr = std::shared_ptr<AudioBuffer>;

public:
    AudioBuffer() = delete;
    AudioBuffer(size_t size);
    AudioBuffer(char *buffer, size_t size);
    ~AudioBuffer();
    char *data() { return m_data;}
    size_t size() const { return m_size; }
    void setData(off64_t offset, size_t len, char *buffer);
    void getData(off64_t offset, size_t len, char *buffer);
};

using AudioBufferPtr = std::shared_ptr<AudioBuffer>;
