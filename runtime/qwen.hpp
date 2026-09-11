#pragma once
#include "weights.hpp"
#include "tokenizer.hpp"
namespace glyph::runtime {
// A model importer generates ordinary Glyph source. Native code supplies primitives only.
std::string qwen_graph(const std::string& directory);
class QwenModel {
    Module module_;
    TensorContext context_;
    QwenTokenizer tokenizer_;
    size_t vocab_size_;
    std::vector<int> eos_;
    std::shared_ptr<Tensor> step(const std::vector<int>& ids, size_t offset);
public:
    explicit QwenModel(const std::string& directory);
    std::string generate(const std::string& prompt);
    std::vector<int> tokenize(const std::string& text)const;
    // Greedy continuation of an exact token sequence. Exercises the attention
    // cache: the prompt is one prefill step, then one token per step.
    std::vector<int> continue_greedy(const std::vector<int>& prompt, size_t count);
    // Diagnostic: greedy-decode with the cache, then recompute every prefix from
    // scratch and report the worst logit difference per step.
    std::vector<double> cache_drift(const std::vector<int>& prompt, size_t count);
    std::vector<double> logits(const std::string& text);
};
}
