// macro.cpp — the compile-time Lisp interpreter (see macro.h).

#include "macro.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

struct Meta::Env {
    std::map<std::string, Form *> vars;
    Env *parent = nullptr;
    Form **find(const std::string &n) {
        for (Env *e = this; e; e = e->parent) {
            auto it = e->vars.find(n);
            if (it != e->vars.end()) return &it->second;
        }
        return nullptr;
    }
};

struct Meta::Closure {
    std::string name;
    Form *params;  // a parameter list; nested lists destructure, &rest/&body takes the rest
    Form *body;    // the whole definition; body forms from `first`
    size_t first;
    Env *env;
};

namespace {

Form *mk_int(Loc at, int64_t v) {
    Form *f = new Form{Form::Int, at};
    f->ival = (uint64_t)v;
    f->text = std::to_string(v);
    return f;
}
Form *mk_float(Loc at, double v) {
    Form *f = new Form{Form::Float, at};
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", v);
    f->text = buf;
    if (f->text.find_first_of(".en") == std::string::npos) f->text += ".0";
    return f;
}
Form *mk_str(Loc at, const std::string &s) {
    Form *f = new Form{Form::Str, at};
    f->text = s;
    return f;
}
Form *mk_bool(Loc at, bool v) { return mk_sym(at, v ? "true" : "false"); }

bool truthy(Form *f) {
    if (f->kind == Form::List) return !f->items.empty();
    return !f->is_sym("false");
}
double num(Form *f, Loc at) {
    if (f->kind == Form::Int) return (double)(int64_t)f->ival;
    if (f->kind == Form::Float) return strtod(f->text.c_str(), nullptr);
    fatal(at, "expected a number, got %s", show(f).c_str());
}
bool equal(Form *a, Form *b) {
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case Form::Int: return a->ival == b->ival;
    case Form::Float: return num(a, a->loc) == num(b, b->loc);
    case Form::Sym: case Form::Str: return a->text == b->text;
    case Form::Fn: return a->fn == b->fn;
    case Form::List:
        if (a->items.size() != b->items.size()) return false;
        for (size_t i = 0; i < a->items.size(); i++)
            if (!equal(a->items[i], b->items[i])) return false;
        return true;
    }
    return false;
}
// text of a symbol, string or number, for sym/str
std::string piece(Form *f, Loc at) {
    if (f->kind == Form::Sym || f->kind == Form::Str || f->kind == Form::Int || f->kind == Form::Float) return f->text;
    fatal(at, "cannot make a name from %s", show(f).c_str());
}
Form *list_arg(Form *f, Loc at) {
    if (f->kind != Form::List) fatal(at, "expected a list, got %s", show(f).c_str());
    return f;
}

}  // namespace

