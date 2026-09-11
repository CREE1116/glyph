#include "kernel.hpp"
#include "weights.hpp"
#include "glyph/compiler.hpp"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sqlite3.h>
#include <sstream>
namespace glyph::runtime {
static void require(bool b, const std::string &m) {
    if (!b)
        throw Error("bytecode: " + m);
}
static std::string operand(std::istringstream &in) {
    in >> std::ws;
    require(in.get() == '"', "expected quoted operand");
    std::string result;
    bool closed = false;
    for (int c; (c = in.get()) != EOF;) {
        if (c == '"') {
            closed = true;
            break;
        }
        if (c == '\\') {
            c = in.get();
            if (c == 'n')
                c = '\n';
            else if (c == 'r')
                c = '\r';
            else if (c == 't')
                c = '\t';
            else if (c == 'u') {
                require(in.get() == '0' && in.get() == '0', "only byte escapes supported");
                auto hex = [](int x) {
                    if (x >= '0' && x <= '9')
                        return x - '0';
                    if (x >= 'a' && x <= 'f')
                        return x - 'a' + 10;
                    return -1;
                };
                int a = hex(in.get()), b = hex(in.get());
                require(a >= 0 && b >= 0, "invalid escape");
                c = a * 16 + b;
            } else
                require(c == '"' || c == '\\', "invalid escape");
        }
        require(c >= 0, "truncated operand");
        result += char(c);
    }
    require(closed, "unterminated operand");
    return result;
}
Module decode(const std::string &bytes) {
    std::istringstream in(bytes);
    std::string header;
    std::getline(in, header);
    require(header == "GLYPH-BC 1", "bad magic or version");
    Module m;
    Function *fn = nullptr;
    Schema *schema = nullptr;
    std::string row, field, argname;
    while (std::getline(in, row)) {
        if (row.empty())
            continue;
        std::istringstream r(row);
        std::string op, arg, extra;
        require(bool(r >> op), "malformed instruction");
        arg = operand(r);
        require(!(r >> extra), "trailing data");
        if (op == "FUNC") {
            require(!fn && !schema && !m.functions.count(arg), "duplicate/nested function");
            fn = &m.functions[arg];
        } else if (op == "ENDFUNC") {
            require(fn, "unexpected ENDFUNC");
            fn = nullptr;
        } else if (fn) {
            if (op == "PARAM") {
                require(fn->code.empty(), "late parameter");
                for (auto &p : fn->params)
                    require(p != arg, "duplicate parameter");
                fn->params.push_back(arg);
            } else
                fn->code.push_back({op, arg});
        } else if (op == "TYPE") {
            require(!schema && !m.schemas.count(arg), "duplicate/nested type");
            schema = &m.schemas[arg];
        } else if (op == "ENDTYPE") {
            require(schema, "unexpected ENDTYPE");
            schema = nullptr;
        } else if (schema) {
            if (op == "STORAGE")
                schema->storage = arg;
            else if (op == "FIELDNAME")
                field = arg;
            else if (op == "FIELDTYPE") {
                require(!field.empty(), "missing field name");
                schema->fields.push_back({field, arg});
                field.clear();
            } else if (op == "UNIQUE")
                schema->unique.push_back(arg);
            else
                throw Error("unknown schema directive " + op);
        } else if (op == "ENTRY")
            m.entry = arg;
        else if (op == "ARGNAME")
            argname = arg;
        else if (op == "ARGTYPE") {
            require(!argname.empty(), "missing argument name");
            m.inputs.push_back({argname, arg});
            argname.clear();
        } else if (op == "ATOMIC") {
            require(arg == "0" || arg == "1", "invalid atomic flag");
            m.atomic = arg == "1";
        } else
            throw Error("unknown bytecode directive " + op);
    }
    require(!fn && !schema, "truncated module");
    require(m.functions.count("@entry") && !m.entry.empty(), "missing entry");
    return m;
}
Value parse_value(const std::string &s, const std::string &type) {
    try {
        size_t n;
        if (type == "Text")
            return s;
        if (type == "Int") {
            auto x = std::stoll(s, &n);
            if (n == s.size())
                return std::int64_t(x);
        }
        if (type == "Float") {
            auto x = std::stod(s, &n);
            if (n == s.size() && std::isfinite(x))
                return x;
        }
        if (type == "Bool" && (s == "true" || s == "false"))
            return s == "true";
    } catch (const std::exception &) {
    }
    throw Error("invalid " + type + " argument: " + s);
}
std::string display(const Value &v) {
    if (std::holds_alternative<std::monostate>(v.data))
        return "null";
    if (auto x = std::get_if<std::int64_t>(&v.data))
        return std::to_string(*x);
    if (auto x = std::get_if<double>(&v.data)) {
        std::ostringstream s;
        s << std::setprecision(17) << *x;
        return s.str();
    }
    if (auto x = std::get_if<bool>(&v.data))
        return *x ? "true" : "false";
    if (auto x = std::get_if<std::string>(&v.data))
        return quote(*x);
    if (auto x = std::get_if<std::shared_ptr<Tensor>>(&v.data)) {
        std::string s = "{\"shape\":[" + std::to_string((*x)->rows) + "," +
                        std::to_string((*x)->cols) + "],\"data\":[";
        bool first = true;
        for (auto d : (*x)->data) {
            if (!first)
                s += ",";
            first = false;
            s += display(Value(d));
        }
        return s + "]}";
    }
    auto o = std::get<std::shared_ptr<Object>>(v.data);
    std::string s = "{";
    bool first = true;
    for (auto &[k, x] : o->fields) {
        if (!first)
            s += ",";
        first = false;
        s += quote(k) + ":" + display(x);
    }
    return s + "}";
}
namespace {
class VM {
    const Module &m;
    sqlite3 *db = nullptr;
    size_t fuel = 1000000;
    TensorContext owned_context;
    TensorContext* tensor_context;
    void sql(const std::string &s) {
        char *err = nullptr;
        int rc = sqlite3_exec(db, s.c_str(), nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string message = err ? err : "SQLite error";
            sqlite3_free(err);
            throw Error("DBError: " + message);
        }
    }
    static std::string qi(std::string s) {
        std::string q = "\"";
        for (char c : s) {
            q += c;
            if (c == '"')
                q += '"';
        }
        return q + '"';
    }
    void write(const std::string &name, const Value &v) {
        auto it = m.schemas.find(name);
        if (it == m.schemas.end() || it->second.storage.empty())
            throw Error("invalid state");
        auto o = std::get<std::shared_ptr<Object>>(v.data);
        if (o->type != name)
            throw Error("state type mismatch");
        std::string q = "INSERT INTO " + qi(name) + " VALUES (";
        for (size_t i = 0; i < it->second.fields.size(); ++i) {
            if (i)
                q += ",";
            q += "?";
        }
        q += ")";
        sqlite3_stmt *raw = nullptr;
        if (sqlite3_prepare_v2(db, q.c_str(), -1, &raw, nullptr) != SQLITE_OK)
            throw Error("DBError: " + std::string(sqlite3_errmsg(db)));
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        int i = 1;
        for (auto &f : it->second.fields) {
            auto &x = o->fields.at(f.first);
            int rc = SQLITE_ERROR;
            if (auto a = std::get_if<std::int64_t>(&x.data))
                rc = sqlite3_bind_int64(raw, i, *a);
            else if (auto a = std::get_if<double>(&x.data))
                rc = sqlite3_bind_double(raw, i, *a);
            else if (auto a = std::get_if<bool>(&x.data))
                rc = sqlite3_bind_int(raw, i, *a);
            else if (auto a = std::get_if<std::string>(&x.data))
                rc = sqlite3_bind_text(raw, i, a->data(), a->size(), SQLITE_TRANSIENT);
            if (rc != SQLITE_OK)
                throw Error("DBError: bind failed");
            ++i;
        }
        if (sqlite3_step(raw) != SQLITE_DONE)
            throw Error("DBError: " + std::string(sqlite3_errmsg(db)));
    }
    static bool boolean(const Value &v) {
        auto p = std::get_if<bool>(&v.data);
        if (!p)
            throw Error("expected Bool");
        return *p;
    }
    Value binary(std::string op, Value a, Value b) {
        if (a.data.index() != b.data.index())
            throw Error("operand type mismatch");
        if (op == "AND" || op == "OR")
            return op == "AND" ? (boolean(a) && boolean(b)) : (boolean(a) || boolean(b));
        bool eq = op == "EQ" || op == "NE";
        bool cmp = eq || op == "LT" || op == "GT" || op == "LE" || op == "GE";
        auto compare = [&](auto x, auto y) -> Value {
            if (op == "EQ")
                return x == y;
            if (op == "NE")
                return x != y;
            if (op == "LT")
                return x < y;
            if (op == "GT")
                return x > y;
            if (op == "LE")
                return x <= y;
            return x >= y;
        };
        if (auto x = std::get_if<std::string>(&a.data)) {
            auto y = text_value(b);
            if (cmp)
                return compare(*x, y);
            if (op == "ADD")
                return *x + y;
            throw Error("invalid Text operation");
        }
        if (auto x = std::get_if<bool>(&a.data)) {
            if (!eq)
                throw Error("invalid Bool operation");
            return compare(*x, boolean(b));
        }
        if (auto x = std::get_if<std::int64_t>(&a.data)) {
            auto y = integer(b);
            if (cmp)
                return compare(*x, y);
            std::int64_t z = 0;
            bool overflow = false;
            if (op == "ADD")
                overflow = __builtin_add_overflow(*x, y, &z);
            else if (op == "SUB")
                overflow = __builtin_sub_overflow(*x, y, &z);
            else if (op == "MUL")
                overflow = __builtin_mul_overflow(*x, y, &z);
            else if (op == "DIV" || op == "MOD") {
                if (!y)
                    throw Error("division by zero");
                if (*x == std::numeric_limits<std::int64_t>::min() && y == -1)
                    throw Error("integer overflow");
                z = op == "DIV" ? *x / y : *x % y;
            } else
                throw Error("unknown arithmetic opcode " + op);
            if (overflow)
                throw Error("integer overflow");
            return z;
        }
        if (auto x = std::get_if<double>(&a.data)) {
            double y = std::get<double>(b.data);
            if (cmp)
                return compare(*x, y);
            double z;
            if (op == "ADD")
                z = *x + y;
            else if (op == "SUB")
                z = *x - y;
            else if (op == "MUL")
                z = *x * y;
            else if (op == "DIV") {
                if (!y)
                    throw Error("division by zero");
                z = *x / y;
            } else
                throw Error("invalid Float operation");
            if (!std::isfinite(z))
                throw Error("nonfinite arithmetic result");
            return z;
        }
        throw Error("invalid binary operands");
    }
    Value call(const std::string &name, const std::vector<Value> &args, int depth) {
        if (depth > 128)
            throw Error("call depth exceeded");
        auto it = m.functions.find(name);
        if (it == m.functions.end())
            throw Error("unknown function " + name);
        auto &f = it->second;
        if (args.size() != f.params.size())
            throw Error("argument count mismatch");
        std::map<std::string, Value> locals;
        for (size_t i = 0; i < args.size(); ++i)
            locals[f.params[i]] = args[i];
        std::vector<Value> stack;
        auto pop = [&]() {
            if (stack.empty())
                throw Error("bytecode stack underflow");
            auto v = stack.back();
            stack.pop_back();
            return v;
        };
        for (auto &ins : f.code) {
            if (!fuel--)
                throw Error("instruction budget exhausted");
            auto &op = ins.op;
            auto &arg = ins.arg;
            if (op == "INT" || op == "FLOAT" || op == "BOOL")
                stack.push_back(parse_value(arg, op == "INT"     ? "Int"
                                                 : op == "FLOAT" ? "Float"
                                                                 : "Bool"));
            else if (op == "TEXT")
                stack.emplace_back(arg);
            else if (op == "UNIT")
                stack.emplace_back();
            else if (op == "LOAD") {
                if (!locals.count(arg))
                    throw Error("unknown local " + arg);
                stack.push_back(locals.at(arg));
            } else if (op == "STORE") {
                if (locals.count(arg))
                    throw Error("immutable local reassignment");
                locals[arg] = pop();
            } else if (op == "DROP") {
                if (!locals.erase(arg))
                    throw Error("unknown local release " + arg);
            } else if (op == "CALL") {
                if (!m.functions.count(arg))
                    throw Error("unknown call target");
                std::vector<Value> a(m.functions.at(arg).params.size());
                for (size_t i = a.size(); i > 0; --i)
                    a[i - 1] = pop();
                stack.push_back(call(arg, a, depth + 1));
            } else if (op == "RECORD") {
                if (!m.schemas.count(arg))
                    throw Error("unknown record");
                auto o = std::make_shared<Object>();
                o->type = arg;
                auto &fields = m.schemas.at(arg).fields;
                for (size_t i = fields.size(); i > 0; --i)
                    o->fields[fields[i - 1].first] = pop();
                stack.emplace_back(o);
            } else if (op == "FIELD") {
                auto v = pop();
                auto o = std::get<std::shared_ptr<Object>>(v.data);
                if (!o->fields.count(arg))
                    throw Error("unknown field");
                stack.push_back(o->fields.at(arg));
            } else if (op == "ASSERT") {
                if (!boolean(pop()))
                    throw Error("ContractViolation: " + arg);
            } else if (op == "WRITE") {
                if (stack.empty())
                    throw Error("missing state value");
                write(arg, stack.back());
            } else if (op == "RET") {
                auto result = pop();
                if (!stack.empty())
                    throw Error("unbalanced return stack");
                return result;
            } else if (op == "NOT")
                stack.emplace_back(!boolean(pop()));
            else if (op == "NEG") {
                auto a = pop();
                if (auto x = std::get_if<std::int64_t>(&a.data)) {
                    if (*x == std::numeric_limits<std::int64_t>::min())
                        throw Error("integer overflow");
                    stack.emplace_back(-*x);
                } else
                    stack.emplace_back(-std::get<double>(a.data));
            } else if (op == "LEN")
                stack.emplace_back(std::int64_t(text_value(pop()).size()));
            else if (op == "STR")
                stack.emplace_back(std::to_string(integer(pop())));
            else if (op == "PARSEINT")
                stack.push_back(parse_value(trim(text_value(pop())), "Int"));
            else if (op == "READLINE") {
                std::cout.flush();
                std::string input;
                if (!std::getline(std::cin, input))
                    throw Error("IOError: input ended before a line was read");
                stack.emplace_back(std::move(input));
            } else if (op == "PRINT") {
                if (stack.empty())
                    throw Error("print underflow");
                std::cout << text_value(stack.back()) << '\n';
            } else if (op == "SELECT") {
                auto no = pop(), yes = pop(), condition = pop();
                stack.push_back(boolean(condition) ? yes : no);
            } else if (op=="TWEIGHT"||op=="TLINEAR"||op=="TWEMBED"||op=="TBIAS"||op=="TWNORM"||op=="TMROPE"||
                       op=="TMROPEAT"||op=="TGQA"||op=="TGQACACHE")
                stack.push_back(model_tensor_op(op,stack,*tensor_context));
            else if (op.size() > 1 && op[0] == 'T' && op != "TEXT")
                stack.push_back(tensor_op(op, stack));
            else {
                auto b = pop(), a = pop();
                stack.push_back(binary(op, a, b));
            }
        }
        throw Error("function missing return");
    }

