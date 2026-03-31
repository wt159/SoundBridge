#pragma once

#include <ExtractorFactory.h>
#include <FileSource.h>
#include <cstdlib>
#include <fstream>
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

private:
    static bool pathExists(const std::string &path)
    {
        std::ifstream stream(path.c_str(), std::ios::binary);
        return stream.good();
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