Meta::Meta(MetaHost &host) : host_(host) {
    auto &B = builtins_;
    auto need = [](std::vector<Form *> &a, size_t n, Loc at, const char *name) {
        if (a.size() != n) fatal(at, "%s takes %zu argument%s", name, n, n == 1 ? "" : "s");
    };
    B["list"] = [](std::vector<Form *> &a, Loc at) { return mk_list(at, a); };
    B["cons"] = [need](std::vector<Form *> &a, Loc at) {
        need(a, 2, at, "cons");
        std::vector<Form *> items{a[0]};
        for (Form *x : list_arg(a[1], at)->items) items.push_back(x);
        return mk_list(at, items);
    };
    B["first"] = [need](std::vector<Form *> &a, Loc at) {
        need(a, 1, at, "first");
        Form *l = list_arg(a[0], at);
        if (l->items.empty()) fatal(at, "first of an empty list");
        return l->items[0];
    };
    B["rest"] = [need](std::vector<Form *> &a, Loc at) {
        need(a, 1, at, "rest");
        Form *l = list_arg(a[0], at);
        std::vector<Form *> items;
        if (!l->items.empty()) items.assign(l->items.begin() + 1, l->items.end());
        return mk_list(at, items);
    };
    B["nth"] = [need](std::vector<Form *> &a, Loc at) {
        need(a, 2, at, "nth");
        Form *l = list_arg(a[0], at);
        size_t i = (size_t)num(a[1], at);
        if (i >= l->items.size()) fatal(at, "nth: index %zu of a %zu-element list", i, l->items.size());
        return l->items[i];
    };
    B["length"] = [need](std::vector<Form *> &a, Loc at) {
        need(a, 1, at, "length");
        if (a[0]->kind == Form::Str) return mk_int(at, (int64_t)a[0]->text.size());
        return mk_int(at, (int64_t)list_arg(a[0], at)->items.size());
    };
    B["append"] = [](std::vector<Form *> &a, Loc at) {
        std::vector<Form *> items;
        for (Form *l : a)
            for (Form *x : list_arg(l, at)->items) items.push_back(x);
        return mk_list(at, items);
    };
    B["reverse"] = [need](std::vector<Form *> &a, Loc at) {
        need(a, 1, at, "reverse");
        auto items = list_arg(a[0], at)->items;
        return mk_list(at, std::vector<Form *>(items.rbegin(), items.rend()));
    };
    B["range"] = [](std::vector<Form *> &a, Loc at) {  // (range n) or (range from to)
        if (a.empty() || a.size() > 2) fatal(at, "(range n) or (range from to)");
        int64_t lo = a.size() == 2 ? (int64_t)num(a[0], at) : 0, hi = (int64_t)num(a.back(), at);
        std::vector<Form *> items;
        for (int64_t i = lo; i < hi; i++) items.push_back(mk_int(at, i));
        return mk_list(at, items);
    };
    B["empty?"] = [need](std::vector<Form *> &a, Loc at) { need(a, 1, at, "empty?"); return mk_bool(at, list_arg(a[0], at)->items.empty()); };
    B["symbol?"] = [](std::vector<Form *> &a, Loc at) { return mk_bool(at, a.size() == 1 && a[0]->kind == Form::Sym); };
    B["list?"] = [](std::vector<Form *> &a, Loc at) { return mk_bool(at, a.size() == 1 && a[0]->kind == Form::List); };
    B["int?"] = [](std::vector<Form *> &a, Loc at) { return mk_bool(at, a.size() == 1 && a[0]->kind == Form::Int); };
    B["float?"] = [](std::vector<Form *> &a, Loc at) { return mk_bool(at, a.size() == 1 && a[0]->kind == Form::Float); };
    B["string?"] = [](std::vector<Form *> &a, Loc at) { return mk_bool(at, a.size() == 1 && a[0]->kind == Form::Str); };
    B["eq"] = [need](std::vector<Form *> &a, Loc at) { need(a, 2, at, "eq"); return mk_bool(at, equal(a[0], a[1])); };
    B["not"] = [need](std::vector<Form *> &a, Loc at) { need(a, 1, at, "not"); return mk_bool(at, !truthy(a[0])); };
    // arithmetic: integers stay integers
    auto arith = [](char op) {
        return [op](std::vector<Form *> &a, Loc at) -> Form * {
            if (a.empty()) fatal(at, "(%c ...) needs operands", op);
            bool ints = true;
            for (Form *x : a) ints = ints && x->kind == Form::Int;
            if (a.size() == 1 && op == '-') return ints ? mk_int(at, -(int64_t)a[0]->ival) : mk_float(at, -num(a[0], at));
            if (ints) {
                int64_t v = (int64_t)a[0]->ival;
                for (size_t i = 1; i < a.size(); i++) {
                    int64_t x = (int64_t)a[i]->ival;
                    if ((op == '/' || op == '%') && x == 0) fatal(at, "division by zero");
                    v = op == '+' ? v + x : op == '-' ? v - x : op == '*' ? v * x : op == '/' ? v / x : v % x;
                }
                return mk_int(at, v);
            }
            double v = num(a[0], at);
            for (size_t i = 1; i < a.size(); i++) {
                double x = num(a[i], at);
                v = op == '+' ? v + x : op == '-' ? v - x : op == '*' ? v * x : op == '/' ? v / x : std::fmod(v, x);
            }
            return mk_float(at, v);
        };
    };
    for (char op : std::string("+-*/%")) B[std::string(1, op)] = arith(op);
    auto cmp = [need](const std::string &op) {
        return [op, need](std::vector<Form *> &a, Loc at) {
            need(a, 2, at, op.c_str());
            double x = num(a[0], at), y = num(a[1], at);
            bool r = op == "=" ? x == y : op == "!=" ? x != y : op == "<" ? x < y : op == "<=" ? x <= y : op == ">" ? x > y : x >= y;
            return mk_bool(at, r);
        };
    };
    for (const char *op : {"=", "!=", "<", "<=", ">", ">="}) B[op] = cmp(op);
    B["sym"] = [](std::vector<Form *> &a, Loc at) {
        std::string s;
        for (Form *x : a) s += piece(x, at);
        return mk_sym(at, s);
    };
    B["str"] = [](std::vector<Form *> &a, Loc at) {
        std::string s;
        for (Form *x : a) s += x->kind == Form::List ? show(x) : piece(x, at);
        return mk_str(at, s);
    };
    B["symbol-name"] = [need](std::vector<Form *> &a, Loc at) {
        need(a, 1, at, "symbol-name");
        if (a[0]->kind != Form::Sym) fatal(at, "symbol-name of %s", show(a[0]).c_str());
        return mk_str(at, a[0]->text);
    };
    B["gensym"] = [this](std::vector<Form *> &a, Loc at) {
        std::string base = a.empty() ? "g" : piece(a[0], at);
        return mk_sym(at, "__" + base + std::to_string(++gensym_));
    };
    B["error"] = [this](std::vector<Form *> &a, Loc at) -> Form * {
        std::string s;
        for (Form *x : a) s += x->kind == Form::Str ? x->text : show(x);
        (void)at;
        fatal(call_at_, "%s", s.c_str());
    };
    B["print"] = [](std::vector<Form *> &a, Loc at) {
        for (size_t i = 0; i < a.size(); i++) fprintf(stderr, "%s%s", i ? " " : "", a[i]->kind == Form::Str ? a[i]->text.c_str() : show(a[i]).c_str());
        fputc('\n', stderr);
        return mk_list(at, {});
    };
    // types
    B["type-of"] = [this, need](std::vector<Form *> &a, Loc at) {
        need(a, 1, at, "type-of");
        return type_form(host_.type_of(a[0]), at);
    };
    auto type_arg = [this, need](std::vector<Form *> &a, Loc at, const char *name) {
        need(a, 1, at, name);
        return host_.parse_type(a[0]);
    };
    B["type-kind"] = [type_arg](std::vector<Form *> &a, Loc at) {
        static const char *const names[] = {"void", "bool", "int", "float", "ptr", "array", "struct", "fn"};
        return mk_sym(at, names[type_arg(a, at, "type-kind")->kind]);
    };
    B["type-elem"] = [this, type_arg](std::vector<Form *> &a, Loc at) {
        Type *t = type_arg(a, at, "type-elem");
        if (!t->elem) fatal(at, "%s has no element type", type_name(t).c_str());
        return type_form(t->elem, at);
    };
    B["type-len"] = [type_arg](std::vector<Form *> &a, Loc at) {
        Type *t = type_arg(a, at, "type-len");
        if (t->kind != Type::Array) fatal(at, "%s is not an array", type_name(t).c_str());
        return mk_int(at, (int64_t)t->n);
    };
    B["type-size"] = [type_arg](std::vector<Form *> &a, Loc at) { return mk_int(at, (int64_t)size_of(type_arg(a, at, "type-size"))); };
    B["type-fields"] = [this, type_arg](std::vector<Form *> &a, Loc at) {
        Type *t = type_arg(a, at, "type-fields");
        if (t->kind != Type::Struct || !t->st->complete) fatal(at, "%s is not a struct with known fields", type_name(t).c_str());
        std::vector<Form *> fs;
        for (auto &f : t->st->fields) fs.push_back(mk_list(at, {mk_sym(at, f.name), type_form(f.type, at)}));
        return mk_list(at, fs);
    };
}