  public:
    VM(const Module &mod, const std::string &path, TensorContext* context) : m(mod), tensor_context(context?context:&owned_context) {
        if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
            std::string e = db ? sqlite3_errmsg(db) : "open failed";
            if (db)
                sqlite3_close(db);
            db = nullptr;
            throw Error("DBError: " + e);
        }
        try {
            sqlite3_busy_timeout(db, 2000);
            for (auto &[name, s] : m.schemas) {
                if (s.storage.empty())
                    continue;
                std::string q = "CREATE " + std::string(s.storage == "temporary" ? "TEMP " : "") +
                                "TABLE IF NOT EXISTS " + qi(name) + " (";
                bool first = true;
                for (auto &[f, t] : s.fields) {
                    if (!first)
                        q += ",";
                    first = false;
                    q += qi(f) + " " +
                         (t == "Text"    ? "TEXT"
                          : t == "Float" ? "REAL"
                                         : "INTEGER") +
                         " NOT NULL";
                    for (auto &u : s.unique)
                        if (u == f)
                            q += " UNIQUE";
                }
                q += ")";
                sql(q);
            }
        } catch (...) {
            sqlite3_close(db);
            db = nullptr;
            throw;
        }
    }
    ~VM() {
        if (db)
            sqlite3_close(db);
    }
    Value execute(const std::vector<Value> &args) {
        if (m.atomic)
            sql("BEGIN IMMEDIATE");
        try {
            auto v = call("@entry", args, 0);
            if (m.atomic)
                sql("COMMIT");
            return v;
        } catch (...) {
            if (m.atomic)
                sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }
};
} // namespace
Value run(const Module &m, const std::vector<Value> &args, const std::string &path, TensorContext* context) {
    return VM(m, path, context).execute(args);
}
} // namespace glyph::runtime
