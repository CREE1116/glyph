#pragma once
#include <memory>
#include <string>
#include <vector>
namespace glyph::runtime {
class QwenTokenizer {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit QwenTokenizer(const std::string& path);
    ~QwenTokenizer();
    std::vector<int> encode(const std::string& text) const;
    std::string decode(int token) const;
};
}
