#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>
namespace glyph::runtime {
struct Tensor {
    size_t rows = 0, cols = 0;
    std::vector<double> data;
    std::vector<float> weights; // immutable native weight storage; not an activation
};
struct TensorContext;
struct Object;
struct Value {
    using Data = std::variant<std::monostate, std::int64_t, double, bool, std::string,
                              std::shared_ptr<Object>, std::shared_ptr<Tensor>>;
    Data data;
    Value() = default;
    template <class T> Value(T x) : data(std::move(x)) {};
};
struct Object {
    std::string type;
    std::map<std::string, Value> fields;
};
struct Instruction {
    std::string op, arg;
};
struct Function {
    std::vector<std::string> params;
    std::vector<Instruction> code;
};
struct Schema {
    std::string storage;
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<std::string> unique;
};
struct Module {
    std::map<std::string, Function> functions;
    std::map<std::string, Schema> schemas;
    std::vector<std::pair<std::string, std::string>> inputs;
    bool atomic = false;
    std::string entry;
};
Module decode(const std::string &bytecode);
Value run(const Module &module, const std::vector<Value> &inputs,
          const std::string &database = ":memory:", TensorContext* context = nullptr);
Value parse_value(const std::string &text, const std::string &type);
std::string display(const Value &value);
Value tensor_op(const std::string &op, std::vector<Value> &stack);
std::shared_ptr<Tensor> tensor(const Value &v);
std::int64_t integer(const Value &v);
std::string text_value(const Value &v);
} // namespace glyph::runtime
