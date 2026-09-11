#include "glyph/compiler.hpp"
#include <cctype>
#include <cmath>
namespace glyph {
namespace {
struct Token {
    std::string text;
    bool str = false;
};
class Parser {
    std::vector<Token> ts;
    size_t at = 0;
    const Env &env;
    const Program &p;
    int line;
    Token take() {
        return ts.at(at++);
    }
    bool eat(const std::string &s) {
        if (ts.at(at).text == s && !ts.at(at).str) {
            ++at;
            return true;
        }
        return false;
    }
    void need(const std::string &s) {
        if (!eat(s))
            fail(line, "expected '" + s + "'");
    }
    static int prec(const std::string &s) {
        if (s == "||")
            return 1;
        if (s == "&&")
            return 2;
        if (s == "==" || s == "!=")
            return 3;
        if (s == "<" || s == ">" || s == "<=" || s == ">=")
            return 4;
        if (s == "+" || s == "-")
            return 5;
        if (s == "*" || s == "/" || s == "%")
            return 6;
        return 0;
    }
    Expr primary() {
        if (eat("!")) {
            auto x = primary();
            if (x.type != "Bool")
                fail(line, "! requires Bool");
            x.code += "NOT \"\"\n";
            return x;
        }
        if (eat("-")) {
            auto x = primary();
            if (x.type != "Int" && x.type != "Float")
                fail(line, "unary - requires number");
            x.code += "NEG \"\"\n";
            return x;
        }
        Expr x;
        auto t = take();
        if (t.str)
            x = {"Text", "TEXT " + quote(t.text) + "\n", {}};
        else if (t.text == "(") {
            x = parse_expr();
            need(")");
        } else if (t.text == "true" || t.text == "false")
            x = {"Bool", "BOOL " + quote(t.text) + "\n", {}};
        else if (!t.text.empty() && std::isdigit(static_cast<unsigned char>(t.text[0]))) {
            bool f = t.text.find('.') != t.text.npos;
            try {
                size_t n = 0;
                if (f) {
                    auto d = std::stod(t.text, &n);
                    if (!std::isfinite(d))
                        throw Error("finite");
                } else
                    (void)std::stoll(t.text, &n);
                if (n != t.text.size())
                    throw Error("literal");
            } catch (...) {
                fail(line, "invalid numeric literal " + t.text);
            }
            x = {
                f ? "Float" : "Int", std::string(f ? "FLOAT " : "INT ") + quote(t.text) + "\n", {}};
        } else if (eat("(")) {
            std::vector<Expr> args;
            if (!eat(")")) {
                do {
                    args.push_back(parse_expr());
                } while (eat(","));
                need(")");
            }
            for (auto &a : args) {
                x.code += a.code;
                x.effects.insert(a.effects.begin(), a.effects.end());
            }
            auto check = [&](std::vector<std::string> types) {
                if (types.size() != args.size())
                    fail(line, "wrong argument count for " + t.text);
                for (size_t i = 0; i < types.size(); ++i)
                    if (types[i] != args[i].type)
                        fail(line, "argument type mismatch for " + t.text + ": expected " +
                                       types[i] + ", got " + args[i].type);
            };
            if (p.records.count(t.text)) {
                std::vector<std::string> types;
                for (auto &f : p.records.at(t.text).fields)
                    types.push_back(f.type);
                check(types);
                x.type = t.text;
                x.code += "RECORD " + quote(t.text) + "\n";
            } else if (t.text == "Text.length") {
                check({"Text"});
                x.type = "Int";
                x.code += "LEN \"\"\n";
            } else if (t.text == "Text.concat") {
                check({"Text", "Text"});
                x.type = "Text";
                x.code += "ADD \"\"\n";
            } else if (t.text == "Text.fromInt") {
                check({"Int"});
                x.type = "Text";
                x.code += "STR \"\"\n";
            } else if (t.text == "Text.toInt") {
                check({"Text"});
                x.type = "Int";
                x.code += "PARSEINT \"\"\n";
            } else if (t.text == "IO.readLine") {
                check({});
                x.type = "Text";
                x.effects.insert("IO");
                x.code += "READLINE \"\"\n";
            } else if (t.text == "IO.print") {
                check({"Text"});
                x.type = "Text";
                x.effects.insert("IO");
                x.code += "PRINT \"\"\n";
            } else if (t.text == "Weight.load") {
                check({"Text", "Text"}); x.type="Weight"; x.effects.insert("File.Read");
                x.code += "TWEIGHT \"\"\n";
            } else if (t.text == "select") {
                if (args.size() != 3 || args[0].type != "Bool" || args[1].type != args[2].type)
                    fail(line, "select expects (Bool, T, T)");
                x.type = args[1].type;
                x.code += "SELECT \"\"\n";
            } else if (t.text.rfind("Tensor.", 0) == 0) {
                const std::map<std::string, std::pair<std::vector<std::string>, std::string>> ops =
                    {{"Tensor.Linear", {{"Tensor","Weight"},"TLINEAR"}},
                     {"Tensor.Embed", {{"Weight","Tensor"},"TWEMBED"}},
                     {"Tensor.Bias", {{"Tensor","Weight"},"TBIAS"}},
                     {"Tensor.Norm", {{"Tensor","Weight","Float"},"TWNORM"}},
                     {"Tensor.MultiRoPE", {{"Tensor","Int","Float"},"TMROPE"}},
                     {"Tensor.Attend", {{"Tensor","Tensor","Tensor","Int","Int"},"TGQA"}},
                     {"Tensor.AttendCached", {{"Tensor","Tensor","Tensor","Int","Int","Text"},"TGQACACHE"}},
                     {"Tensor.MultiRoPEAt", {{"Tensor","Int","Float","Int"},"TMROPEAT"}},
                     {"Tensor.full", {{"Int", "Int", "Float"}, "TFULL"}},
                     {"Tensor.values", {{"Int", "Int", "Text"}, "TVALUES"}},
                     {"Tensor.load", {{"Text"}, "TLOAD"}},
                     {"Tensor.MatMul", {{"Tensor", "Tensor"}, "TMATMUL"}},
                     {"Tensor.Add", {{"Tensor", "Tensor"}, "TADD"}},
                     {"Tensor.Mul", {{"Tensor", "Tensor"}, "TMUL"}},
                     {"Tensor.Embedding", {{"Tensor", "Tensor"}, "TEMBED"}},
                     {"Tensor.RMSNorm", {{"Tensor", "Tensor"}, "TRMS"}},
                     {"Tensor.Scale", {{"Tensor", "Float"}, "TSCALE"}},
                     {"Tensor.Transpose", {{"Tensor"}, "TTRANSPOSE"}},
                     {"Tensor.Softmax", {{"Tensor"}, "TSOFTMAX"}},
                     {"Tensor.SiLU", {{"Tensor"}, "TSILU"}},
                     {"Tensor.RoPE", {{"Tensor"}, "TROPE"}},
                     {"Tensor.CausalMask", {{"Tensor"}, "TCAUSAL"}},
                     {"Tensor.Last", {{"Tensor"}, "TLAST"}},
                     {"Tensor.rows", {{"Tensor"}, "TROWS"}},
                     {"Tensor.cols", {{"Tensor"}, "TCOLS"}},
                     {"Tensor.argmax", {{"Tensor"}, "TARGMAX"}}};
                auto i = ops.find(t.text);
                if (i == ops.end())
                    fail(line, "unknown tensor primitive " + t.text);
                check(i->second.first);
                x.type = (t.text == "Tensor.rows" || t.text == "Tensor.cols" ||
                          t.text == "Tensor.argmax")
                             ? "Int"
                             : "Tensor";
                x.code += i->second.second + " \"\"\n";
                if (t.text == "Tensor.load")
                    x.effects.insert("File.Read");
                if (t.text == "Tensor.AttendCached")
                    x.effects.insert("Model.Cache");
            } else if (t.text == "Unit") {
                check({});
                x.type = "Unit";
                x.code += "UNIT \"\"\n";
            } else
                fail(line, "unknown constructor or allowed builtin " + t.text);
        } else {
            auto it = env.find(t.text);
            if (it == env.end())
                fail(line, "unknown immutable binding " + t.text);
            x = {it->second.type, it->second.code, {}};
        }
        while (eat(".")) {
            auto f = take().text;
            if (!p.records.count(x.type))
                fail(line, "field access requires a struct");
            bool found = false;
            for (auto &v : p.records.at(x.type).fields)
                if (v.name == f) {
                    x.type = v.type;
                    found = true;
                    break;
                }
            if (!found)
                fail(line, "unknown field " + f);
            x.code += "FIELD " + quote(f) + "\n";
        }
        return x;
    }
    Expr parse_expr(int min = 1) {
        auto x = primary();
        while (true) {
            std::string op = ts.at(at).text;
            int pr = prec(op);
            if (pr < min)
                break;
            take();
            auto y = parse_expr(pr + 1);
            if (x.type != y.type)
                fail(line, "operator " + op + " requires matching types, got " + x.type + " and " +
                               y.type);
            bool num = x.type == "Int" || x.type == "Float";
            std::string ins;
            if (op == "&&" || op == "||") {
                if (x.type != "Bool")
                    fail(line, "logical operators require Bool");
                ins = op == "&&" ? "AND" : "OR";
            } else if (op == "==" || op == "!=") {
                if (!num && x.type != "Text" && x.type != "Bool")
                    fail(line, "equality supports primitives");
                x.type = "Bool";
                ins = op == "==" ? "EQ" : "NE";
            } else if (pr == 4) {
                if (!num && x.type != "Text")
                    fail(line, "ordering requires numbers or Text");
                x.type = "Bool";
                ins = op == "<" ? "LT" : op == ">" ? "GT" : op == "<=" ? "LE" : "GE";
            } else {
                if (!num && !(op == "+" && x.type == "Text"))
                    fail(line, "arithmetic requires numbers (or Text + Text)");
                if (op == "%" && x.type != "Int")
                    fail(line, "% requires Int");
                ins = op == "+"   ? "ADD"
                      : op == "-" ? "SUB"
                      : op == "*" ? "MUL"
                      : op == "/" ? "DIV"
                                  : "MOD";
            }
            x.code += y.code + ins + " \"\"\n";
            x.effects.insert(y.effects.begin(), y.effects.end());
        }
        return x;
    }

