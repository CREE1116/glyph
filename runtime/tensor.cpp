#include "glyph/compiler.hpp"
#include "kernel.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
namespace glyph::runtime {
std::shared_ptr<Tensor> tensor(const Value &v) {
    auto p = std::get_if<std::shared_ptr<Tensor>>(&v.data);
    if (!p || !*p)
        throw Error("expected Tensor");
    return *p;
}
std::int64_t integer(const Value &v) {
    auto p = std::get_if<std::int64_t>(&v.data);
    if (!p)
        throw Error("expected Int");
    return *p;
}
std::string text_value(const Value &v) {
    auto p = std::get_if<std::string>(&v.data);
    if (!p)
        throw Error("expected Text");
    return *p;
}
static std::shared_ptr<Tensor> allocate(size_t r, size_t c) {
    if (r == 0 || c == 0 || r > 16000000 / c)
        throw Error("invalid Tensor shape or 16M element limit exceeded");
    auto t = std::make_shared<Tensor>();
    t->rows = r;
    t->cols = c;
    t->data.resize(r * c);
    return t;
}
Value tensor_op(const std::string &op, std::vector<Value> &s) {
    auto pop = [&]() {
        if (s.empty())
            throw Error("tensor stack underflow");
        auto v = s.back();
        s.pop_back();
        return v;
    };
    if (op == "TLOAD") {
        auto path = text_value(pop());
        std::ifstream f(path);
        std::string magic;
        size_t r, c;
        if (!(f >> magic >> r >> c) || magic != "GLYPH-TENSOR-1")
            throw Error("invalid tensor file " + path);
        auto t = allocate(r, c);
        for (auto &x : t->data)
            if (!(f >> x) || !std::isfinite(x))
                throw Error("invalid tensor data " + path);
        std::string extra;
        if (f >> extra)
            throw Error("excess tensor data");
        return t;
    }
    if (op == "TVALUES") {
        auto text = text_value(pop());
        auto c = integer(pop()), r = integer(pop());
        auto t = allocate(r, c);
        std::istringstream in(text);
        for (auto &x : t->data)
            if (!(in >> x) || !std::isfinite(x))
                throw Error("Tensor.values needs rows*cols finite numbers");
        std::string extra;
        if (in >> extra)
            throw Error("Tensor.values has more numbers than rows*cols");
        return t;
    }
    if (op == "TFULL") {
        auto value = pop();
        auto c = integer(pop()), r = integer(pop());
        auto t = allocate(r, c);
        double x = std::get<double>(value.data);
        std::fill(t->data.begin(), t->data.end(), x);
        return t;
    }
    if (op == "TROWS" || op == "TCOLS") {
        auto a = tensor(pop());
        return std::int64_t(op == "TROWS" ? a->rows : a->cols);
    }
    if (op == "TARGMAX") {
        auto a = tensor(pop());
        auto b = a->data.begin() + (a->rows - 1) * a->cols;
        return std::int64_t(std::max_element(b, a->data.end()) - b);
    }
    if (op == "TEMBED") {
        auto ids = tensor(pop()), w = tensor(pop());
        if (ids->cols != 1)
            throw Error("Embedding token IDs must be [N,1]");
        auto o = allocate(ids->rows, w->cols);
        for (size_t i = 0; i < ids->rows; ++i) {
            double id = ids->data[i];
            if (id < 0 || id >= double(w->rows) || id != std::floor(id))
                throw Error("Embedding token out of range");
            std::copy_n(w->data.begin() + size_t(id) * w->cols, w->cols,
                        o->data.begin() + i * w->cols);
        }
        return o;
    }
    if (op == "TMATMUL" || op == "TADD" || op == "TMUL") {
        auto b = tensor(pop()), a = tensor(pop());
        if (op == "TMATMUL") {
            if (a->cols != b->rows)
                throw Error("MatMul shape mismatch");
            auto o = allocate(a->rows, b->cols);
            for (size_t i = 0; i < a->rows; ++i)
                for (size_t k = 0; k < a->cols; ++k)
                    for (size_t j = 0; j < b->cols; ++j)
                        o->data[i * b->cols + j] +=
                            a->data[i * a->cols + k] * b->data[k * b->cols + j];
            return o;
        }
        if (a->rows != b->rows || a->cols != b->cols)
            throw Error("elementwise shape mismatch");
        auto o = allocate(a->rows, a->cols);
        for (size_t i = 0; i < o->data.size(); ++i)
            o->data[i] = op == "TADD" ? a->data[i] + b->data[i] : a->data[i] * b->data[i];
        return o;
    }
    if (op == "TRMS") {
        auto w = tensor(pop()), a = tensor(pop());
        if (w->rows != 1 || w->cols != a->cols)
            throw Error("RMSNorm weight shape mismatch");
        auto o = allocate(a->rows, a->cols);
        for (size_t r = 0; r < a->rows; ++r) {
            double sum = 0;
            for (size_t j = 0; j < a->cols; ++j)
                sum += std::pow(a->data[r * a->cols + j], 2);
            double inv = 1 / std::sqrt(sum / a->cols + 1e-6);
            for (size_t j = 0; j < a->cols; ++j)
                o->data[r * a->cols + j] = a->data[r * a->cols + j] * inv * w->data[j];
        }
        return o;
    }
    if (op == "TSCALE") {
        double scale = std::get<double>(pop().data);
        auto a = tensor(pop()), o = allocate(a->rows, a->cols);
        for (size_t i = 0; i < o->data.size(); ++i)
            o->data[i] = a->data[i] * scale;
        return o;
    }
    auto a = tensor(pop());
    if (op == "TTRANSPOSE") {
        auto o = allocate(a->cols, a->rows);
        for (size_t i = 0; i < a->rows; ++i)
            for (size_t j = 0; j < a->cols; ++j)
                o->data[j * a->rows + i] = a->data[i * a->cols + j];
        return o;
    }
    if (op == "TLAST") {
        auto o = allocate(1, a->cols);
        std::copy_n(a->data.end() - a->cols, a->cols, o->data.begin());
        return o;
    }
    auto o = allocate(a->rows, a->cols);
    if (op == "TSILU") {
        for (size_t i = 0; i < o->data.size(); ++i)
            o->data[i] = a->data[i] / (1 + std::exp(-a->data[i]));
        return o;
    }
    if (op == "TSOFTMAX") {
        for (size_t r = 0; r < a->rows; ++r) {
            auto b = a->data.begin() + r * a->cols;
            double m = *std::max_element(b, b + a->cols), sum = 0;
            for (size_t j = 0; j < a->cols; ++j)
                sum += (o->data[r * a->cols + j] = std::exp(a->data[r * a->cols + j] - m));
            if (!std::isfinite(sum) || sum == 0)
                throw Error("Softmax nonfinite input");
            for (size_t j = 0; j < a->cols; ++j)
                o->data[r * a->cols + j] /= sum;
        }
        return o;
    }
    if (op == "TCAUSAL") {
        if (a->rows != a->cols)
            throw Error("CausalMask requires square scores");
        o->data = a->data;
        for (size_t i = 0; i < a->rows; ++i)
            for (size_t j = i + 1; j < a->cols; ++j)
                o->data[i * a->cols + j] = -std::numeric_limits<double>::infinity();
        return o;
    }
    if (op == "TROPE") {
        if (a->cols % 2)
            throw Error("RoPE requires even width");
        for (size_t r = 0; r < a->rows; ++r)
            for (size_t j = 0; j < a->cols / 2; ++j) {
                double angle = r / std::pow(10000.0, 2.0 * j / a->cols),
                       x = a->data[r * a->cols + j], y = a->data[r * a->cols + j + a->cols / 2];
                o->data[r * a->cols + j] = x * std::cos(angle) - y * std::sin(angle);
                o->data[r * a->cols + j + a->cols / 2] = x * std::sin(angle) + y * std::cos(angle);
            }
        return o;
    }
    throw Error("unknown tensor opcode " + op);
}
} // namespace glyph::runtime