Form *Meta::type_form(Type *t, Loc at) {
    switch (t->kind) {
    case Type::Ptr: return mk_list(at, {mk_sym(at, "ptr"), type_form(t->elem, at)});
    case Type::Array: return mk_list(at, {mk_sym(at, "array"), type_form(t->elem, at), mk_int(at, (int64_t)t->n)});
    case Type::Fn: {
        std::vector<Form *> ps;
        for (Type *p : t->params) ps.push_back(type_form(p, at));
        if (t->variadic) ps.push_back(mk_sym(at, "..."));
        return mk_list(at, {mk_sym(at, "fn"), mk_list(at, ps), type_form(t->elem, at)});
    }
    default: return mk_sym(at, type_name(t));
    }
}

// bind a parameter pattern to a value: a name binds it, a list destructures
// (not called "bind": with a std::string argument, ADL would find std::bind)
static void bind_params(Form *pat, Form *val, Meta::Env *env, const std::string &fn, Loc at);

void Meta::define(Form *def) {
    bool macro = def->is_call("defmacro");
    if (def->size() < 3 || def->at(1)->kind != Form::Sym || !def->at(2)->is_list())
        fatal(def->loc, "(%s name (params...) body...)", macro ? "defmacro" : "defmeta");
    auto *c = new Closure{def->at(1)->text, def->at(2), def, 3, nullptr};
    auto &table = macro ? macros_ : metas_;
    if (table.count(c->name)) fatal(def->loc, "'%s' is already defined", c->name.c_str());
    table[c->name] = c;
}

