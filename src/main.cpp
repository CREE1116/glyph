#include "../runtime/kernel.hpp"
#include "../runtime/qwen.hpp"
#include "glyph/compiler.hpp"
#include <filesystem>
#include <iostream>
namespace fs = std::filesystem;
int main(int argc, char **argv) {
    using namespace glyph;
    try {
        if (argc < 2 || std::string(argv[1]) == "--help") {
            std::cout
                << "Glyph 0.1-dev — C++ graph compiler and native bytecode runtime\n"
                   "glyph model MODEL_DIR --tokens TEXT | --logits TEXT | --export model.glyph\n"
                   "glyph check SOURCE\n"
                   "glyph graph SOURCE [--html] [-o graph.json|graph.html]\n"
                   "glyph build SOURCE [-o program.gyb] [--flow Main]\n"
                   "glyph run SOURCE|program.gyb [--flow Main] [--db file.sqlite]\n"
                   "          [--model MODEL_DIR] [-- ARG...]\n"
                   "glyph synth SOURCE -o resolved.glyph [--model model.glyph] [--trace "
                   "trace.jsonl]\n"
                   "glyph synth SOURCE --units [-o units.txt]\n"
                   "glyph test SOURCE [--flow Main] [-- ARG...]\n";
            return 0;
        }
        std::string command = argv[1];
        if (command == "--version") {
            std::cout << "Glyph 0.1-dev bytecode-v1\n";
            return 0;
        }
        if (argc < 3)
            throw Error("source file required; see --help");
        if(command=="model") {
            if(argc!=5)throw Error("usage: glyph model MODEL_DIR --tokens TEXT | --logits TEXT | --generate TEXT | --export FILE");
            std::string mode=argv[3];
            if(mode=="--export"){write_file(argv[4],runtime::qwen_graph(argv[2]));return 0;}
            if(mode=="--tokens"){runtime::QwenTokenizer tok((fs::path(argv[2])/"tokenizer.json").string());
                auto ids=tok.encode(argv[4]);std::cout<<"[";for(size_t i=0;i<ids.size();++i){if(i)std::cout<<",";std::cout<<ids[i];}std::cout<<"]\n";return 0;}
            if(mode=="--generate"){runtime::QwenModel m(argv[2]);auto ids=m.continue_greedy(m.tokenize(argv[4]),16);
                std::cout<<"[";for(size_t i=0;i<ids.size();++i){if(i)std::cout<<",";std::cout<<ids[i];}std::cout<<"]\n";return 0;}
            if(mode=="--cache-check"){runtime::QwenModel m(argv[2]);auto drift=m.cache_drift(m.tokenize(argv[4]),8);
                std::cout.precision(4);for(size_t i=0;i<drift.size();++i)std::cout<<"step "<<i<<" max logit drift "<<std::scientific<<drift[i]<<"\n";return 0;}
            if(mode=="--logits"){runtime::QwenModel m(argv[2]);auto values=m.logits(argv[4]);std::cout.precision(10);std::cout<<"[";for(size_t i=0;i<values.size();++i){if(i)std::cout<<",";std::cout<<values[i];}std::cout<<"]\n";return 0;}
            throw Error("unknown model mode "+mode);
        }
        std::string file = argv[2], output, entry, db = ":memory:", model, trace;
        bool units = false, html = false;
        std::vector<std::string> args;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--") {
                while (++i < argc)
                    args.push_back(argv[i]);
                break;
            }
            if (a == "--html") {
                html = true;
                continue;
            }
            if (a == "--units") {
                units = true;
                continue;
            }
            if (a != "-o" && a != "--flow" && a != "--db" && a != "--model" && a != "--trace")
                throw Error("unknown option " + a);
            if (++i == argc)
                throw Error("missing value for " + a);
            if (a == "-o")
                output = argv[i];
            else if (a == "--flow")
                entry = argv[i];
            else if (a == "--db")
                db = argv[i];
            else if (a == "--model")
                model = argv[i];
            else
                trace = argv[i];
        }
        if (command != "check" && command != "graph" && command != "build" && command != "run" &&
            command != "synth" && command != "test")
            throw Error("unknown command " + command);
        if (html && command != "graph")
            throw Error("--html is only valid for graph");
        auto source = read_file(file);
        bool bytecode = source.rfind("GLYPH-BC ", 0) == 0;
        std::string bytes;
        Compiled c;
        if (bytecode) {
            if (command != "run" && command != "test")
                throw Error("bytecode input is only supported by run/test");
            if (!entry.empty())
                throw Error("bytecode entry is fixed at build time");
            bytes = source;
        } else {
            c = compile(parse(source, file));
            if (entry.empty())
                entry = c.program.flows.count("Main") ? "Main"
                        : c.flows.size() == 1         ? c.flows[0].flow.name
                                                      : "";
        }
        auto save = [&](const std::string &content) {
            if (output.empty())
                std::cout << content;
            else {
                if (fs::weakly_canonical(output) == fs::weakly_canonical(file))
                    throw Error("output must not overwrite source");
                write_file(output, content);
                std::cerr << "wrote " << output << '\n';
            }
        };
        if (command == "check") {
            std::cout << "OK: " << c.program.nodes.size() << " nodes, " << c.flows.size()
                      << " flows, " << c.unresolved.size() << " unresolved\n";
            return 0;
        }
        if (command == "graph") {
            save(html ? graph_html(c) : graph(c));
            return 0;
        }
        if (command == "synth") {
            if (units) {
                save(synthesis_units(c));
                return 0;
            }
            if (output.empty())
                throw Error("synth requires -o resolved.glyph");
            if (!trace.empty() && (fs::weakly_canonical(trace) == fs::weakly_canonical(file) ||
                                   fs::weakly_canonical(trace) == fs::weakly_canonical(output)))
                throw Error("trace must use a separate path");
            save(synthesize(c, model, trace));
            return 0;
        }
        // run, test and build compile and execute. When impl is still missing and
        // a model is given, resolve it here instead of making the caller invoke
        // synth by hand. The source file is never touched; the resolved program
        // exists only for this process, and -o still writes bytecode for build.
        if (!bytecode && !c.unresolved.empty() && !model.empty()) {
            auto resolved = synthesize(c, model, trace);
            c = compile(parse(resolved, file));
        }
        if (!bytecode) {
            if (entry.empty())
                throw Error("multiple flows; choose --flow NAME");
            bytes = emit(c, entry);
        }
        if (command == "build") {
            if (output.empty())
                output = fs::path(file).replace_extension(".gyb").string();
            save(bytes);
            return 0;
        }
        auto module = runtime::decode(bytes);
        if (args.size() != module.inputs.size())
            throw Error("entry requires " + std::to_string(module.inputs.size()) +
                        " argument(s) after --");
        std::vector<runtime::Value> inputs;
        for (size_t i = 0; i < args.size(); ++i)
            inputs.push_back(runtime::parse_value(args[i], module.inputs[i].second));
        auto result = runtime::run(module, inputs, db);
        std::cout << runtime::display(result) << '\n';
        if (command == "test")
            std::cerr << "PASS: entry completed and all executed contracts held\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "glyph: " << e.what() << '\n';
        return 1;
    }
}
