#include "model.hpp"
#include "glyph/compiler.hpp"
#include <cmath>
namespace glyph::runtime {
Model::Model(Module module) : module_(std::move(module)) {
    if (module_.inputs.size() != 1 || module_.inputs[0].second != "Tensor")
        throw Error("model requires one Tensor input");
}
std::string Model::generate(const std::string &prompt) const {
    constexpr size_t context_bytes = 4096;
    constexpr size_t max_new_tokens = 256;
    constexpr size_t eos = 256;
    if (prompt.empty() || prompt.size() + max_new_tokens > context_bytes)
        throw Error("model prompt plus generation budget exceeds 4096 bytes");
    auto tokens = std::make_shared<Tensor>();
    tokens->cols = 1;
    for (unsigned char byte : prompt)
        tokens->data.push_back(byte);
    std::string output;
    for (size_t step = 0; step < max_new_tokens; ++step) {
        tokens->rows = tokens->data.size();
        auto logits = tensor(run(module_, {Value(tokens)}));
        if (logits->cols != eos + 1 || logits->rows < 1)
            throw Error("byte model logits must have 257 columns (bytes + EOS)");
        size_t base = (logits->rows - 1) * logits->cols;
        for (size_t i = base; i < logits->data.size(); ++i)
            if (!std::isfinite(logits->data[i]))
                throw Error("nonfinite model logits");
        std::vector<Value> stack{Value(logits)};
        auto id = integer(tensor_op("TARGMAX", stack));
        if (id == eos)
            return trim(output);
        output += char(id);
        tokens->data.push_back(double(id));
    }
    throw Error("model did not emit EOS within 256 tokens");
}
} // namespace glyph::runtime