Form *Meta::expand(Form *call) {
    Closure *m = macros_.at(call->head());
    Loc saved = call_at_;
    unsigned saved_mark = mark_;
    call_at_ = call->loc;
    mark_ = ++marks_;  // a fresh mark per expansion
    std::vector<Form *> args(call->items.begin() + 1, call->items.end());  // unevaluated: macros get forms
    Form *r = call_closure(m, args, call->loc);
    call_at_ = saved;
    mark_ = saved_mark;
    return r;
}

static void bind_params(Form *pat, Form *val, Meta::Env *env, const std::string &fn, Loc at) {
    if (pat->kind == Form::Sym) {
        env->vars[pat->text] = val;
        return;
    }
    if (pat->kind != Form::List) fatal(pat->loc, "parameters are names or lists of them");
    if (val->kind != Form::List) fatal(at, "%s: expected a list for %s, got %s", fn.c_str(), show(pat).c_str(), show(val).c_str());
    auto &ps = pat->items;
    size_t n = ps.size();
    bool rest = n >= 2 && (ps[n - 2]->is_sym("&rest") || ps[n - 2]->is_sym("&body"));
    size_t fixed = rest ? n - 2 : n;
    if (val->size() < fixed || (val->size() > fixed && !rest))
        fatal(at, "%s takes %s%zu argument%s for %s, got %zu", fn.c_str(), rest ? "at least " : "", fixed, fixed == 1 ? "" : "s",
              show(pat).c_str(), val->size());
    for (size_t i = 0; i < fixed; i++) bind_params(ps[i], val->at(i), env, fn, at);
    if (rest) bind_params(ps[n - 1], mk_list(at, std::vector<Form *>(val->items.begin() + fixed, val->items.end())), env, fn, at);
}

Form *Meta::call_closure(Closure *c, const std::vector<Form *> &args, Loc at) {
    Env *env = new Env{{}, c->env};
    bind_params(c->params, mk_list(at, args), env, c->name, at);
    Form *r = mk_list(at, {});
    for (size_t i = c->first; i < c->body->size(); i++) r = eval(c->body->at(i), env);
    return r;
}

Form *Meta::apply(Form *fn, const std::vector<Form *> &args, Loc at) {
    if (fn->kind == Form::Fn) return call_closure((Closure *)fn->fn, args, at);
    if (fn->kind == Form::Sym) {
        auto b = builtins_.find(fn->text);
        if (b != builtins_.end()) {
            std::vector<Form *> a = args;
            return b->second(a, at);
        }
        auto m = metas_.find(fn->text);
        if (m != metas_.end()) return call_closure(m->second, args, at);
    }
    fatal(at, "%s is not a function", show(fn).c_str());
}

Form *Meta::quasi(Form *f, Env *env, int depth) {
    if (f->kind == Form::Sym && mark_) {  // hygiene: the template's own symbols get this expansion's mark
        Form *m = mk_sym(f->loc, f->text);
        m->mark = mark_;
        return m;
    }
    if (f->kind != Form::List) return f;
    if (f->is_call("unquote") && f->size() == 2) {
        if (depth == 1) return eval(f->at(1), env);
        return mk_list(f->loc, {f->at(0), quasi(f->at(1), env, depth - 1)});
    }
    if (f->is_call("quasiquote") && f->size() == 2) return mk_list(f->loc, {f->at(0), quasi(f->at(1), env, depth + 1)});
    std::vector<Form *> items;
    for (Form *x : f->items) {
        if (depth == 1 && x->is_call("unquote-splicing") && x->size() == 2) {
            Form *l = eval(x->at(1), env);
            if (l->kind != Form::List) fatal(x->loc, ",@ needs a list, got %s", show(l).c_str());
            items.insert(items.end(), l->items.begin(), l->items.end());
        } else items.push_back(quasi(x, env, depth));
    }
    return mk_list(f->loc, items);
}

