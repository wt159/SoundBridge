#include "FLACDecode.h"
#include "LogWrapper.h"
#include <algorithm>
#include <cstring>

#define LOG_TAG "FLACDecode"

FLACDecode::FLACDecode(AudioDecodeCallback *callback)
    : FLAC::Decoder::Stream()
    , m_callback(callback)
    , m_inBuf(nullptr)
    , m_dataSource(nullptr)
    , m_audioDataOffset(0)
    , m_dataSourceSize(0)
    , m_decSpec()
    , m_decOffset(0)
    , m_abortFlag(nullptr)
    , m_md5CheckResult(-1)
    , m_extractor(nullptr)
    , m_observer(nullptr)
    , m_cursor()
    , m_packet()
    , m_packetOffset(0)
{
    // 必须在 init() 之前打开：libFLAC 的 FLAC__stream_decoder_set_md5_checking()
    // 在非 UNINITIALIZED 状态直接返回 false，init() 之后调用是一次静默空操作
    // （init 里 do_md5_checking 已按 protected_->md5_checking 定死）。这就是 #10 的
    // 「CRC 只进日志」的第二层原因：MD5 根本没开过。
    this->set_md5_checking(true);
    ::FLAC__StreamDecoderInitStatus init_status = this->init();
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        LOGE("initializing decoder: %s", FLAC__StreamDecoderInitStatusString[init_status]);
    }
}

FLACDecode::~FLACDecode()
{
    if (m_decSpec.lineData != nullptr) {
        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
            delete[] m_decSpec.lineData[ch];
            m_decSpec.lineData[ch] = nullptr;
        }
        delete[] m_decSpec.lineData;
        m_decSpec.lineData = nullptr;
    }
    if (m_decSpec.lineSize != nullptr) {
        delete[] m_decSpec.lineSize;
        m_decSpec.lineSize = nullptr;
    }
}

int FLACDecode::decode(const char *data, ssize_t size)
{
    m_inBuf     = std::make_shared<AudioBuffer>(size);
    m_decOffset = 0;
    memcpy(m_inBuf->data(), data, size);
    return this->process_until_end_of_stream() == true ? 0 : -1;
}

int FLACDecode::decode(AudioBufferPtr &inBuf)
{
    setInputBuffer(inBuf);
    return this->process_until_end_of_stream() == true ? 0 : -1;
}

bool FLACDecode::initFromDataSource(DataSourceBase *src, off64_t audioOffset, off64_t dataSize)
{
    m_dataSource      = src;
    m_audioDataOffset = audioOffset;
    m_dataSourceSize  = dataSize - audioOffset;
    m_decOffset       = 0;
    m_inBuf           = nullptr;
    return true;
}

void FLACDecode::setInputBuffer(const AudioBufferPtr &inBuf)
{
    m_inBuf           = inBuf;
    m_decOffset       = 0;
    m_dataSource      = nullptr;
    m_audioDataOffset = 0;
    m_dataSourceSize  = 0;
    m_cursor.reset();
    m_extractor = nullptr;
    releasePacket();
}

bool FLACDecode::initFromPacketFactory(ExtractorHelper *extractor, DecodeSupplyObserver *observer)
{
    if (extractor == nullptr) {
        return false;
    }
    m_extractor      = extractor;
    m_observer       = observer;
    m_cursor.reset(new PacketCursor(extractor));
    // 新任务从流头开始：包序列可能还停在上一任务的位置（提取器跨任务存活）。
    extractor->seekPacketByte(0);
    releasePacket();
    m_inBuf           = nullptr;
    m_dataSource      = nullptr;
    m_audioDataOffset = 0;
    m_dataSourceSize  = extractor->getDataSize();
    m_decOffset       = 0;
    return true;
}

void FLACDecode::releasePacket()
{
    if (m_packet != nullptr) {
        if (m_observer != nullptr) {
            m_observer->onPacketReleased(m_packet->size());
        }
        m_packet.reset();
    }
    m_packetOffset = 0;
}

