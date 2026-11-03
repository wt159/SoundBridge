#pragma once

#include <ExtractorFactory.h>
#include <FileSource.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

struct RealMediaFixture {
    std::string mediaDir() const
    {
        const char *env = std::getenv("SB_MEDIA_DIR");
        if (env != nullptr && env[0] != '\0') {
            return std::string(env);
        }

        const std::string sourcePath(__FILE__);
        const std::string marker = "/sdk/test/fixtures/RealMediaFixture.hpp";
        const size_t markerPos   = sourcePath.rfind(marker);
        if (markerPos != std::string::npos) {
            return sourcePath.substr(0, markerPos) + "/music";
        }

        const std::vector<std::string> candidates = { "../../music", "../music", "music" };
        for (std::vector<std::string>::const_iterator it = candidates.begin();
             it != candidates.end(); ++it) {
            if (pathExists(joinPath(*it, "music.wav"))) {
                return *it;
            }
        }

        return "../../music";
    }

    std::string mediaPath(const std::string &fileName) const
    {
        return joinPath(mediaDir(), fileName);
    }

    bool exists(const std::string &fileName) const { return pathExists(mediaPath(fileName)); }

    std::unique_ptr<ExtractorHelper> create(const std::string &fileName,
                                            const std::string &extension,
                                            std::shared_ptr<FileSource> &source) const
    {
        const std::string path = mediaPath(fileName);
        source.reset(new FileSource(path.c_str()));
        if (!source || source->initCheck() != sdk_utils::OK) {
            return std::unique_ptr<ExtractorHelper>();
        }

        return std::unique_ptr<ExtractorHelper>(
            ExtractorFactory::createExtractor(source.get(), extension, true));
    }

    bool create(const std::string &fileName, const std::string &extension,
                std::shared_ptr<FileSource> &source,
                std::unique_ptr<ExtractorHelper> &extractor) const
    {
        source.reset();
        extractor = create(fileName, extension, source);
        return source && source->initCheck() == sdk_utils::OK;
    }

    bool openOrSkip(const std::string &fileName, const std::string &extension,
                    const std::string &skipName, std::shared_ptr<FileSource> &source,
                    std::unique_ptr<ExtractorHelper> &extractor) const
    {
        if (create(fileName, extension, source, extractor)) {
            return true;
        }

        std::printf("[SKIP] %s (file not found)\n", skipName.c_str());
        return false;
    }

    // 覆盖度断言的双向容差（决策来源 #16）：max(声明时长 × 1%, 150ms)，上限为声明时长的 25%。
    //   1%   ：相对量，随素材时长放大；实测最大偏差是 asf 的 +0.20%，留 5× 余量
    //   150ms：绝对下限，覆盖帧/页量化误差（ogg 一页最多约 93ms、AAC 一帧 21ms）
    //   25%  ：上限，避免极短素材上的断言退化成「怎么都过」
    static uint64_t coverageToleranceMs(uint64_t declaredMs)
    {
        const uint64_t relative = declaredMs / 100;
        const uint64_t floorMs  = 150;
        const uint64_t cap      = declaredMs / 4;

        uint64_t tolerance = std::max(relative, floorMs);
        if (cap > 0 && tolerance > cap) {
            tolerance = cap;
        }
        return tolerance;
    }

private:
    // exists() 的语义是「能被 FileSource 打开」，不是「文件在磁盘上存在」。
    // Windows 上 std::ifstream 走窄字符路径，中文名素材一律打不开（#14）；
    // FileSource 内部经 Utf8Path 转宽字符，与生产链路走同一条路径解析。
    static bool pathExists(const std::string &path)
    {
        FileSource probe(path.c_str());
        return probe.initCheck() == sdk_utils::OK;
    }

    static std::string joinPath(const std::string &directory, const std::string &fileName)
    {
        if (directory.empty()) {
            return fileName;
        }
        if (directory[directory.size() - 1] == '/') {
            return directory + fileName;
        }
        return directory + "/" + fileName;
    }
};