Form *Meta::eval(Form *f, Env *env) {
    switch (f->kind) {
    case Form::Int: case Form::Float: case Form::Str: case Form::Fn: return f;
    case Form::Sym: {
        if (f->text == "true" || f->text == "false") return f;
        if (f->text == "nil") return mk_list(f->loc, {});
        if (Form **v = env->find(f->text)) return *v;
        if (builtins_.count(f->text) || metas_.count(f->text)) return f;  // a function, by name
        fatal(f->loc, "unknown name '%s' in a macro", f->text.c_str());
    }
    case Form::List: break;
    }
    if (f->items.empty()) return f;
    std::string h = f->head();
    auto body = [&](size_t from, Env *e) {
        Form *r = mk_list(f->loc, {});
        for (size_t i = from; i < f->size(); i++) r = eval(f->at(i), e);
        return r;
    };
    if (h == "quote") {
        if (f->size() != 2) fatal(f->loc, "(quote x)");
        return f->at(1);
    }
    if (h == "quasiquote") {
        if (f->size() != 2) fatal(f->loc, "(quasiquote x)");
        return quasi(f->at(1), env, 1);
    }
    if (h == "if") {
        if (f->size() != 3 && f->size() != 4) fatal(f->loc, "(if c a [b])");
        if (truthy(eval(f->at(1), env))) return eval(f->at(2), env);
        return f->size() == 4 ? eval(f->at(3), env) : mk_list(f->loc, {});
    }
    if (h == "when" || h == "unless") {
        if (f->size() < 2) fatal(f->loc, "(%s c body...)", h.c_str());
        if (truthy(eval(f->at(1), env)) == (h == "when")) return body(2, env);
        return mk_list(f->loc, {});
    }
    if (h == "cond") {
        for (size_t i = 1; i < f->size(); i++) {
            Form *cl = f->at(i);
            if (!cl->is_list() || cl->items.empty()) fatal(cl->loc, "cond clause must be (test body...)");
            if (cl->at(0)->is_sym("else") || truthy(eval(cl->at(0), env))) {
                Form *r = mk_list(cl->loc, {});
                for (size_t k = 1; k < cl->size(); k++) r = eval(cl->at(k), env);
                return r;
            }
        }
        return mk_list(f->loc, {});
    }
    if (h == "do") return body(1, env);
    if (h == "let") {
        if (f->size() < 2 || !f->at(1)->is_list()) fatal(f->loc, "(let ((x e) ...) body...)");
        Env *e = new Env{{}, env};
        for (Form *b : f->at(1)->items) {
            if (!b->is_list() || b->size() != 2 || b->at(0)->kind != Form::Sym) fatal(b->loc, "binding must be (name value)");
            e->vars[b->at(0)->text] = eval(b->at(1), e);
        }
        return body(2, e);
    }
    if (h == "set") {
        if (f->size() != 3 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(set name value)");
        Form **v = env->find(f->at(1)->text);
        if (!v) fatal(f->at(1)->loc, "unknown name '%s'", f->at(1)->text.c_str());
        return *v = eval(f->at(2), env);
    }
    if (h == "while") {
        if (f->size() < 2) fatal(f->loc, "(while c body...)");
        while (truthy(eval(f->at(1), env))) body(2, env);
        return mk_list(f->loc, {});
    }
    if (h == "and" || h == "or") {
        Form *r = mk_bool(f->loc, h == "and");
        for (size_t i = 1; i < f->size(); i++) {
            r = eval(f->at(i), env);
            if (truthy(r) != (h == "and")) return r;
        }
        return r;
    }
    if (h == "lambda") {
        if (f->size() < 3 || !f->at(1)->is_list()) fatal(f->loc, "(lambda (params...) body...)");
        auto *c = new Closure{"lambda", f->at(1), f, 2, env};
        Form *fn = new Form{Form::Fn, f->loc};
        fn->fn = c;
        return fn;
    }
    if (h == "map" || h == "filter" || h == "mapcat") {
        if (f->size() != 3) fatal(f->loc, "(%s fn list)", h.c_str());
        Form *fn = eval(f->at(1), env), *l = list_arg(eval(f->at(2), env), f->loc);
        std::vector<Form *> out;
        for (Form *x : l->items) {
            Form *r = apply(fn, {x}, f->loc);
            if (h == "map") out.push_back(r);
            else if (h == "mapcat") {
                if (r->kind != Form::List) fatal(f->loc, "mapcat: the function must return lists, got %s", show(r).c_str());
                out.insert(out.end(), r->items.begin(), r->items.end());
            } else if (truthy(r)) out.push_back(x);
        }
        return mk_list(f->loc, out);
    }
    // a call
    Form *fn = eval(f->at(0), env);
    std::vector<Form *> args;
    for (size_t i = 1; i < f->size(); i++) args.push_back(eval(f->at(i), env));
    return apply(fn, args, f->loc);
}
