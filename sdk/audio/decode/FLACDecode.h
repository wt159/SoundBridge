#pragma once
#include "AudioBuffer.h"
#include "AudioCommon.hpp"
#include "AudioDecode.h"
#include "DataSource.hpp"
#include "DecodeSupplyObserver.h"
#include "ExtractorHelper.hpp"
#include <FLAC++/decoder.h>
#include <atomic>
#include <memory>

class FLACDecode : public FLAC::Decoder::Stream {
private:
    AudioDecodeCallback *m_callback;
    AudioBufferPtr m_inBuf;
    DataSourceBase *m_dataSource;
    off64_t m_audioDataOffset;
    off64_t m_dataSourceSize;
    AudioDecodeSpec m_decSpec;
    size_t m_decOffset;
    std::atomic<bool> *m_abortFlag;
    // finish() 里的整段 MD5 比对结果：-1 未做，0 相符，1 不符
    int m_md5CheckResult;
    // 单包供给（#60 / ADR 0003）：demux 在解码线程里、播放期创建，逐包 pull；
    // 同一时刻只持一包，读完就放（解引用即释放，不等下一次拉包）。
    ExtractorHelper *m_extractor;
    DecodeSupplyObserver *m_observer;
    std::unique_ptr<PacketCursor> m_cursor;
    AudioBufferPtr m_packet;
    size_t m_packetOffset;

public:
    FLACDecode(AudioDecodeCallback *callback);
    ~FLACDecode();
    int decode(const char *data, ssize_t size);
    int decode(AudioBufferPtr &inBuf);
    bool initFromDataSource(DataSourceBase *src, off64_t audioOffset, off64_t dataSize);
    // 旧全量形式（expand 阶段保留，供整块视图与 batch 回退用）。
    void setInputBuffer(const AudioBufferPtr &inBuf);
    // 增量形式：字节由包工厂逐包交出来，本对象只负责把 libFLAC 的读写回调接到游标上。
    bool initFromPacketFactory(ExtractorHelper *extractor, DecodeSupplyObserver *observer);
    int processOne();
    bool seekToSample(FLAC__uint64 sample);
    // EOS 处调用：返回 true 表示整段 PCM 与 STREAMINFO 声明的 MD5 不符（规格 4.3 / 条目 10）。
    // 只问 MD5，不看 error_callback 的调用次数——后者分不清「跳了帧」还是「解了垃圾」。
    bool md5MismatchAtEndOfStream();
    void setAbortFlag(std::atomic<bool> *flag);

protected:
    virtual ::FLAC__StreamDecoderReadStatus read_callback(FLAC__byte buffer[], size_t *bytes);
    virtual ::FLAC__StreamDecoderSeekStatus seek_callback(FLAC__uint64 absolute_byte_offset);
    virtual ::FLAC__StreamDecoderTellStatus tell_callback(FLAC__uint64 *absolute_byte_offset);
    virtual ::FLAC__StreamDecoderLengthStatus length_callback(FLAC__uint64 *stream_length);
    virtual bool eof_callback();
    virtual ::FLAC__StreamDecoderWriteStatus write_callback(const ::FLAC__Frame *frame,
                                                            const FLAC__int32 *const buffer[]);
    virtual void metadata_callback(const ::FLAC__StreamMetadata *metadata);
    virtual void error_callback(::FLAC__StreamDecoderErrorStatus status);

private:
    // 从包工厂拉字节：当前包读尽就拉下一包；序列耗尽返回 END_OF_STREAM。
    ::FLAC__StreamDecoderReadStatus readFromPackets(FLAC__byte buffer[], size_t *bytes);
    void releasePacket();
    // 逻辑读位置 = 已交出包的末端 - 当前包内还没读走的字节。
    int64_t packetPosition() const;
};
