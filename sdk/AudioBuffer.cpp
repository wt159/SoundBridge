#include "AudioBuffer.h"
#include <cstring>

AudioBuffer::AudioBuffer(size_t size)
    : m_data(nullptr)
    , m_size(size)
{
    m_data = new char[size];
}

AudioBuffer::AudioBuffer(char *buffer, size_t size)
    : m_data(buffer)
    , m_size(size)
{
}

AudioBuffer::~AudioBuffer()
{
    if (m_data != nullptr)
        delete[] m_data;
    m_data = nullptr;
    m_size = 0;
}

void AudioBuffer::setData(off64_t offset, size_t len, char *buffer)
{
    size_t available = m_size - offset;
    if (available >= len) {
        memcpy(m_data + offset, buffer, len);
        return;
    }
    memcpy(m_data + offset, buffer, available);
    return;
}

void AudioBuffer::getData(off64_t offset, size_t len, char *buffer)
{
    size_t available = m_size - offset;
    if (available >= len) {
        memcpy(buffer, m_data + offset, len);
        return;
    }
    memset(buffer, 0, len);
    memcpy(buffer, m_data + offset, available);
    return;
}