off64_t FLACDecode::packetPosition() const
{
    if (m_extractor == nullptr) {
        return 0;
    }
    off64_t position = m_extractor->packetBytePosition();
    if (m_packet != nullptr) {
        position -= static_cast<off64_t>(m_packet->size() - m_packetOffset);
    }
    return position < 0 ? 0 : position;
}

int FLACDecode::processOne()
{
    if (m_inBuf == nullptr && m_dataSource == nullptr && !m_cursor) {
        return -1;
    }

    if (this->get_state() == FLAC__STREAM_DECODER_END_OF_STREAM) {
        return 0;
    }

    bool ret = this->process_single();
    if (!ret) {
        return (this->get_state() == FLAC__STREAM_DECODER_END_OF_STREAM) ? 0 : -1;
    }
    return (this->get_state() == FLAC__STREAM_DECODER_END_OF_STREAM) ? 0 : 1;
}

bool FLACDecode::seekToSample(FLAC__uint64 sample)
{
    if (m_inBuf == nullptr && m_dataSource == nullptr && !m_cursor) {
        return false;
    }
    return this->seek_absolute(sample);
}

bool FLACDecode::md5MismatchAtEndOfStream()
{
    // 整段校验的唯一出口：libFLAC 把 STREAMINFO 里声明的 MD5 与实际解出的 PCM
    // 的比对放在 finish() 里（process_single() 的返回值只说明「这一帧解出来了」，
    // LOST_SYNC 之后它照样 resync 继续，样本数可以一个不差）。
    // 因此帧级信号分不清「跳了帧」还是「解了垃圾」，这里必须问 MD5。
    if (m_md5CheckResult < 0) {
        if (!is_valid()) {
            m_md5CheckResult = 0;
        } else {
            // finish() 返回 false 当且仅当 MD5 不符（其它失败在 process 阶段就已返回 false）
            m_md5CheckResult = finish() ? 0 : 1;
            if (m_md5CheckResult != 0) {
                LOGE("stream md5 mismatch");
            }
        }
    }
    return m_md5CheckResult == 1;
}

void FLACDecode::setAbortFlag(std::atomic<bool> *flag)
{
    m_abortFlag = flag;
}

