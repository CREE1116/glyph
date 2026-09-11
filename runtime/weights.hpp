#pragma once
#include "kernel.hpp"
namespace glyph::runtime {
struct TensorContext {
    std::map<std::string, std::shared_ptr<Tensor>> weights;
    // Incremental decoding state. Each attention node owns one named slot.
    // Slots are append-only within a sequence and must be cleared between them.
    struct Slot {
        std::shared_ptr<Tensor> keys, values;
    };
    std::map<std::string, Slot> cache;
    size_t cache_limit = 2048;
    void clear_cache() { cache.clear(); }
};
std::shared_ptr<Tensor> load_weight(const std::string& file, const std::string& name, TensorContext& context);
Value model_tensor_op(const std::string& op, std::vector<Value>& stack, TensorContext& context);
}