  public:
    Parser(const std::string &s, const Env &e, const Program &pg, int l) : env(e), p(pg), line(l) {
        for (size_t i = 0; i < s.size();) {
            char c = s[i];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++i;
                continue;
            }
            if (c == '"') {
                ++i;
                std::string v;
                bool closed = false;
                while (i < s.size()) {
                    char z = s[i++];
                    if (z == '"') {
                        closed = true;
                        break;
                    }
                    if (z == '\\') {
                        if (i == s.size())
                            fail(line, "unterminated escape");
                        z = s[i++];
                        if (z == 'n')
                            z = '\n';
                        else if (z == 't')
                            z = '\t';
                        else if (z != '"' && z != '\\')
                            fail(line, "unsupported string escape");
                    }
                    v += z;
                }
                if (!closed)
                    fail(line, "unterminated string");
                ts.push_back({v, true});
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                size_t b = i++;
                while (i < s.size() &&
                       (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
                    ++i;
                auto n = s.substr(b, i - b);
                if ((n == "Text" || n == "IO" || n == "Tensor" || n == "Weight") && i < s.size() && s[i] == '.') {
                    ++i;
                    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])))
                        ++i;
                    n = s.substr(b, i - b);
                }
                ts.push_back({n, false});
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                size_t b = i++;
                while (i < s.size() &&
                       (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.'))
                    ++i;
                ts.push_back({s.substr(b, i - b), false});
                continue;
            }
            std::string op = s.substr(i, 2);
            if (op == "==" || op == "!=" || op == "<=" || op == ">=" || op == "&&" || op == "||")
                i += 2;
            else {
                op = s.substr(i++, 1);
                if (std::string("()+-*/%!,<>,.").find(c) == std::string::npos)
                    fail(line, "invalid expression character");
            }
            ts.push_back({op, false});
        }
        ts.push_back({"<end>", false});
    }
    Expr run() {
        auto x = parse_expr();
        if (ts.at(at).text != "<end>")
            fail(line, "unexpected token " + ts.at(at).text);
        return x;
    }
};
} // namespace
Expr expression(const std::string &s, const Env &e, const Program &p, int l) {
    try {
        return Parser(s, e, p, l).run();
    } catch (const std::out_of_range &) {
        fail(l, "incomplete expression");
    }
}
} // namespace glyph