::FLAC__StreamDecoderReadStatus FLACDecode::read_callback(FLAC__byte buffer[], size_t *bytes)
{
    if (m_cursor) {
        return readFromPackets(buffer, bytes);
    }

    if (m_dataSource != nullptr) {
        off64_t remaining = m_dataSourceSize - static_cast<off64_t>(m_decOffset);
        if (remaining <= 0) {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        if (static_cast<off64_t>(*bytes) > remaining) {
            *bytes = static_cast<size_t>(remaining);
        }
        ssize_t got = m_dataSource->readAt(m_audioDataOffset + static_cast<off64_t>(m_decOffset),
                                           buffer, *bytes);
        if (got <= 0) {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        *bytes       = static_cast<size_t>(got);
        m_decOffset += *bytes;
        return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    }

    if (m_inBuf == nullptr) {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

    ::FLAC__StreamDecoderReadStatus status = ::FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    if (*bytes > 0) {
        size_t size = m_inBuf->size() - m_decOffset;
        if (*bytes > size)
            *bytes = size;
        memcpy(buffer, m_inBuf->data() + m_decOffset, *bytes);
        m_decOffset += *bytes;
        if (*bytes == 0)
            status = FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        else
            status = FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
    } else
        status = FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    return status;
}

::FLAC__StreamDecoderReadStatus FLACDecode::readFromPackets(FLAC__byte buffer[], size_t *bytes)
{
    if (buffer == nullptr || bytes == nullptr || *bytes == 0) {
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

    // 一包读完就放掉，再拉下一包：同一时刻只为一首曲目持有一包压缩数据。
    while (m_packet == nullptr || m_packetOffset >= m_packet->size()) {
        // 取消链要在**拉包之前**被看到：m_abortDecode 只由 stop 置位，这里立刻退出，
        // 不让 libFLAC 为了「再解一帧」把包工厂继续往下推。
        // seek 刻意不置该位（AudioStreamDecoder::seekToMs）：seek 的取消由目标槽承载，
        // 在解码器侧的写 ring 循环里中断；native RA 定位期间这里必须能正常读流，
        // 否则 READ_STATUS_ABORT 会让 seek_absolute 失败（#65 的非确定性根因）。
        if (m_abortFlag != nullptr && m_abortFlag->load()) {
            return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
        }
        AudioBuffer::AudioBufferPtr next;
        if (!m_cursor->nextPacket(next)) {
            *bytes = 0;
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        releasePacket();
        m_packet       = next;
        m_packetOffset = 0;
        if (m_observer != nullptr) {
            m_observer->onPacketAcquired(m_packet->size());
            m_observer->onPacketPulled(m_cursor->packetsProduced());
        }
    }

    const size_t available = m_packet->size() - m_packetOffset;
    const size_t count     = std::min(*bytes, available);
    std::memcpy(buffer, m_packet->data() + m_packetOffset, count);
    m_packetOffset += count;
    *bytes = count;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

::FLAC__StreamDecoderSeekStatus FLACDecode::seek_callback(FLAC__uint64 absolute_byte_offset)
{
    if (m_cursor) {
        // native random access 仍然可用：把「定位到某字节」交给包工厂的 O(1) 游标，
        // 下一包从该处重新认帧头，libFLAC 自己 resync。取消/停止不在这里判断，
        // 由 read_callback 在拉包前看到取消位后立刻返回 ABORT。
        if (m_extractor == nullptr
            || !m_extractor->seekPacketByte(static_cast<off64_t>(absolute_byte_offset))) {
            return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
        }
        m_cursor->rewind();
        releasePacket();
        m_decOffset = static_cast<size_t>(absolute_byte_offset);
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }

    if (m_dataSource != nullptr) {
        if (absolute_byte_offset > static_cast<FLAC__uint64>(m_dataSourceSize)) {
            return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
        }
        m_decOffset = static_cast<size_t>(absolute_byte_offset);
        return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
    }

    if (m_inBuf == nullptr) {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    if (absolute_byte_offset > static_cast<FLAC__uint64>(m_inBuf->size())) {
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    }
    m_decOffset = static_cast<size_t>(absolute_byte_offset);
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

::FLAC__StreamDecoderTellStatus FLACDecode::tell_callback(FLAC__uint64 *absolute_byte_offset)
{
    if (absolute_byte_offset == nullptr) {
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }

    if (m_cursor) {
        *absolute_byte_offset = static_cast<FLAC__uint64>(packetPosition());
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }

    if (m_dataSource != nullptr) {
        *absolute_byte_offset = static_cast<FLAC__uint64>(m_decOffset);
        return FLAC__STREAM_DECODER_TELL_STATUS_OK;
    }

    if (m_inBuf == nullptr) {
        return FLAC__STREAM_DECODER_TELL_STATUS_ERROR;
    }

    *absolute_byte_offset = static_cast<FLAC__uint64>(m_decOffset);
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

::FLAC__StreamDecoderLengthStatus FLACDecode::length_callback(FLAC__uint64 *stream_length)
{
    if (stream_length == nullptr) {
        return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }

    if (m_cursor) {
        *stream_length = static_cast<FLAC__uint64>(m_dataSourceSize);
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }

    if (m_dataSource != nullptr) {
        *stream_length = static_cast<FLAC__uint64>(m_dataSourceSize);
        return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
    }

    if (m_inBuf == nullptr) {
        return FLAC__STREAM_DECODER_LENGTH_STATUS_ERROR;
    }

    *stream_length = static_cast<FLAC__uint64>(m_inBuf->size());
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

bool FLACDecode::eof_callback()
{
    if (m_cursor) {
        return packetPosition() >= m_dataSourceSize;
    }

    if (m_dataSource != nullptr) {
        return m_decOffset >= static_cast<size_t>(m_dataSourceSize);
    }

    if (m_inBuf == nullptr) {
        return true;
    }
    return m_decOffset >= m_inBuf->size();
}

::FLAC__StreamDecoderWriteStatus FLACDecode::write_callback(const ::FLAC__Frame *frame,
                                                            const FLAC__int32 *const buffer[])
{
    if (m_abortFlag != nullptr && m_abortFlag->load()) {
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }

    ::FLAC__StreamDecoderWriteStatus status = ::FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    // 首个被写出的帧不一定是 sample_number == 0：喂整段流（为了 STREAMINFO/MD5）之后，
    // 丢帧/插空帧都可能让第一个 write_callback 从中间开始。所以初始化条件看缓冲是否就绪，
    // 而不是看帧序号——旧写法在这里会解引用空指针。
    if (m_decSpec.lineData == nullptr
        || m_decSpec.spec.numChannel != static_cast<int>(this->get_channels())
        || m_decSpec.spec.bitsPerSample != static_cast<int>(this->get_bits_per_sample())) {
        if (m_decSpec.lineData != nullptr) {
            for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
                delete[] m_decSpec.lineData[ch];
            }
            delete[] m_decSpec.lineData;
            m_decSpec.lineData = nullptr;
        }
        if (m_decSpec.lineSize != nullptr) {
            delete[] m_decSpec.lineSize;
            m_decSpec.lineSize = nullptr;
        }
        m_decSpec.spec.numChannel     = static_cast<int>(this->get_channels());
        m_decSpec.spec.bitsPerSample  = static_cast<int>(this->get_bits_per_sample());
        m_decSpec.spec.bytesPerSample = m_decSpec.spec.bitsPerSample >> 3;
        m_decSpec.spec.format         = getAudioFormatByBitPreSample(m_decSpec.spec.bitsPerSample);
        m_decSpec.spec.sampleRate     = static_cast<int>(this->get_sample_rate());
        m_decSpec.spec.samples        = 0;
        m_decSpec.lineData            = new uint8_t *[m_decSpec.spec.numChannel];
        m_decSpec.lineSize            = new int[m_decSpec.spec.numChannel];
        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
            m_decSpec.lineData[ch] = nullptr;
            m_decSpec.lineSize[ch] = 0;
        }
        LOGI("numChannel: %d, bitsPerSample: %d, bytesPerSample: %d, sampleRate: %d",
             m_decSpec.spec.numChannel, m_decSpec.spec.bitsPerSample, m_decSpec.spec.bytesPerSample,
             m_decSpec.spec.sampleRate);
    }
    if (m_decSpec.spec.samples != frame->header.blocksize
        || m_decSpec.lineData[0] == nullptr) {
        m_decSpec.spec.samples = frame->header.blocksize;
        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
            delete[] m_decSpec.lineData[ch];
            m_decSpec.lineSize[ch] = static_cast<int>(m_decSpec.spec.samples)
                * m_decSpec.spec.bytesPerSample;
            m_decSpec.lineData[ch] = new uint8_t[m_decSpec.lineSize[ch]];
        }
    }
    for (uint32_t i = 0; i < frame->header.blocksize; i++) {
        for (int ch = 0; ch < m_decSpec.spec.numChannel; ch++) {
            memcpy(m_decSpec.lineData[ch] + i * m_decSpec.spec.bytesPerSample, buffer[ch] + i,
                   m_decSpec.spec.bytesPerSample);
        }
    }
    m_callback->onAudioDecodeCallback(m_decSpec);
    return status;
}

void FLACDecode::metadata_callback(const ::FLAC__StreamMetadata *metadata)
{
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        m_decSpec.spec.numChannel     = metadata->data.stream_info.channels;
        m_decSpec.spec.bitsPerSample  = metadata->data.stream_info.bits_per_sample;
        m_decSpec.spec.bytesPerSample = metadata->data.stream_info.bits_per_sample >> 3;
        m_decSpec.spec.sampleRate     = metadata->data.stream_info.sample_rate;
        m_decSpec.spec.samples        = metadata->data.stream_info.max_blocksize;
        LOGI("numChannel: %d, bitsPerSample: %d, bytesPerSample: %d, sampleRate: %d, samples: %llu",
             m_decSpec.spec.numChannel, m_decSpec.spec.bitsPerSample, m_decSpec.spec.bytesPerSample,
             m_decSpec.spec.sampleRate, m_decSpec.spec.samples);
    }
}

void FLACDecode::error_callback(::FLAC__StreamDecoderErrorStatus status)
{
    LOGE("error_callback: %s", FLAC__StreamDecoderErrorStatusString[status]);
}
