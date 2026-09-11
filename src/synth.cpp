#include "../runtime/kernel.hpp"
#include "../runtime/model.hpp"
#include "../runtime/qwen.hpp"
#include <filesystem>
#include "glyph/compiler.hpp"
#include <cmath>
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
// Compilation only proves that a candidate types and connects. An `ensure`
// contract is a runtime check, so the way to make it filter candidates is to run
// the flow. Probes that fall outside a declared input refinement, or that trip a
// `require`, are not candidate faults and are skipped.
static std::string probe_contracts(const Compiled &c, const std::string &node) {
    const ResolvedFlow *target = nullptr;
    for (const auto &flow : c.flows) {
        if (!flow.effects.empty())
            continue;
        for (const auto &step : flow.steps)
            if (step.node == node) {
                target = &flow;
                break;
            }
        if (target)
            break;
    }
    if (!target)
        return "";
    std::vector<std::vector<runtime::Value>> columns;
    for (const auto &input : target->flow.inputs) {
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
    runtime::Module module;
    try {
        module = runtime::decode(emit(c, target->flow.name));
    } catch (const Error &) {
        return "";
    }
    const std::string marker = node + ": ensure";
    for (const auto &row : grid) {
        try {
            runtime::run(module, row);
        } catch (const Error &e) {
            std::string message = e.what();
            if (message.find(marker) == std::string::npos)
                continue;
            std::string inputs;
            for (size_t i = 0; i < row.size(); ++i)
                inputs += (i ? ", " : "") + target->flow.inputs[i].name + " = " +
                          runtime::display(row[i]);
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
                if(candidate.rfind("```",0)==0){auto first=candidate.find('\n'),last=candidate.rfind("```");
                    if(first!=candidate.npos&&last>first&&trim(candidate.substr(last+3)).empty())
                        candidate=trim(candidate.substr(first+1,last-first-1));}
            }
            try {
                if (candidate.empty() || candidate.find('\n') != candidate.npos ||
                    candidate.find('\r') != candidate.npos)
                    throw Error("candidate must be a single expression");
                auto trial = p;
                trial.nodes.at(name).sections["impl"] = {{node.line, candidate}};
                auto checked = compile(trial);
                auto violation = probe_contracts(checked, name);
                if (!violation.empty())
                    throw Error(violation);
                p = std::move(trial);
                accepted = true;
                trace += "{\"node\":" + quote(name) + ",\"generator\":" + quote(generator) +
                         ",\"input\":" + quote(unit) + ",\"candidate\":" + quote(candidate) +
                         ",\"accepted\":true}\n";
                break;
            } catch (const Error &e) {
                diagnostic = "Candidate: "+candidate+"\n"+e.what();
                trace += "{\"node\":" + quote(name) + ",\"input\":" + quote(unit) +
                         ",\"candidate\":" + quote(candidate) +
                         ",\"accepted\":false,\"diagnostic\":" + quote(diagnostic) + "}\n";
                candidate.clear();
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
