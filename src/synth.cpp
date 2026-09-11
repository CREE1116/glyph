#include "../runtime/kernel.hpp"
#include "../runtime/model.hpp"
#include "../runtime/qwen.hpp"
#include <filesystem>
#include "glyph/compiler.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
namespace glyph {
static std::string serialize(const Program &p) {
    std::string s = "# Fully resolved Glyph. Generated implementations are checked source.\n\n";
    for (auto &[name, r] : p.records) {
        s += (r.storage.empty() ? "type " : "state ") + name +
             (r.storage.empty() ? "" : " " + r.storage) + " {\n";
        for (auto &f : r.fields)
            s += "    " + f.name + ": " + f.type + (r.unique.count(f.name) ? " unique" : "") + "\n";
        s += "}\n\n";
    }
    auto ports = [&](const std::vector<Port> &ins, const std::string &out) {
        if (!ins.empty()) {
            s += "in:\n";
            for (auto &i : ins)
                s += "    " + i.name + ": " + i.type +
                     (i.refinement.empty() ? "" : " " + i.refinement) + "\n";
        }
        s += "out:\n    " + out + "\n";
    };
    for (auto &[name, n] : p.nodes) {
        s += "node " + name + "\n";
        ports(n.inputs, n.output);
        for (auto &[key, lines] : n.sections) {
            if (key == "in" || key == "out" || lines.empty())
                continue;
            s += key + ":\n";
            for (auto &l : lines)
                s += "    " + l.text + "\n";
        }
        s += "\n";
    }
    for (auto &[name, f] : p.flows) {
        s += "flow " + name + (f.atomic ? " atomic" : "") + "\n";
        ports(f.inputs, f.output);
        if (!f.failures.empty()) {
            s += "fail:\n";
            for (auto &x : f.failures)
                s += "    " + x + "\n";
        }
        for (auto &l : f.steps)
            s += l.text + "\n";
        s += "\n";
    }
    return s;
}
// The compiler already knows the node's input names, that Bool literals are
// lowercase, and how min/max/abs desugar into select. Asking the model to get
// those right is asking it to redo work the compiler can do, so a candidate is
// normalized before it is judged. Every rewrite is applied only when it is
// unambiguous, and the normalized text still has to pass the ordinary type,
// effect and contract checks. Rewrites are recorded so a trace shows them.
namespace {
bool word_char(char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Splits the argument list of a call at top-level commas. Empty on imbalance.
std::vector<std::string> call_arguments(const std::string &text, size_t open, size_t &close) {
    std::vector<std::string> args;
    int depth = 0;
    size_t start = open + 1;
    for (size_t i = open; i < text.size(); ++i) {
        if (text[i] == '(')
            ++depth;
        else if (text[i] == ')') {
            if (--depth == 0) {
                args.push_back(trim(text.substr(start, i - start)));
                close = i;
                return args;
            }
        } else if (text[i] == ',' && depth == 1) {
            args.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return {};
}

// Rewrites min/max/abs calls into select, innermost first.
bool desugar(std::string &text, std::vector<std::string> &applied) {
    static const char *names[] = {"min", "max", "abs"};
    for (const char *name : names) {
        size_t at = 0;
        while ((at = text.find(name, at)) != std::string::npos) {
            size_t after = at + strlen(name);
            bool bounded = (at == 0 || !word_char(text[at - 1])) && after < text.size() &&
                           text[after] == '(';
            if (!bounded) {
                ++at;
                continue;
            }
            size_t close = 0;
            auto args = call_arguments(text, after, close);
            size_t wanted = std::string(name) == "abs" ? 1u : 2u;
            if (args.size() != wanted || args[0].empty() ||
                (wanted == 2 && args[1].empty()) ||
                args[0].find('(') != std::string::npos ||
                (wanted == 2 && args[1].find('(') != std::string::npos)) {
                ++at;
                continue; // Nested or malformed: leave it for the type checker to reject.
            }
            std::string replacement;
            if (wanted == 1)
                replacement = "select(" + args[0] + " < 0, -(" + args[0] + "), " + args[0] + ")";
            else
                replacement = "select(" + args[0] + (std::string(name) == "max" ? " > " : " < ") +
                              args[1] + ", " + args[0] + ", " + args[1] + ")";
            applied.push_back(std::string("rewrote ") + name + " as select");
            text = text.substr(0, at) + replacement + text.substr(close + 1);
            return true;
        }
    }
    return false;
}

std::string normalize_candidate(std::string text, const Node &node, const Program &program,
                                std::vector<std::string> &applied) {
    if (text.rfind("```", 0) == 0) {
        auto first = text.find('\n'), last = text.rfind("```");
        if (first != std::string::npos && last > first) {
            text = trim(text.substr(first + 1, last - first - 1));
            applied.push_back("removed Markdown fence");
        }
    }
    auto newline = text.find('\n');
    if (newline != std::string::npos) {
        text = trim(text.substr(0, newline));
        applied.push_back("kept the first line");
    }
    for (const auto &pair : {std::pair<const char *, const char *>{"True", "true"},
                             {"False", "false"}}) {
        size_t at = 0;
        const std::string from = pair.first, to = pair.second;
        while ((at = text.find(from, at)) != std::string::npos) {
            bool bounded = (at == 0 || !word_char(text[at - 1])) &&
                           (at + from.size() >= text.size() || !word_char(text[at + from.size()]));
            if (!bounded) {
                at += from.size();
                continue;
            }
            text = text.substr(0, at) + to + text.substr(at + from.size());
            applied.push_back("lowercased the Bool literal " + from);
            at += to.size();
        }
    }
    while (desugar(text, applied)) {
    }
    // A single unbound bare name can only have meant the node's only input.
    if (node.inputs.size() == 1 && !node.inputs[0].name.empty()) {
        std::set<std::string> known{node.inputs[0].name, "input", "output", "true", "false",
                                    "select", "Tensor", "Text", "Weight", "Unit"};
        for (const auto &record : program.records)
            known.insert(record.first);
        std::set<std::string> unbound;
        for (size_t i = 0; i < text.size();) {
            if (!word_char(text[i]) || isdigit(static_cast<unsigned char>(text[i]))) {
                ++i;
                continue;
            }
            size_t start = i;
            while (i < text.size() && word_char(text[i]))
                ++i;
            std::string word = text.substr(start, i - start);
            bool called = i < text.size() && (text[i] == '(' || text[i] == '.');
            bool qualified = start > 0 && text[start - 1] == '.';
            if (!called && !qualified && !known.count(word))
                unbound.insert(word);
        }
        if (unbound.size() == 1) {
            const std::string from = *unbound.begin(), to = node.inputs[0].name;
            std::string rebound;
            for (size_t i = 0; i < text.size();) {
                if (text.compare(i, from.size(), from) == 0 &&
                    (i == 0 || !word_char(text[i - 1])) &&
                    (i + from.size() >= text.size() || !word_char(text[i + from.size()]))) {
                    rebound += to;
                    i += from.size();
                } else
                    rebound += text[i++];
            }
            applied.push_back("bound the name " + from + " to the input " + to);
            text = rebound;
        }
    }
    return trim(text);
}
} // namespace

// Compilation only proves that a candidate types and connects. An `ensure`
// contract is a runtime check, and so is an arithmetic fault such as division by
// zero, so the way to make either filter candidates is to run the node.
//
// The node is probed on its own, in a throwaway one-node flow, rather than
// inside the program's real flow. Probing the real flow would skip every node
// whose siblings are still unresolved, and it would blame a node for a contract
// its neighbour broke. A probe that falls outside a declared input refinement,
// or that trips a `require`, describes an input the node never promised to
// handle, so it is skipped. Everything else the node raises counts against it.
static std::string probe_contracts(const Program &program, const Node &node) {
    std::vector<std::vector<runtime::Value>> columns;
    for (const auto &input : node.inputs) {
        if (input.type == "Int")
            columns.push_back({std::int64_t(-1000), std::int64_t(-1), std::int64_t(0),
                               std::int64_t(1), std::int64_t(7), std::int64_t(50),
                               std::int64_t(100), std::int64_t(1000)});
        else if (input.type == "Float")
            columns.push_back({-1.5, 0.0, 1.5, 100.0});
        else if (input.type == "Bool")
            columns.push_back({true, false});
        else if (input.type == "Text")
            columns.push_back({std::string(""), std::string("a"), std::string("glyph")});
        else
            return ""; // Structures and Tensors have no meaningful default grid.
    }
    Program single;
    single.path = "<contract probe>";
    single.records = program.records;
    single.nodes[node.name] = node;
    Flow flow;
    flow.name = "Probe";
    flow.output = node.output;
    flow.line = node.line;
    std::string arguments;
    for (auto input : node.inputs) {
        input.refinement.clear(); // Refinements belong to the node, not the flow.
        flow.inputs.push_back(input);
        arguments += (arguments.empty() ? "" : ", ") + input.name;
    }
    flow.steps = {{node.line, node.name + "(" + arguments + ")"}};
    single.flows["Probe"] = flow;
    runtime::Module module;
    try {
        module = runtime::decode(emit(compile(single), "Probe"));
    } catch (const Error &) {
        return ""; // Effects, records or anything else this probe cannot stage.
    }
    std::vector<std::vector<runtime::Value>> grid{{}};
    for (const auto &column : columns) {
        std::vector<std::vector<runtime::Value>> next;
        for (const auto &row : grid)
            for (const auto &value : column) {
                if (next.size() >= 64)
                    break;
                next.push_back(row);
                next.back().push_back(value);
            }
        grid = std::move(next);
    }
    for (const auto &row : grid) {
        try {
            runtime::run(module, row);
        } catch (const Error &e) {
            std::string message = e.what();
            if (message.find("input refinement") != std::string::npos ||
                message.find(": require ") != std::string::npos)
                continue;
            std::string inputs;
            for (size_t i = 0; i < row.size(); ++i)
                inputs += (i ? ", " : "") + node.inputs[i].name + " = " + runtime::display(row[i]);
            return message + (inputs.empty() ? "" : " with " + inputs);
        }
    }
    return "";
}
std::string synthesize(const Compiled &original, const std::string &model_path,
                       const std::string &trace_path) {
    Program p = original.program;
    std::unique_ptr<runtime::Model> model;
    std::unique_ptr<runtime::QwenModel> qwen;
    std::string trace;
    for (auto &name : original.unresolved) {
        auto &node = p.nodes.at(name);
        std::string candidate;
        std::string generator = "deterministic.ensure-equality";
        for (auto &l : node.sections["ensure"]) {
            const std::string prefix = "output == ";
            if (l.text.rfind(prefix, 0) == 0) {
                candidate = l.text.substr(prefix.size());
                break;
            }
        }
        Compiled sliced = original;
        sliced.unresolved = {name};
        std::string unit = synthesis_units(sliced);
        std::string prompt =
            "Implement one Glyph node. Return only one expression, no Markdown.\n"
            "Syntax: select(condition, value_if_true, value_if_false). No if/else, ternary ?, min, max, return or function definitions.\n"
            "The condition must be Bool. The two branches must both have the node's output type.\n"
            "Comparisons such as x < 0 are Bool, so they can only appear as a condition.\n"
            "Nest select for more than two cases.\n"
            "Example absolute value: select(x < 0, -x, x)\n"
            "Example increasing an integer: input + 1\n"
            "Example upper bound of ten: select(x > 10, 10, x)\n" + unit;
        std::string diagnostic;
        std::set<std::string> rejected;
        bool accepted = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (candidate.empty()) {
                if (model_path.empty())
                    throw Error("cannot deterministically synthesize " + name +
                                "; pass --model model.glyph (native Glyph graph), or write impl");
                if (!model && !qwen && std::filesystem::is_directory(model_path))
                    qwen=std::make_unique<runtime::QwenModel>(model_path);
                if (!model && !qwen) {
                    auto compiled_model = compile(parse(read_file(model_path), model_path));
                    if (!compiled_model.program.flows.count("Forward") ||
                        compiled_model.program.flows.at("Forward").output != "Tensor")
                        throw Error("model requires flow Forward: Tensor -> Tensor");
                    for (const auto &flow : compiled_model.flows)
                        for (const auto &effect : flow.effects)
                            if (effect != "File.Read" && effect != "Model.Cache")
                                throw Error("model flow only allows File.Read for weights and "
                                            "Model.Cache for incremental decoding");
                    model = std::make_unique<runtime::Model>(
                        runtime::decode(emit(compiled_model, "Forward")));
                }
                generator = qwen ? "glyph.runtime.qwen2" : "glyph.graph.byte-lm.v1";
                // The repair note has to arrive before the answer cue, or the model
                // simply continues from the cue and repeats its rejected answer.
                auto request=prompt+(diagnostic.empty()?"":"The previous attempt was rejected by the compiler.\n"+
                                     diagnostic+"\nReturn a corrected expression.\n")+"EXPRESSION:\n";
                candidate=qwen?qwen->generate(request):model->generate(request);
                std::vector<std::string> rewrites;
                candidate = normalize_candidate(candidate, node, p, rewrites);
                for (const auto &rewrite : rewrites)
                    std::cerr << "normalized " << name << ": " << rewrite << "\n";
            }
            try {
                if (candidate.empty() || candidate.find('\n') != candidate.npos ||
                    candidate.find('\r') != candidate.npos)
                    throw Error("candidate must be a single expression");
                auto trial = p;
                trial.nodes.at(name).sections["impl"] = {{node.line, candidate}};
                compile(trial);
                auto violation = probe_contracts(trial, trial.nodes.at(name));
                if (!violation.empty())
                    throw Error(violation);
                p = std::move(trial);
                accepted = true;
                trace += "{\"node\":" + quote(name) + ",\"generator\":" + quote(generator) +
                         ",\"input\":" + quote(unit) + ",\"candidate\":" + quote(candidate) +
                         ",\"accepted\":true}\n";
                break;
            } catch (const Error &e) {
                bool repeat = !rejected.insert(candidate).second;
                diagnostic = "Candidate: "+candidate+"\n"+e.what();
                trace += "{\"node\":" + quote(name) + ",\"input\":" + quote(unit) +
                         ",\"candidate\":" + quote(candidate) +
                         ",\"accepted\":false,\"diagnostic\":" + quote(diagnostic) + "}\n";
                candidate.clear();
                // Decoding is greedy, so an identical candidate means the repair
                // note changed nothing and further attempts cannot either.
                if (repeat) {
                    std::cerr << "stopping " << name << ": the model repeated a rejected candidate\n";
                    break;
                }
            }
        }
        if (!accepted) {
            if (!trace_path.empty())
                write_file(trace_path, trace);
            throw Error("synthesis verification failed for " + name + ": " + diagnostic);
        }
        std::cerr << "synthesized " << name << " (" << generator << ")\n";
    }
    auto source = serialize(p);
    compile(parse(source, "<synthesized>"));
    if (!trace_path.empty())
        write_file(trace_path, trace);
    return source;
}
} // namespace glyph
