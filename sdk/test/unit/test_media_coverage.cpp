#include "../fixtures/RealMediaFixture.hpp"
#include <AudioBuffer.h>
#include <AudioCommon.hpp>
#include <AudioDecodeProcess.h>
#include <ExtractorFactory.h>
#include <ExtractorHelper.hpp>
#include <FileSource.h>
#include <cstdint>
#include <cstdio>
#include <doctest/doctest.h>
#include <memory>
#include <set>
#include <string>

// 解码覆盖度与时长正确性的统一基线（决策来源 #16，前置 #13/#15/#17/#18/#20/#21）。
//
// 判据是双向的：下界防截断（APE 只解首帧 = 18%），上界防重复（FLAC 重复解码 = 35×）。
// 只有下界会漏报后者——这正是 #13 普查发现的漏洞。
//
// 实测侧一律取「解码后的 spec」而不是提取器的 spec：mp3/ogg 的提取器不声明采样格式
// （bytesPerSample=0，会除零），aac/asf/m4a/mp3/wma 的解码输出是 fltp（4 B/采样，
// 按 2 B 换算会得到虚假的 2 倍——#19 的「mp3 2.03 倍」就是这么来的）。
TEST_SUITE("MediaCoverage")
{
    namespace
    {
        struct MediaCase {
            const char *fileName;
            const char *extension;
        };

        // 每个受支持扩展名至少一份素材；wav/aac/mp3 各两份，顺带覆盖不同采样率与码率档位。
        const MediaCase kMediaCases[] = {
            { "music.wav", ".wav" },
            { "ring.wav", ".wav" },
            { "48000_fltp_1.aac", ".aac" },
            { "WavinFlag.aac", ".aac" },
            { "music-ape.ape", ".ape" },
            { "music-mkv.mkv", ".mkv" },
            { "俱乐部音乐循环-Freesound.aiff", ".aiff" },
            { "小镇姑娘-陶喆.flac", ".flac" },
            { "像我这样的人-毛不易.128.mp3", ".mp3" },
            { "消愁-毛不易.320.mp3", ".mp3" },
            { "Ringtones-耳聆网.ogg", ".ogg" },
            { "摇滚乐_Freesound.m4a", ".m4a" },
            { "萨克斯机.asf", ".asf" },
            { "十七岁-陶喆.96.wma", ".wma" },
        };

        const size_t kMediaCaseCount = sizeof(kMediaCases) / sizeof(kMediaCases[0]);

        // 返回 false 表示这份素材没能完成验证（缺素材 / 打不开 / 初始化失败）。
        // 所有失败都用 CHECK 记录，让同一轮里其余素材继续跑完。
        bool checkCoverage(const RealMediaFixture &fixture, const MediaCase &item)
        {
            const std::string name(item.fileName);
            if (!fixture.exists(name)) {
                std::printf("[SKIP] %s (素材未随仓库下发)\n", name.c_str());
                return false;
            }

            std::shared_ptr<FileSource> source;
            std::unique_ptr<ExtractorHelper> extractor;
            if (!fixture.create(name, item.extension, source, extractor)
                || extractor == nullptr) {
                CHECK_MESSAGE(false, "create extractor failed: " << name);
                return false;
            }
            if (extractor->initCheck() != sdk_utils::OK) {
                CHECK_MESSAGE(false, "extractor init failed: " << name);
                return false;
            }

            const AudioSpec declared = extractor->getAudioSpec();
            // #15 契约：durationMs == 0 的语义是「未知」。受支持格式 + 随仓库下发的素材
            // 不允许未知——出现了就是推导逻辑退化，不是「这条素材特殊」。
            if (declared.durationMs == 0) {
                CHECK_MESSAGE(false, "durationMs unknown: " << name);
                return false;
            }

            AudioDecodeProcess process(extractor.get());
            if (process.initCheck() != sdk_utils::OK) {
                CHECK_MESSAGE(false, "decode init failed: " << name);
                return false;
            }

            AudioBuffer::AudioBufferPtr pcm = process.getDecodeBuffer();
            if (pcm == nullptr || pcm->size() == 0) {
                CHECK_MESSAGE(false, "no PCM decoded: " << name);
                return false;
            }

            const AudioSpec decoded = process.getDecodeSpec();
            if (decoded.sampleRate <= 0 || decoded.numChannel <= 0
                || decoded.bytesPerSample <= 0) {
                CHECK_MESSAGE(false, "decoded spec unusable: " << name);
                return false;
            }

            const uint64_t pcmBytes = static_cast<uint64_t>(pcm->size());
            const uint64_t frameBytes = static_cast<uint64_t>(decoded.numChannel)
                                        * static_cast<uint64_t>(decoded.bytesPerSample);
            CHECK_MESSAGE(pcmBytes % frameBytes == 0,
                          "PCM 长度不是整数帧，说明截断落在帧中间: " << name);

            const uint64_t measuredMs
                = pcmBytes / frameBytes * 1000ULL / static_cast<uint64_t>(decoded.sampleRate);
            const uint64_t tolerance = RealMediaFixture::coverageToleranceMs(declared.durationMs);
            const uint64_t lower
                = declared.durationMs > tolerance ? declared.durationMs - tolerance : 0;
            const uint64_t upper = declared.durationMs + tolerance;

            MESSAGE(name << ": declared " << declared.durationMs << "ms, measured " << measuredMs
                         << "ms, tolerance " << tolerance << "ms");

            CHECK_MESSAGE(measuredMs >= lower,
                          "解码覆盖不足（截断）: " << name << " measured " << measuredMs
                                                   << "ms < lower bound " << lower << "ms");
            CHECK_MESSAGE(measuredMs <= upper,
                          "解码超出标称（重复解码）: " << name << " measured "
                                                       << measuredMs << "ms > upper bound "
                                                       << upper << "ms");
            return true;
        }
    } // namespace

    // 结构性锁：受支持扩展名 ⇔ 有素材与覆盖度用例。
    // 这比白名单更强——#20 对「受支持」的定义就是「能解出与标称时长一致的 PCM，
    // 且被覆盖度断言覆盖」，所以新增格式不能只改注册表，必须同时补素材与这一行。
    TEST_CASE("every supported extension has a real-media coverage case")
    {
        std::set<std::string> covered;
        for (size_t i = 0; i < kMediaCaseCount; ++i) {
            covered.insert(kMediaCases[i].extension);
        }

        for (auto it = defaultExtractorMap.begin(); it != defaultExtractorMap.end(); ++it) {
            CHECK_MESSAGE(covered.count(it->first) == 1,
                          "受支持格式缺少覆盖度素材: " << it->first);
        }

        for (auto it = covered.begin(); it != covered.end(); ++it) {
            CHECK_MESSAGE(defaultExtractorMap.count(*it) == 1,
                          "覆盖度矩阵里含已摘除格式: " << *it);
        }
    }

    TEST_CASE("decoded PCM matches advertised duration")
    {
        RealMediaFixture fixture;
        size_t checked = 0;
        for (size_t i = 0; i < kMediaCaseCount; ++i) {
            INFO(std::string(kMediaCases[i].fileName));
            if (checkCoverage(fixture, kMediaCases[i])) {
                ++checked;
            }
        }

        MESSAGE("coverage matrix covered " << checked << "/" << kMediaCaseCount << " file(s)");
        CHECK(checked == kMediaCaseCount);
    }
}
