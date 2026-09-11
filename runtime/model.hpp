#pragma once
#include "kernel.hpp"
namespace glyph::runtime {
// Bootstrap byte tokenizer and greedy autoregressive driver. Forward is ordinary
// Glyph bytecode. There is deliberately no model-specific C++ forward function.
class Model {
    Module module_;

  public:
    explicit Model(Module module);
    std::string generate(const std::string &prompt) const;
};
} // namespace glyph::runtime
