// compiler.cpp — type checking and LLVM code generation, in one pass over the
// typed core forms.
//
// Top level:
//   (c-include "h" ...)              C declarations from headers (cimport.cpp)
//   (struct name (field T) ...)      also (union name ...)
//   (const NAME expr)                a compile-time constant; untyped if expr is
//   (def name T [init])              a global variable
//   (defn name ((a T) ...) R body...)
//   (extern name (T ... [...]) R)    a C function by hand
//
// Expressions (everything is an expression; statements have type void):
//   literals, names, (f args...), (do e...), (let ((x [T] [init]) ...) body...)
//   (set place e)  (if c a [b])  (when c body...)  (unless c body...)
//   (while c body...)  (for (i start end [step]) body...)  (break)  (continue)  (return [e])
//   (+ - * / % & | ^ << >>)  (- x)  (~ x)  (= != < <= > >=)  (and ...) (or ...) (not x)
//   (. x field ...)  (aref a i)  (deref p)  (addr place)  (cast T e)  (bitcast T e)
//   (sizeof T)  (alignof T)  (make T [:field e ...] | [e ...])
//
// Literals are untyped until used: 1.5 becomes an f32 where an f32 is
// expected (rounded straight from the decimal text, like C's 1.5f), and an
// f64 otherwise. Mixing typed operands of different types is an error; use
// (cast T e).

#include "compiler.h"
#include "macro.h"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

using llvm::BasicBlock;

namespace {

struct Val {
    llvm::Value *v = nullptr;
    Type *t = nullptr;
    uint64_t align = 0;  // for addresses: known alignment (0 = the type's)
    // a bit field: v is the address of the bytes holding it, read as one integer
    llvm::IntegerType *bwin = nullptr;
    unsigned bshift = 0, bwidth = 0;
};

// How a value crosses a call, per the x86-64 System V ABI.
struct ArgAbi {
    enum Kind { Direct, Coerce, Memory, Ignore } kind = Direct;
    std::vector<llvm::Type *> parts;  // Coerce: one register-sized piece per eightbyte
};
struct FnAbi {
    ArgAbi ret;
    std::vector<ArgAbi> params;
    llvm::FunctionType *ty = nullptr;
    llvm::AttributeList attrs;
};

struct Loop {
    BasicBlock *cont, *brk;
};

const char *const ARITH[] = {"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"};
const char *const CMP[] = {"=", "!=", "<", "<=", ">", ">="};

bool in(const char *s, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(s, list[i])) return true;
    return false;
}
// A local variable's name in the scopes: the symbol's text, plus its
// hygiene mark if a macro template introduced it. Globals, types, fields and
// keywords are looked up by text alone, so a template's free names mean the
// global definitions, never a local of the code the macro is used in.
std::string key(const Form *sym) { return sym->mark ? sym->text + "\x01" + std::to_string(sym->mark) : sym->text; }

bool is_arith(const char *s) { return in(s, ARITH, sizeof ARITH / sizeof *ARITH); }
bool is_cmp(const char *s) { return in(s, CMP, sizeof CMP / sizeof *CMP); }

}  // namespace

struct Compiler::Impl : MetaHost {
    Compiler &c;
    llvm::LLVMContext &ctx;
    llvm::Module &mod;
    llvm::IRBuilder<> b;
    llvm::Function *fn = nullptr;
    Type *fn_ret = nullptr;
    Type *cur_ft = nullptr;            // the current function's type
    llvm::Value *cur_sret = nullptr;   // where a struct result returned in memory goes
    std::vector<std::map<std::string, Val>> scopes;  // locals: name → address
    std::vector<Loop> loops;
    std::map<StructInfo *, bool> laying_out;          // cycle guard
    Meta meta{*this};
    // defop: operator → its definitions (functions named like "+(v3,v3)")
    std::map<std::string, std::vector<Global *>> overloads;

    Impl(Compiler &c) : c(c), ctx(*c.ctx_), mod(*c.mod_), b(ctx) {}

    /* -------------------------------------------------------- macros --- */

    // f with its macro calls at the head expanded (repeatedly)
    Form *expand(Form *f) {
        while (f->kind == Form::List && !f->items.empty() && f->at(0)->kind == Form::Sym && meta.is_macro(f->at(0)->text))
            f = meta.expand(f);
        return f;
    }

    // the type of an expression where it stands, without emitting code: it is
    // compiled into a scratch block that is thrown away
    Type *type_of(Form *f) override {
        if (!fn) fatal(f->loc, "type-of works only inside functions");
        llvm::IRBuilderBase::InsertPoint ip = b.saveIP();
        size_t nblocks = fn->size();
        b.SetInsertPoint(BasicBlock::Create(ctx, "typeof", fn));
        Type *t = expr(f, nullptr).t;
        std::vector<BasicBlock *> made;
        size_t i = 0;
        for (BasicBlock &bb : *fn)
            if (i++ >= nblocks) made.push_back(&bb);
        for (BasicBlock *bb : made) bb->dropAllReferences();
        for (BasicBlock *bb : made) bb->eraseFromParent();
        b.restoreIP(ip);
        return t;
    }

    /* --------------------------------------------------------- types --- */

    llvm::Type *ll(Type *t) {
        switch (t->kind) {
        case Type::Void: return b.getVoidTy();
        case Type::Bool: return b.getInt1Ty();
        case Type::Int: return b.getIntNTy(t->bits);
        case Type::Float: return t->bits == 32 ? b.getFloatTy() : b.getDoubleTy();
        case Type::Ptr: return b.getPtrTy();
        case Type::Array: return llvm::ArrayType::get(ll(t->elem), t->n);
        case Type::Struct: return ll_struct(t->st);
        case Type::Fn: return fn_abi(t).ty;
        }
        return nullptr;
    }

    /* ------------------------------------------- calling convention --- */
    // Every function, lisc's own included, uses the C calling convention, so
    // C can call lisc functions through pointers. Scalars pass directly
    // (small integers extended, as C expects); a struct of up to 16 bytes
    // passes as one or two register-sized pieces, each INTEGER or SSE by what
    // it holds; bigger structs (or ones that no longer fit in the remaining
    // registers) go through memory: a copy on the stack (byval) for arguments,
    // a pointer to the caller's space (sret) for results.

    std::map<Type *, FnAbi> abis;

    // the scalars inside t, with their byte offsets
    void leaves(Type *t, uint64_t off, std::vector<std::pair<uint64_t, Type *>> &out) {
        if (t->kind == Type::Struct) {
            for (auto &f : t->st->fields) {
                if (f.bit_width) {
                    for (uint64_t b0 = f.bit_off / 8; b0 <= (f.bit_off + f.bit_width - 1) / 8; b0++) out.push_back({off + b0, t_int(8, false)});
                } else leaves(f.type, off + f.offset, out);
            }
        } else if (t->kind == Type::Array) {
            for (uint64_t i = 0; i < t->n; i++) leaves(t->elem, off + i * size_of(t->elem), out);
        } else out.push_back({off, t});
    }

    ArgAbi classify(Type *t) {
        ArgAbi a;
        if (t->kind != Type::Struct) return a;  // Direct
        uint64_t size = size_of(t);
        if (size == 0) { a.kind = ArgAbi::Ignore; return a; }
        a.kind = ArgAbi::Memory;
        if (size > 16) return a;
        enum { NONE, INT, SSE } cls[2] = {NONE, NONE};
        std::vector<std::pair<uint64_t, Type *>> ls;
        leaves(t, 0, ls);
        for (auto &l : ls) {
            uint64_t eb = l.first / 8;
            if ((l.first + size_of(l.second) - 1) / 8 != eb) return a;  // straddles two eightbytes
            auto c = l.second->is_float() ? SSE : INT;
            if (cls[eb] == NONE || (cls[eb] == SSE && c == INT)) cls[eb] = c;
        }
        a.kind = ArgAbi::Coerce;
        for (uint64_t eb = 0; eb * 8 < size; eb++) {
            uint64_t bytes = std::min<uint64_t>(8, size - eb * 8);
            if (cls[eb] != SSE) { a.parts.push_back(b.getIntNTy(8 * bytes)); continue; }
            bool dbl = false, f0 = false, f4 = false;
            for (auto &l : ls) {
                if (l.first / 8 != eb) continue;
                if (l.second->bits == 64) dbl = true;
                else if (l.first % 8 == 0) f0 = true;
                else f4 = true;
            }
            if (dbl) a.parts.push_back(b.getDoubleTy());
            else if (f0 && !f4) a.parts.push_back(b.getFloatTy());
            else a.parts.push_back(llvm::FixedVectorType::get(b.getFloatTy(), 2));
        }
        return a;
    }

    FnAbi &fn_abi(Type *ft) {
        auto it = abis.find(ft);
        if (it != abis.end()) return it->second;
        FnAbi a;
        std::vector<llvm::Type *> irps;
        std::vector<llvm::AttributeSet> sets;  // per IR parameter
        llvm::AttrBuilder rattrs(ctx);
        int gpr = 6, sse = 8;  // argument registers left
        auto ext = [&](llvm::AttrBuilder &ab, Type *t) {
            if (t->kind == Type::Bool || (t->is_int() && t->bits < 32)) ab.addAttribute(t->kind == Type::Int && t->sign ? llvm::Attribute::SExt : llvm::Attribute::ZExt);
        };
        Type *rt = ft->elem;
        llvm::Type *irret = rt == t_void() ? b.getVoidTy() : nullptr;
        if (rt != t_void()) {
            a.ret = classify(rt);
            switch (a.ret.kind) {
            case ArgAbi::Direct: irret = ll(rt); ext(rattrs, rt); break;
            case ArgAbi::Ignore: irret = b.getVoidTy(); break;
            case ArgAbi::Coerce:
                irret = a.ret.parts.size() == 1 ? a.ret.parts[0] : llvm::StructType::get(ctx, a.ret.parts);
                break;
            case ArgAbi::Memory: {
                irret = b.getVoidTy();
                irps.push_back(b.getPtrTy());
                llvm::AttrBuilder ab(ctx);
                ab.addStructRetAttr(ll(rt));
                ab.addAlignmentAttr(align_of(rt));
                ab.addAttribute(llvm::Attribute::NoAlias);
                sets.push_back(llvm::AttributeSet::get(ctx, ab));
                gpr--;
                break;
            }
            }
        }
        for (Type *p : ft->params) {
            ArgAbi pa = classify(p);
            if (pa.kind == ArgAbi::Coerce) {
                int need_int = 0, need_sse = 0;
                for (llvm::Type *part : pa.parts) (part->isIntegerTy() ? need_int : need_sse)++;
                if (need_int <= gpr && need_sse <= sse) { gpr -= need_int; sse -= need_sse; }
                else { pa.kind = ArgAbi::Memory; pa.parts.clear(); }
            }
            switch (pa.kind) {
            case ArgAbi::Direct: {
                irps.push_back(ll(p));
                llvm::AttrBuilder ab(ctx);
                ext(ab, p);
                sets.push_back(llvm::AttributeSet::get(ctx, ab));
                if (p->is_float()) sse--; else gpr--;
                break;
            }
            case ArgAbi::Coerce:
                for (llvm::Type *part : pa.parts) {
                    irps.push_back(part);
                    sets.emplace_back();
                }
                break;
            case ArgAbi::Memory: {
                irps.push_back(b.getPtrTy());
                llvm::AttrBuilder ab(ctx);
                ab.addByValAttr(ll(p));
                ab.addAlignmentAttr(std::max<uint64_t>(8, align_of(p)));
                sets.push_back(llvm::AttributeSet::get(ctx, ab));
                break;
            }
            case ArgAbi::Ignore: break;
            }
            a.params.push_back(pa);
        }
        a.ty = llvm::FunctionType::get(irret, irps, ft->variadic);
        a.attrs = llvm::AttributeList::get(ctx, llvm::AttributeSet(), llvm::AttributeSet::get(ctx, rattrs), sets);
        return abis[ft] = a;
    }

    // byte offset `off` inside the memory at p
    llvm::Value *at_offset(llvm::Value *p, uint64_t off) { return off ? b.CreateInBoundsGEP(b.getInt8Ty(), p, b.getInt64(off)) : p; }

    // a struct value as its register pieces
    void coerce_out(const ArgAbi &abi, Val v, std::vector<llvm::Value *> &out) {
        Val m = spill(v);
        for (size_t i = 0; i < abi.parts.size(); i++)
            out.push_back(b.CreateAlignedLoad(abi.parts[i], at_offset(m.v, 8 * i), llvm::Align(std::min<uint64_t>(8, align_of(v.t)))));
    }
    // register pieces stored into a struct's memory
    void coerce_in(const ArgAbi &abi, const std::vector<llvm::Value *> &parts, llvm::Value *mem, Type *t) {
        for (size_t i = 0; i < parts.size(); i++)
            b.CreateAlignedStore(parts[i], at_offset(mem, 8 * i), llvm::Align(std::min<uint64_t>(8, align_of(t))));
        (void)abi;
    }

    // return v from the current function, lowered per its ABI
    void emit_ret(Val v) {
        const ArgAbi &r = fn_abi(cur_ft).ret;
        switch (r.kind) {
        case ArgAbi::Direct: b.CreateRet(v.v); break;
        case ArgAbi::Ignore: b.CreateRetVoid(); break;
        case ArgAbi::Memory:
            store({cur_sret, v.t}, v.v);
            b.CreateRetVoid();
            break;
        case ArgAbi::Coerce: {
            std::vector<llvm::Value *> parts;
            coerce_out(r, v, parts);
            if (parts.size() == 1) { b.CreateRet(parts[0]); break; }
            llvm::Value *agg = llvm::UndefValue::get(fn->getReturnType());
            for (unsigned i = 0; i < parts.size(); i++) agg = b.CreateInsertValue(agg, parts[i], i);
            b.CreateRet(agg);
            break;
        }
        }
    }

    // a packed LLVM struct with explicit padding, so the layout is exactly
    // the C one (fields are reached by byte offset, see field_addr)
    llvm::Type *ll_struct(StructInfo *st) {
        if (st->ll) return st->ll;
        if (!st->complete) return b.getInt8Ty();  // opaque: only usable behind a pointer
        std::vector<llvm::Type *> parts;
        uint64_t cur = 0;
        std::vector<const Field *> order;
        for (auto &f : st->fields)
            if (!f.bit_width) order.push_back(&f);  // bit fields live in the padding bytes
        std::stable_sort(order.begin(), order.end(), [](const Field *a, const Field *x) { return a->offset < x->offset; });
        for (const Field *f : order) {
            if (f->offset < cur) continue;  // overlaps (union member): reached by offset only
            if (f->offset > cur) parts.push_back(llvm::ArrayType::get(b.getInt8Ty(), f->offset - cur));
            parts.push_back(ll(f->type));
            cur = f->offset + size_of(f->type);
        }
        if (st->size > cur) parts.push_back(llvm::ArrayType::get(b.getInt8Ty(), st->size - cur));
        st->ll = llvm::StructType::get(ctx, parts, true);
        return st->ll;
    }

    void ensure_layout(StructInfo *st, Loc at) {
        if (st->complete) return;
        fatal(at, "struct %s is used before its fields are known", st->name.c_str());
    }

    Type *parse_type(Form *f) override {
        f = expand(f);
        if (f->kind == Form::Sym) {
            static const std::map<std::string, Type *> builtin = {
                {"void", t_void()}, {"bool", t_bool()},
                {"i8", t_int(8, true)}, {"i16", t_int(16, true)}, {"i32", t_int(32, true)}, {"i64", t_int(64, true)},
                {"u8", t_int(8, false)}, {"u16", t_int(16, false)}, {"u32", t_int(32, false)}, {"u64", t_int(64, false)},
                {"isize", t_int(64, true)}, {"usize", t_int(64, false)},
                {"f32", t_float(32)}, {"f64", t_float(64)},
            };
            auto it = builtin.find(f->text);
            if (it != builtin.end()) return it->second;
            auto ut = c.types_.find(f->text);
            if (ut != c.types_.end()) return ut->second;
            fatal(f->loc, "unknown type '%s'", f->text.c_str());
        }
        if (f->is_call("ptr") && f->size() == 2) return t_ptr(parse_type(f->at(1)));
        if (f->is_call("array") && f->size() == 3) {
            Val n = expr(f->at(2), t_int(64, false));
            auto *k = llvm::dyn_cast<llvm::ConstantInt>(n.v);
            if (!k || !n.t->is_int()) fatal(f->at(2)->loc, "array length must be an integer constant");
            return t_array(parse_type(f->at(1)), k->getZExtValue());
        }
        if (f->is_call("fn") && f->size() == 3 && f->at(1)->is_list()) {
            std::vector<Type *> ps;
            bool variadic = false;
            for (Form *p : f->at(1)->items) {
                if (p->is_sym("...")) variadic = true;
                else ps.push_back(parse_type(p));
            }
            return t_fn(parse_type(f->at(2)), ps, variadic);
        }
        fatal(f->loc, "bad type %s", show(f).c_str());
    }

    /* ------------------------------------------------------- helpers --- */

    [[noreturn]] void type_error(Loc at, const char *what, Type *want, Type *got) {
        fatal(at, "%s: expected %s, got %s", what, type_name(want).c_str(), type_name(got).c_str());
    }

    llvm::AllocaInst *alloca_(Type *t, const std::string &name) {
        llvm::IRBuilder<> eb(&fn->getEntryBlock(), fn->getEntryBlock().begin());
        llvm::AllocaInst *a = eb.CreateAlloca(ll(t), nullptr, name);
        a->setAlignment(llvm::Align(align_of(t)));
        return a;
    }

    llvm::Value *load(Val addr) {
        if (addr.bwidth) return load_bits(addr);
        return b.CreateAlignedLoad(ll(addr.t), addr.v, llvm::Align(addr.align ? addr.align : align_of(addr.t)));
    }
    void store(Val addr, llvm::Value *v) {
        if (addr.bwidth) return store_bits(addr, v);
        b.CreateAlignedStore(v, addr.v, llvm::Align(addr.align ? addr.align : align_of(addr.t)));
    }

    // bit fields: read the bytes holding the field as one integer (little
    // endian: the field's bits are bshift..bshift+bwidth-1), then shift/mask
    llvm::Value *load_bits(Val a) {
        unsigned n = a.bwin->getBitWidth();
        llvm::Value *w = b.CreateAlignedLoad(a.bwin, a.v, llvm::Align(1));
        if (a.t->is_int() && a.t->sign) {  // sign-extend from the field's top bit
            w = b.CreateShl(w, n - a.bshift - a.bwidth);
            w = b.CreateAShr(w, n - a.bwidth);
        } else {
            w = b.CreateLShr(w, a.bshift);
            w = b.CreateAnd(w, llvm::APInt::getLowBitsSet(n, a.bwidth));
        }
        if (a.t->kind == Type::Bool) return b.CreateICmpNE(w, llvm::ConstantInt::get(a.bwin, 0));
        return b.CreateIntCast(w, ll(a.t), a.t->sign);
    }
    void store_bits(Val a, llvm::Value *v) {
        unsigned n = a.bwin->getBitWidth();
        llvm::APInt mask = llvm::APInt::getBitsSet(n, a.bshift, a.bshift + a.bwidth);
        llvm::Value *nv = a.t->kind == Type::Bool ? b.CreateZExt(v, a.bwin) : b.CreateZExtOrTrunc(v, a.bwin);
        nv = b.CreateAnd(b.CreateShl(nv, a.bshift), mask);
        llvm::Value *w = b.CreateAlignedLoad(a.bwin, a.v, llvm::Align(1));
        w = b.CreateOr(b.CreateAnd(w, ~mask), nv);
        b.CreateAlignedStore(w, a.v, llvm::Align(1));
    }

    // after a return/break the current block is closed; later code goes to an unreachable block
    void ensure_open() {
        if (b.GetInsertBlock()->getTerminator()) b.SetInsertPoint(BasicBlock::Create(ctx, "dead", fn));
    }
    bool open() const { return !b.GetInsertBlock()->getTerminator(); }

    Val lookup_local(const std::string &n) {
        for (auto s = scopes.rbegin(); s != scopes.rend(); ++s) {
            auto it = s->find(n);
            if (it != s->end()) return it->second;
        }
        return {};
    }

    Global *lookup_global(const std::string &n) {
        auto it = c.globals_.find(n);
        return it == c.globals_.end() ? nullptr : &it->second;
    }

    // does f's value take its type from the context? (literals and arithmetic on them)
    bool untyped(Form *f) {
        f = expand(f);
        if (f->kind == Form::Int || f->kind == Form::Float) return true;
        if (f->kind == Form::Sym) {
            if (lookup_local(key(f)).v) return false;
            Global *g = lookup_global(f->text);
            return g && g->kind == Global::Const && g->form && untyped(g->form);
        }
        if (f->is_list() && f->size() >= 2 && is_arith(f->head())) {
            for (size_t i = 1; i < f->size(); i++)
                if (!untyped(f->at(i))) return false;
            return true;
        }
        return false;
    }
    // the type an untyped form gets when nothing is expected
    Type *default_type(Form *f) {
        f = expand(f);
        if (f->kind == Form::Float) return t_float(64);
        if (f->kind == Form::Int) {
            int64_t v = (int64_t)f->ival;
            bool hex = f->text.find("0x") != std::string::npos || f->text.find("0X") != std::string::npos;
            if (v >= INT32_MIN && v <= INT32_MAX) return t_int(32, true);
            if (hex && f->ival <= UINT32_MAX) return t_int(32, false);
            if (hex && (int64_t)f->ival < 0) return t_int(64, false);
            return t_int(64, true);
        }
        if (f->kind == Form::Sym) return default_type(lookup_global(f->text)->form);
        Type *t = t_int(32, true);
        for (size_t i = 1; i < f->size(); i++) {
            Type *u = default_type(f->at(i));
            if (u->is_float() || (u->bits > t->bits)) t = u;
        }
        return t;
    }

    llvm::Value *to_bool(Val v, Loc at) {
        switch (v.t->kind) {
        case Type::Bool: return v.v;
        case Type::Int: return b.CreateICmpNE(v.v, llvm::ConstantInt::get(ll(v.t), 0));
        case Type::Ptr: return b.CreateIsNotNull(v.v);
        default: fatal(at, "%s is not a condition", type_name(v.t).c_str());
        }
    }

    // implicit conversions on assignment and argument passing: same type, or
    // between (ptr void) and any pointer
    Val coerce(Val v, Type *to, Loc at, const char *what) {
        if (v.t == to) return v;
        if (v.t->is_ptr() && to->is_ptr() && (v.t->elem == t_void() || to->elem == t_void())) return {v.v, to};
        type_error(at, what, to, v.t);
    }

    // an expression where a value of type `want` goes (assignment, argument,
    // return): arrays used as pointers decay to their first element
    Val value_for(Form *f, Type *want, const char *what) {
        if (want && want->is_ptr() && is_place(f)) {
            Val a = place(f);
            if (a.t->kind == Type::Array && (a.t->elem == want->elem || want->elem == t_void())) return {a.v, want};
        }
        Val v = expr(f, want);
        return want ? coerce(v, want, f->loc, what) : v;
    }

    /* ------------------------------------------------------- places ---- */

    bool is_place(Form *f) {
        f = expand(f);
        if (f->kind == Form::Sym) {
            if (lookup_local(key(f)).v) return true;
            Global *g = lookup_global(f->text);
            return g && g->kind == Global::Var;
        }
        return f->is_call(".") || f->is_call("aref") || f->is_call("deref");
    }

    // the address of a place, with the place's type
    Val place(Form *f) {
        f = expand(f);
        if (f->kind == Form::Sym) {
            Val l = lookup_local(key(f));
            if (l.v) return l;
            Global *g = lookup_global(f->text);
            if (g && g->kind == Global::Var) return {g->value, g->type};
            fatal(f->loc, "'%s' is not a variable", f->text.c_str());
        }
        if (f->is_call(".")) {
            if (f->size() < 3) fatal(f->loc, "(. x field ...) needs a field");
            Val a = struct_base(f->at(1));
            for (size_t i = 2; i < f->size(); i++) a = field_addr(a, f->at(i));
            return a;
        }
        if (f->is_call("aref")) {
            if (f->size() != 3) fatal(f->loc, "(aref a i)");
            Val base;
            if (is_place(f->at(1))) {
                base = place(f->at(1));
                if (base.t->kind == Type::Ptr) base = {load(base), base.t};
            } else {
                base = expr(f->at(1), nullptr);
                if (base.t->kind == Type::Array) base = spill(base);
            }
            Val i = expr(f->at(2), t_int(64, true));
            if (!i.t->is_int()) fatal(f->at(2)->loc, "index must be an integer, got %s", type_name(i.t).c_str());
            llvm::Value *idx = b.CreateIntCast(i.v, b.getInt64Ty(), i.t->sign);
            if (base.t->kind == Type::Array) {
                llvm::Value *p = b.CreateInBoundsGEP(ll(base.t), base.v, {b.getInt64(0), idx});
                return {p, base.t->elem};
            }
            if (base.t->kind == Type::Ptr) {
                if (base.t->elem == t_void()) fatal(f->loc, "cannot index a (ptr void)");
                return {b.CreateInBoundsGEP(ll(base.t->elem), base.v, idx), base.t->elem};
            }
            fatal(f->at(1)->loc, "cannot index %s", type_name(base.t).c_str());
        }
        if (f->is_call("deref")) {
            if (f->size() != 2) fatal(f->loc, "(deref p)");
            Val p = expr(f->at(1), nullptr);
            if (!p.t->is_ptr() || p.t->elem == t_void()) fatal(f->loc, "cannot dereference %s", type_name(p.t).c_str());
            return {p.v, p.t->elem};
        }
        fatal(f->loc, "%s is not a place", show(f).c_str());
    }

    // a value in memory, for taking addresses of temporaries
    Val spill(Val v) {
        llvm::AllocaInst *a = alloca_(v.t, "tmp");
        store({a, v.t}, v.v);
        return {a, v.t};
    }

    // the address of x for field access: x itself if it is a place, what it
    // points to if it is a pointer value, or a temporary copy
    Val struct_base(Form *x) {
        if (is_place(x)) return place(x);
        Val v = expr(x, nullptr);
        if (v.t->is_ptr()) return {v.v, v.t->elem};
        return spill(v);
    }

    Val field_addr(Val a, Form *name) {
        while (a.t->is_ptr()) a = {load(a), a.t->elem};  // auto-deref
        if (a.t->kind != Type::Struct) fatal(name->loc, "%s has no field '%s'", type_name(a.t).c_str(), name->text.c_str());
        ensure_layout(a.t->st, name->loc);
        const Field *fd = a.t->st->find(name->text);
        if (!fd) fatal(name->loc, "%s has no field '%s'", type_name(a.t).c_str(), name->text.c_str());
        if (fd->bit_width) {
            uint64_t first = fd->bit_off / 8, last = (fd->bit_off + fd->bit_width - 1) / 8;
            Val v{at_offset(a.v, first), fd->type, 1};
            v.bwin = b.getIntNTy(8 * (last - first + 1));
            v.bshift = fd->bit_off % 8;
            v.bwidth = fd->bit_width;
            return v;
        }
        llvm::Value *p = fd->offset ? b.CreateInBoundsGEP(b.getInt8Ty(), a.v, b.getInt64(fd->offset)) : a.v;
        // alignment: what the struct's alignment and the offset guarantee
        uint64_t base = a.align ? a.align : align_of(a.t);
        uint64_t al = fd->offset ? (fd->offset & -fd->offset) : base;
        if (al > base) al = base;
        if (al > align_of(fd->type)) al = align_of(fd->type);
        return {p, fd->type, al};
    }

    /* --------------------------------------------------- expressions --- */

    Val literal(Form *f, Type *want) {
        if (f->kind == Form::Int) {
            if (!want || (!want->is_num() && !want->is_ptr())) want = default_type(f);
            if (want->is_float()) return float_const(f->text, want);
            if (want->is_ptr()) {
                if (f->ival != 0) fatal(f->loc, "only 0 converts to a pointer");
                return {llvm::ConstantPointerNull::get(b.getPtrTy()), want};
            }
            if (want->bits < 64) {
                int64_t v = (int64_t)f->ival;
                bool fits = want->sign ? (v >= -(int64_t(1) << (want->bits - 1)) && v < (int64_t(1) << (want->bits - 1)))
                                       : (f->ival < (uint64_t(1) << want->bits)) || (v < 0 && v >= -(int64_t(1) << (want->bits - 1)));
                if (!fits) fatal(f->loc, "%s does not fit in %s", f->text.c_str(), type_name(want).c_str());
            }
            return {llvm::ConstantInt::get(ll(want), f->ival), want};
        }
        // Float
        if (want && want->is_int()) fatal(f->loc, "%s is not an integer", f->text.c_str());
        return float_const(f->text, want && want->is_float() ? want : t_float(64));
    }

    // rounded once, from the text: like C's 1.5f / 1.5
    Val float_const(const std::string &text, Type *t) {
        if (t->bits == 32) return {llvm::ConstantFP::get(b.getFloatTy(), strtof(text.c_str(), nullptr)), t};
        return {llvm::ConstantFP::get(b.getDoubleTy(), strtod(text.c_str(), nullptr)), t};
    }

    Val symbol(Form *f, Type *want) {
        const std::string &n = f->text;
        if (n == "true" || n == "false") return {b.getInt1(n == "true"), t_bool()};
        if (n == "null") {
            Type *t = want && want->is_ptr() ? want : t_ptr(t_void());
            return {llvm::ConstantPointerNull::get(b.getPtrTy()), t};
        }
        Val l = lookup_local(key(f));
        if (l.v) return {load(l), l.t};
        Global *g = lookup_global(n);
        if (!g) fatal(f->loc, "unknown name '%s'", n.c_str());
        switch (g->kind) {
        case Global::Var: return {load({g->value, g->type}), g->type};
        case Global::Func: return {g->value, t_ptr(g->type)};
        case Global::Const:
            if (g->form) return expr(g->form, want);
            return {g->value, g->type};
        }
        return {};
    }

    Val expr(Form *f, Type *want) {
        f = expand(f);
        switch (f->kind) {
        case Form::Int: case Form::Float: return literal(f, want);
        case Form::Str: {
            llvm::Constant *s = b.CreateGlobalStringPtr(f->text, "str", 0, &mod);
            return {s, t_ptr(t_int(8, true))};
        }
        case Form::Sym: return symbol(f, want);
        case Form::List: break;
        }
        if (f->items.empty()) fatal(f->loc, "empty form ()");
        const char *h = f->head();
        if (!*h) return call(f);
        std::string hs = h;
        if (is_arith(h)) return arith(f, want);
        if (is_cmp(h)) return compare(f);
        if (hs == "do") return body(f, 1, want);
        if (hs == "let") return let(f, want);
        if (hs == "set") return set(f);
        if (hs == "if") return if_(f, want);
        if (hs == "when" || hs == "unless") return when(f, hs == "unless");
        if (hs == "while") return while_(f);
        if (hs == "for") return for_(f);
        if (hs == "break" || hs == "continue") return jump(f, hs == "break");
        if (hs == "return") return return_(f);
        if (hs == "and" || hs == "or") return logic(f, hs == "and");
        if (hs == "not") {
            if (f->size() != 2) fatal(f->loc, "(not x)");
            return {b.CreateNot(to_bool(expr(f->at(1), t_bool()), f->loc)), t_bool()};
        }
        if (hs == "~") {
            if (f->size() != 2) fatal(f->loc, "(~ x)");
            Val v = expr(f->at(1), want);
            if (!v.t->is_int()) fatal(f->loc, "~ needs an integer");
            return {b.CreateNot(v.v), v.t};
        }
        if (hs == "." || hs == "aref" || hs == "deref") {
            Val a = place(f);
            if (a.t->kind == Type::Array && want && want->is_ptr()) return {a.v, want};
            return {load(a), a.t};
        }
        if (hs == "addr") {
            if (f->size() != 2) fatal(f->loc, "(addr place)");
            if (f->at(1)->kind == Form::Sym) {
                Global *g = lookup_global(f->at(1)->text);
                if (g && g->kind == Global::Func && !lookup_local(key(f->at(1))).v) return {g->value, t_ptr(g->type)};
            }
            Val a = place(f->at(1));
            if (a.bwidth) fatal(f->at(1)->loc, "cannot take the address of a bit field");
            return {a.v, t_ptr(a.t)};
        }
        if (hs == "cast" || hs == "bitcast") {
            if (f->size() != 3) fatal(f->loc, "(%s T e)", h);
            Type *to = parse_type(f->at(1));
            // the operand keeps its own type (3.99 is an f64 here), then converts
            Val v = expr(f->at(2), nullptr);
            return hs == "cast" ? cast(v, to, f->loc) : bitcast(v, to, f->loc);
        }
        if (hs == "sizeof" || hs == "alignof") {
            if (f->size() != 2) fatal(f->loc, "(%s T)", h);
            Type *t = parse_type(f->at(1));
            if (t->kind == Type::Struct) ensure_layout(t->st, f->loc);
            Type *rt = want && want->is_int() ? want : t_int(64, false);
            return {llvm::ConstantInt::get(ll(rt), hs == "sizeof" ? size_of(t) : align_of(t)), rt};
        }
        if (hs == "make") return make(f);
        if (hs == "quote" || hs == "quasiquote") fatal(f->loc, "quoted forms exist only inside macros");
        return call(f);
    }

    Val body(Form *f, size_t from, Type *want) {
        Val last{nullptr, t_void()};
        for (size_t i = from; i < f->size(); i++) {
            ensure_open();
            last = expr(f->at(i), i + 1 == f->size() ? want : nullptr);
        }
        return last;
    }

    // the operand type of an arithmetic or compare form: the first typed operand's
    Type *operand_type(Form *f, size_t from, Type *want, size_t *typed_at) {
        for (size_t i = from; i < f->size(); i++)
            if (!untyped(f->at(i))) { *typed_at = i; return nullptr; }
        *typed_at = 0;
        if (want && want->is_num()) return want;
        Type *t = t_int(32, true);
        for (size_t i = from; i < f->size(); i++) {
            Type *u = default_type(f->at(i));
            if (u->is_float() && !t->is_float()) t = u;
            else if (u->kind == t->kind && u->bits > t->bits) t = u;
        }
        return t;
    }

    // operands of f from `from` on, all of one type
    std::vector<Val> operands(Form *f, size_t from, Type *want) {
        size_t k;
        Type *t = operand_type(f, from, want, &k);
        std::vector<Val> vs(f->size());
        if (!t) {
            vs[k] = expr(f->at(k), nullptr);
            t = vs[k].t;
        }
        for (size_t i = from; i < f->size(); i++) {
            if (vs[i].v) continue;
            Type *ti = t->is_ptr() ? t_int(64, true) : t;  // pointer arithmetic: integer offsets
            vs[i] = expr(f->at(i), ti);
            if (!t->is_ptr() && vs[i].t != t) type_error(f->at(i)->loc, f->head(), t, vs[i].t);
        }
        return {vs.begin() + from, vs.end()};
    }

    /* ------------------------------------------- operator overloading --- */

    // an operand: compiled (v.v set) or still untyped (only the form)
    struct Opnd {
        Val v;
        Form *f;
    };

    // typed operands compiled in order; untyped ones wait for a type
    std::vector<Opnd> opnds(Form *f) {
        std::vector<Opnd> xs;
        for (size_t i = 1; i < f->size(); i++) xs.push_back({untyped(f->at(i)) ? Val{} : expr(f->at(i), nullptr), f->at(i)});
        return xs;
    }

    // the defop matching these operands, called; found = false if none matches
    Val overload(const std::string &op, std::vector<Opnd> xs, Loc at, bool *found) {
        std::vector<Global *> match;
        for (Global *g : overloads[op]) {
            if (g->type->params.size() != xs.size()) continue;
            bool ok = true;
            for (size_t i = 0; i < xs.size() && ok; i++)
                ok = xs[i].v.v ? g->type->params[i] == xs[i].v.t : g->type->params[i]->is_num();
            if (ok) match.push_back(g);
        }
        *found = !match.empty();
        if (match.empty()) return {};
        if (match.size() > 1) fatal(at, "(%s ...) matches %zu definitions; give the literals a type", op.c_str(), match.size());
        Global *g = match[0];
        std::vector<Val> args;
        for (size_t i = 0; i < xs.size(); i++) args.push_back(xs[i].v.v ? xs[i].v : expr(xs[i].f, g->type->params[i]));
        return emit_call(g->value, g->type, args, {});
    }

    // (op a b) with built-in semantics, for operands already sorted out
    Opnd builtin_pair(const std::string &op, Opnd a, Opnd x, Loc at, bool is_compare) {
        if (!a.v.v && !x.v.v && !is_compare)  // both untyped: stays untyped
            return {Val{}, mk_list(at, {mk_sym(at, op), a.f, x.f})};
        Type *t = a.v.v ? a.v.t : x.v.t;
        if (!a.v.v) a.v = expr(a.f, t);
        if (!x.v.v) x.v = expr(x.f, t->is_ptr() && !is_compare ? t_int(64, true) : t);
        if (is_compare) return {compare_vals(op, a.v, x.v, at), nullptr};
        if (!t->is_ptr() && a.v.t != x.v.t) type_error(at, op.c_str(), a.v.t, x.v.t);
        if (!t->is_num() && !t->is_ptr()) fatal(at, "no (%s %s %s): define it with defop", op.c_str(), type_name(a.v.t).c_str(), type_name(x.v.t).c_str());
        return {binop(op, a.v, x.v, at), nullptr};
    }

    Val arith(Form *f, Type *want) {
        std::string op = f->head();
        if (f->size() < 2) fatal(f->loc, "(%s ...) needs operands", op.c_str());
        if (overloads.count(op) && !untyped(f)) {  // all-literal arithmetic is never overloaded
            std::vector<Opnd> xs = opnds(f);
            bool found;
            if (xs.size() == 1) {
                Val r = overload(op, xs, f->loc, &found);
                if (found) return r;
            } else {
                Opnd acc = xs[0];
                for (size_t i = 1; i < xs.size(); i++) {
                    Val r = overload(op, {acc, xs[i]}, f->at(i + 1)->loc, &found);
                    acc = found ? Opnd{r, nullptr} : builtin_pair(op, acc, xs[i], f->at(i + 1)->loc, false);
                }
                return acc.v.v ? acc.v : expr(acc.f, want);
            }
            // a unary operator with no matching definition: built in
            Val v = xs[0].v.v ? xs[0].v : expr(xs[0].f, want);
            if (op != "-") fatal(f->loc, "(%s x) needs two operands", op.c_str());
            if (v.t->is_float()) return {b.CreateFNeg(v.v), v.t};
            if (v.t->is_int()) return {b.CreateNeg(v.v), v.t};
            fatal(f->loc, "no (- %s): define it with defop", type_name(v.t).c_str());
        }
        std::vector<Val> vs = operands(f, 1, want);
        if (vs.size() == 1) {
            if (op != "-") fatal(f->loc, "(%s x) needs two operands", op.c_str());
            Val v = vs[0];
            if (v.t->is_float()) return {b.CreateFNeg(v.v), v.t};
            if (v.t->is_int()) return {b.CreateNeg(v.v), v.t};
            fatal(f->loc, "cannot negate %s", type_name(v.t).c_str());
        }
        Val acc = vs[0];
        for (size_t i = 1; i < vs.size(); i++) acc = binop(op, acc, vs[i], f->at(i + 1)->loc);
        return acc;
    }

    Val binop(const std::string &op, Val a, Val x, Loc at) {
        Type *t = a.t;
        if (t->is_ptr()) {
            if (op == "+" || op == "-") {
                if (x.t->is_int()) {
                    if (t->elem == t_void()) fatal(at, "arithmetic on (ptr void)");
                    llvm::Value *i = b.CreateIntCast(x.v, b.getInt64Ty(), x.t->sign);
                    if (op == "-") i = b.CreateNeg(i);
                    return {b.CreateGEP(ll(t->elem), a.v, i), t};
                }
                if (op == "-" && x.t == t) {
                    llvm::Value *d = b.CreateSub(b.CreatePtrToInt(a.v, b.getInt64Ty()), b.CreatePtrToInt(x.v, b.getInt64Ty()));
                    return {b.CreateExactSDiv(d, b.getInt64(size_of(t->elem))), t_int(64, true)};
                }
            }
            fatal(at, "(%s %s %s) is not defined", op.c_str(), type_name(t).c_str(), type_name(x.t).c_str());
        }
        if (op == "<<" || op == ">>") {
            if (!t->is_int() || !x.t->is_int()) fatal(at, "shifts need integers");
            llvm::Value *n = b.CreateIntCast(x.v, ll(t), false);
            if (op == "<<") return {b.CreateShl(a.v, n), t};
            return {t->sign ? b.CreateAShr(a.v, n) : b.CreateLShr(a.v, n), t};
        }
        if (t->is_float()) {
            if (op == "+") return {b.CreateFAdd(a.v, x.v), t};
            if (op == "-") return {b.CreateFSub(a.v, x.v), t};
            if (op == "*") return {b.CreateFMul(a.v, x.v), t};
            if (op == "/") return {b.CreateFDiv(a.v, x.v), t};
            if (op == "%") return {b.CreateFRem(a.v, x.v), t};
            fatal(at, "'%s' needs integers", op.c_str());
        }
        if (!t->is_int()) fatal(at, "no (%s %s %s): define it with defop", op.c_str(), type_name(t).c_str(), type_name(t).c_str());
        if (op == "+") return {b.CreateAdd(a.v, x.v), t};
        if (op == "-") return {b.CreateSub(a.v, x.v), t};
        if (op == "*") return {b.CreateMul(a.v, x.v), t};
        if (op == "/") return {t->sign ? b.CreateSDiv(a.v, x.v) : b.CreateUDiv(a.v, x.v), t};
        if (op == "%") return {t->sign ? b.CreateSRem(a.v, x.v) : b.CreateURem(a.v, x.v), t};
        if (op == "&") return {b.CreateAnd(a.v, x.v), t};
        if (op == "|") return {b.CreateOr(a.v, x.v), t};
        return {b.CreateXor(a.v, x.v), t};
    }

    Val compare(Form *f) {
        std::string op = f->head();
        if (f->size() != 3) fatal(f->loc, "(%s a b)", op.c_str());
        if (overloads.count(op)) {
            std::vector<Opnd> xs = opnds(f);
            bool found;
            Val r = overload(op, xs, f->loc, &found);
            if (found) return r;
            if (!xs[0].v.v && !xs[1].v.v) xs[0].v = expr(xs[0].f, default_type(mk_list(f->loc, {mk_sym(f->loc, "+"), xs[0].f, xs[1].f})));
            return builtin_pair(op, xs[0], xs[1], f->loc, true).v;
        }
        std::vector<Val> vs = operands(f, 1, nullptr);
        return compare_vals(op, vs[0], vs[1], f->at(2)->loc);
    }

    Val compare_vals(const std::string &op, Val a, Val x, Loc at) {
        if (x.t != a.t) {
            if (a.t->is_ptr() && x.t->is_ptr()) x.t = a.t;
            else type_error(at, op.c_str(), a.t, x.t);
        }
        Type *t = a.t;
        using P = llvm::CmpInst::Predicate;
        int k = op == "=" ? 0 : op == "!=" ? 1 : op == "<" ? 2 : op == "<=" ? 3 : op == ">" ? 4 : 5;
        if (t->is_float()) {
            const P fp[] = {P::FCMP_OEQ, P::FCMP_UNE, P::FCMP_OLT, P::FCMP_OLE, P::FCMP_OGT, P::FCMP_OGE};
            return {b.CreateFCmp(fp[k], a.v, x.v), t_bool()};
        }
        if (t->kind == Type::Struct || t->kind == Type::Array) fatal(at, "no (%s %s %s): define it with defop", op.c_str(), type_name(t).c_str(), type_name(t).c_str());
        const P si[] = {P::ICMP_EQ, P::ICMP_NE, P::ICMP_SLT, P::ICMP_SLE, P::ICMP_SGT, P::ICMP_SGE};
        const P ui[] = {P::ICMP_EQ, P::ICMP_NE, P::ICMP_ULT, P::ICMP_ULE, P::ICMP_UGT, P::ICMP_UGE};
        return {b.CreateICmp(t->is_int() && t->sign ? si[k] : ui[k], a.v, x.v), t_bool()};
    }

    Val logic(Form *f, bool is_and) {
        if (f->size() < 2) return {b.getInt1(is_and), t_bool()};
        BasicBlock *done = BasicBlock::Create(ctx, is_and ? "and" : "or", fn);
        std::vector<std::pair<llvm::Value *, BasicBlock *>> in;
        for (size_t i = 1; i < f->size(); i++) {
            llvm::Value *v = to_bool(expr(f->at(i), t_bool()), f->at(i)->loc);
            if (i + 1 == f->size()) {
                in.push_back({v, b.GetInsertBlock()});
                b.CreateBr(done);
                break;
            }
            BasicBlock *next = BasicBlock::Create(ctx, "next", fn);
            in.push_back({b.getInt1(!is_and), b.GetInsertBlock()});
            if (is_and) b.CreateCondBr(v, next, done);
            else b.CreateCondBr(v, done, next);
            b.SetInsertPoint(next);
        }
        b.SetInsertPoint(done);
        llvm::PHINode *phi = b.CreatePHI(b.getInt1Ty(), in.size());
        for (auto &p : in) phi->addIncoming(p.first, p.second);
        return {phi, t_bool()};
    }

    Val let(Form *f, Type *want) {
        if (f->size() < 2 || !f->at(1)->is_list()) fatal(f->loc, "(let ((name [T] [init]) ...) body...)");
        scopes.emplace_back();
        for (Form *bd : f->at(1)->items) {
            if (!bd->is_list() || bd->size() < 2 || bd->size() > 3 || bd->at(0)->kind != Form::Sym)
                fatal(bd->loc, "binding must be (name init), (name T init) or (name T)");
            const std::string &name = bd->at(0)->text;
            Type *t = nullptr;
            Form *init = nullptr;
            if (bd->size() == 3) { t = parse_type(bd->at(1)); init = bd->at(2); }
            else if (is_type_form(bd->at(1))) t = parse_type(bd->at(1));
            else init = bd->at(1);
            Val v;
            if (init) {
                v = value_for(init, t, "initializer");
                t = v.t;
                if (t == t_void()) fatal(init->loc, "'%s' would have type void", name.c_str());
            }
            llvm::AllocaInst *a = alloca_(t, name);
            store({a, t}, init ? v.v : llvm::Constant::getNullValue(ll(t)));
            scopes.back()[key(bd->at(0))] = {a, t};
        }
        Val r = body(f, 2, want);
        scopes.pop_back();
        return r;
    }

    // (x T) in a let: is the second item a type rather than an initializer?
    bool is_type_form(Form *f) {
        f = expand(f);
        if (f->kind == Form::Sym) {
            static const char *const names[] = {"void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "isize", "usize", "f32", "f64"};
            for (const char *n : names)
                if (f->text == n) return true;
            return c.types_.count(f->text) && !lookup_local(key(f)).v && !lookup_global(f->text);
        }
        return f->is_call("ptr") || f->is_call("array") || f->is_call("fn");
    }

    Val set(Form *f) {
        if (f->size() != 3) fatal(f->loc, "(set place value)");
        Val a = place(f->at(1));
        Val v = value_for(f->at(2), a.t, "assignment");
        store(a, v.v);
        return {nullptr, t_void()};
    }

    Val if_(Form *f, Type *want) {
        if (f->size() != 3 && f->size() != 4) fatal(f->loc, "(if cond then [else])");
        llvm::Value *c0 = to_bool(expr(f->at(1), t_bool()), f->at(1)->loc);
        BasicBlock *th = BasicBlock::Create(ctx, "then", fn), *el = BasicBlock::Create(ctx, "else", fn);
        BasicBlock *done = BasicBlock::Create(ctx, "endif", fn);
        b.CreateCondBr(c0, th, el);
        bool two = f->size() == 4;
        // An untyped branch takes the other branch's type, so that one is
        // compiled first (the branches are separate blocks; order is free).
        bool else_first = two && !want && untyped(f->at(2)) && !untyped(f->at(3));
        Type *t = want;
        if (two && !t && untyped(f->at(2)) && untyped(f->at(3))) t = default_type(mk_list(f->loc, {mk_sym(f->loc, "+"), f->at(2), f->at(3)}));
        Val a, x{nullptr, t_void()};
        BasicBlock *th_end, *el_end;
        bool th_open, el_open;
        auto then_branch = [&](Type *tt) {
            b.SetInsertPoint(th);
            a = expr(f->at(2), tt);
            th_end = b.GetInsertBlock();
            th_open = open();
        };
        auto else_branch = [&](Type *tt) {
            b.SetInsertPoint(el);
            if (two) x = expr(f->at(3), tt);
            el_end = b.GetInsertBlock();
            el_open = open();
        };
        if (else_first) {
            else_branch(nullptr);
            then_branch(x.t);
        } else {
            then_branch(t);
            else_branch(t ? t : a.t);
        }
        bool value = two && a.t != t_void() && x.t != t_void();
        if (value && a.t != x.t) type_error(f->at(3)->loc, "if branches", a.t, x.t);
        if (th_open) { b.SetInsertPoint(th_end); b.CreateBr(done); }
        if (el_open) { b.SetInsertPoint(el_end); b.CreateBr(done); }
        b.SetInsertPoint(done);
        if (!th_open && !el_open) { b.CreateUnreachable(); return {nullptr, t_void()}; }
        if (!value) return {nullptr, t_void()};
        llvm::PHINode *phi = b.CreatePHI(ll(a.t), 2);
        if (th_open) phi->addIncoming(a.v, th_end);
        if (el_open) phi->addIncoming(x.v, el_end);
        return {phi, a.t};
    }

    Val when(Form *f, bool unless) {
        if (f->size() < 2) fatal(f->loc, "(%s cond body...)", unless ? "unless" : "when");
        llvm::Value *c0 = to_bool(expr(f->at(1), t_bool()), f->at(1)->loc);
        BasicBlock *th = BasicBlock::Create(ctx, "when", fn), *done = BasicBlock::Create(ctx, "endwhen", fn);
        if (unless) b.CreateCondBr(c0, done, th);
        else b.CreateCondBr(c0, th, done);
        b.SetInsertPoint(th);
        body(f, 2, nullptr);
        if (open()) b.CreateBr(done);
        b.SetInsertPoint(done);
        return {nullptr, t_void()};
    }

    Val while_(Form *f) {
        if (f->size() < 2) fatal(f->loc, "(while cond body...)");
        BasicBlock *cond = BasicBlock::Create(ctx, "while", fn), *bodyb = BasicBlock::Create(ctx, "do", fn);
        BasicBlock *done = BasicBlock::Create(ctx, "endwhile", fn);
        b.CreateBr(cond);
        b.SetInsertPoint(cond);
        b.CreateCondBr(to_bool(expr(f->at(1), t_bool()), f->at(1)->loc), bodyb, done);
        b.SetInsertPoint(bodyb);
        loops.push_back({cond, done});
        body(f, 2, nullptr);
        loops.pop_back();
        if (open()) b.CreateBr(cond);
        b.SetInsertPoint(done);
        return {nullptr, t_void()};
    }

    // (for (i start end [step]) body...): i from start while i < end
    Val for_(Form *f) {
        if (f->size() < 2 || !f->at(1)->is_list() || f->at(1)->size() < 3 || f->at(1)->size() > 4 || f->at(1)->at(0)->kind != Form::Sym)
            fatal(f->loc, "(for (i start end [step]) body...)");
        Form *spec = f->at(1);
        // the counter's type: the first typed bound's (untyped bounds have no side effects)
        Val start, end;
        Type *t;
        if (!untyped(spec->at(1))) t = (start = expr(spec->at(1), nullptr)).t;
        else if (!untyped(spec->at(2))) t = (end = expr(spec->at(2), nullptr)).t;
        else t = default_type(mk_list(spec->loc, {mk_sym(spec->loc, "+"), spec->at(1), spec->at(2)}));
        if (!t->is_int()) fatal(spec->loc, "for needs an integer counter, got %s", type_name(t).c_str());
        if (!start.v) start = expr(spec->at(1), t);
        if (!end.v) end = expr(spec->at(2), t);
        if (start.t != t || end.t != t) type_error(spec->loc, "for bounds", t, start.t != t ? start.t : end.t);
        scopes.emplace_back();
        llvm::AllocaInst *iv = alloca_(t, spec->at(0)->text);
        store({iv, t}, start.v);
        scopes.back()[key(spec->at(0))] = {iv, t};
        BasicBlock *cond = BasicBlock::Create(ctx, "for", fn), *bodyb = BasicBlock::Create(ctx, "body", fn);
        BasicBlock *step = BasicBlock::Create(ctx, "step", fn), *done = BasicBlock::Create(ctx, "endfor", fn);
        b.CreateBr(cond);
        b.SetInsertPoint(cond);
        llvm::Value *i = load({iv, t});
        b.CreateCondBr(t->sign ? b.CreateICmpSLT(i, end.v) : b.CreateICmpULT(i, end.v), bodyb, done);
        b.SetInsertPoint(bodyb);
        loops.push_back({step, done});
        body(f, 2, nullptr);
        loops.pop_back();
        if (open()) b.CreateBr(step);
        b.SetInsertPoint(step);
        Val by = spec->size() == 4 ? expr(spec->at(3), t) : Val{llvm::ConstantInt::get(ll(t), 1), t};
        if (by.t != t) type_error(spec->at(3)->loc, "for step", t, by.t);
        store({iv, t}, b.CreateAdd(load({iv, t}), by.v));
        b.CreateBr(cond);
        b.SetInsertPoint(done);
        scopes.pop_back();
        return {nullptr, t_void()};
    }

    Val jump(Form *f, bool is_break) {
        if (loops.empty()) fatal(f->loc, "(%s) outside a loop", is_break ? "break" : "continue");
        b.CreateBr(is_break ? loops.back().brk : loops.back().cont);
        return {nullptr, t_void()};
    }

    Val return_(Form *f) {
        if (f->size() == 1) {
            if (fn_ret != t_void()) fatal(f->loc, "return needs a %s value", type_name(fn_ret).c_str());
            b.CreateRetVoid();
        } else {
            Val v = value_for(f->at(1), fn_ret, "return value");
            emit_ret(v);
        }
        return {nullptr, t_void()};
    }

    Val cast(Val v, Type *to, Loc at) {
        Type *from = v.t;
        if (from == to) return v;
        llvm::Type *lt = ll(to);
        if (from->is_int() && to->is_int()) return {b.CreateIntCast(v.v, lt, from->sign), to};
        if (from->kind == Type::Bool && to->is_int()) return {b.CreateZExt(v.v, lt), to};
        if (from->is_int() && to->kind == Type::Bool) return {b.CreateICmpNE(v.v, llvm::ConstantInt::get(ll(from), 0)), to};
        if (from->is_int() && to->is_float()) return {from->sign ? b.CreateSIToFP(v.v, lt) : b.CreateUIToFP(v.v, lt), to};
        if (from->is_float() && to->is_int()) return {to->sign ? b.CreateFPToSI(v.v, lt) : b.CreateFPToUI(v.v, lt), to};
        if (from->is_float() && to->is_float()) return {b.CreateFPCast(v.v, lt), to};
        if (from->is_ptr() && to->is_ptr()) return {v.v, to};
        if (from->is_ptr() && to->is_int()) return {b.CreatePtrToInt(v.v, lt), to};
        if (from->is_int() && to->is_ptr()) return {b.CreateIntToPtr(v.v, lt), to};
        fatal(at, "cannot cast %s to %s", type_name(from).c_str(), type_name(to).c_str());
    }

    // same bits, other type (f32 <-> u32 ...)
    Val bitcast(Val v, Type *to, Loc at) {
        if (!v.t->is_scalar() || !to->is_scalar() || size_of(v.t) != size_of(to))
            fatal(at, "cannot bitcast %s to %s", type_name(v.t).c_str(), type_name(to).c_str());
        if (v.t->is_ptr() || to->is_ptr()) return cast(v, to, at);
        return {b.CreateBitCast(v.v, ll(to)), to};
    }

    // (make T :field e ...) for structs, (make (array T N) e ...) for arrays; the rest is zero
    Val make(Form *f) {
        if (f->size() < 2) fatal(f->loc, "(make T ...)");
        Type *t = parse_type(f->at(1));
        llvm::AllocaInst *a = alloca_(t, "make");
        store({a, t}, llvm::Constant::getNullValue(ll(t)));
        if (t->kind == Type::Struct) {
            ensure_layout(t->st, f->loc);
            if ((f->size() - 2) % 2) fatal(f->loc, "(make %s :field value ...)", type_name(t).c_str());
            for (size_t i = 2; i < f->size(); i += 2) {
                Form *k = f->at(i);
                if (k->kind != Form::Sym || k->text.size() < 2 || k->text[0] != ':') fatal(k->loc, "expected :field");
                Val fa = field_addr({a, t}, mk_sym(k->loc, k->text.substr(1)));
                store(fa, value_for(f->at(i + 1), fa.t, "field").v);
            }
        } else if (t->kind == Type::Array) {
            if (f->size() - 2 > t->n) fatal(f->loc, "too many elements for %s", type_name(t).c_str());
            for (size_t i = 2; i < f->size(); i++) {
                llvm::Value *p = b.CreateInBoundsGEP(ll(t), a, {b.getInt64(0), b.getInt64(i - 2)});
                store({p, t->elem}, value_for(f->at(i), t->elem, "element").v);
            }
        } else fatal(f->loc, "make needs a struct or array type");
        return {load({a, t}), t};
    }

    Val call(Form *f) {
        Form *head = f->at(0);
        Val callee;
        Type *ft = nullptr;
        if (head->kind == Form::Sym && !lookup_local(key(head)).v) {
            Global *g = lookup_global(head->text);
            if (!g) fatal(head->loc, "unknown function '%s'", head->text.c_str());
            if (g->kind == Global::Func) { callee = {g->value, g->type}; ft = g->type; }
        }
        if (!ft) {
            callee = expr(head, nullptr);
            if (!callee.t->is_ptr() || callee.t->elem->kind != Type::Fn) fatal(head->loc, "%s is not a function", show(head).c_str());
            ft = callee.t->elem;
        }
        size_t n = f->size() - 1;
        if (n < ft->params.size() || (n > ft->params.size() && !ft->variadic))
            fatal(f->loc, "%s takes %zu argument%s, got %zu", show(head).c_str(), ft->params.size(), ft->params.size() == 1 ? "" : "s", n);
        std::vector<Val> fixed, var;
        for (size_t i = 0; i < n; i++) {
            Form *a = f->at(i + 1);
            if (i < ft->params.size()) {
                fixed.push_back(value_for(a, ft->params[i], "argument"));
                continue;
            }
            // variadic: C's default promotions
            Val v = expr(a, nullptr);
            if (v.t->is_float() && v.t->bits < 64) v = cast(v, t_float(64), a->loc);
            else if (v.t->is_int() && v.t->bits < 32) v = cast(v, t_int(32, v.t->sign), a->loc);
            else if (v.t->kind == Type::Bool) v = cast(v, t_int(32, true), a->loc);
            else if (v.t->kind == Type::Struct || v.t->kind == Type::Array) fatal(a->loc, "cannot pass %s to a variadic function", type_name(v.t).c_str());
            var.push_back(v);
        }
        return emit_call(callee.v, ft, fixed, var);
    }

    // a call with its arguments compiled, lowered per the calling convention
    Val emit_call(llvm::Value *callee, Type *ft, const std::vector<Val> &fixed, const std::vector<Val> &var) {
        FnAbi &abi = fn_abi(ft);
        std::vector<llvm::Value *> args;
        llvm::Value *sret = nullptr;
        if (abi.ret.kind == ArgAbi::Memory) args.push_back(sret = alloca_(ft->elem, "sret"));
        for (size_t i = 0; i < fixed.size(); i++) {
            const ArgAbi &pa = abi.params[i];
            if (pa.kind == ArgAbi::Direct) args.push_back(fixed[i].v);
            else if (pa.kind == ArgAbi::Coerce) coerce_out(pa, fixed[i], args);
            else if (pa.kind == ArgAbi::Memory) args.push_back(spill(fixed[i]).v);  // byval: the callee gets its own copy
        }
        for (const Val &v : var) args.push_back(v.v);
        llvm::CallInst *ci = b.CreateCall(abi.ty, callee, args);
        ci->setAttributes(abi.attrs);
        Type *rt = ft->elem;
        switch (abi.ret.kind) {
        case ArgAbi::Direct: return {ci, rt};
        case ArgAbi::Ignore: return {llvm::Constant::getNullValue(ll(rt)), rt};
        case ArgAbi::Memory: return {load({sret, rt}), rt};
        case ArgAbi::Coerce: {
            llvm::AllocaInst *m = alloca_(rt, "ret");
            std::vector<llvm::Value *> parts;
            if (abi.ret.parts.size() == 1) parts.push_back(ci);
            else
                for (unsigned i = 0; i < abi.ret.parts.size(); i++) parts.push_back(b.CreateExtractValue(ci, i));
            coerce_in(abi.ret, parts, m, rt);
            return {load({m, rt}), rt};
        }
        }
        return {};
    }

    /* ------------------------------------------------------ top level --- */

    // top-level macro calls expanded; (do ...) at the top level is spliced
    void expand_top(Form *f, std::vector<Form *> &out) {
        f = expand(f);
        if (f->is_call("do")) {
            for (size_t i = 1; i < f->size(); i++) expand_top(f->at(i), out);
        } else out.push_back(f);
    }

    // (include "file.lsp"): that file's top-level forms in place, recursively;
    // the path is relative to the including file; each file is read once
    std::map<std::string, bool> included;
    void with_includes(const std::vector<Form *> &in, std::vector<Form *> &out) {
        for (Form *f : in) {
            if (!f->is_call("include")) { out.push_back(f); continue; }
            for (size_t i = 1; i < f->size(); i++) {
                Form *p = f->at(i);
                if (p->kind != Form::Str) fatal(p->loc, "(include \"file.lsp\" ...)");
                std::string from = f->loc.file, path = p->text;
                if (path[0] != '/' && from.find('/') != std::string::npos) path = from.substr(0, from.rfind('/') + 1) + path;
                char real[4096];
                if (!realpath(path.c_str(), real)) fatal(p->loc, "cannot include %s: %s", path.c_str(), strerror(errno));
                if (included[real]) continue;
                included[real] = true;
                with_includes(read_file(strdup(path.c_str())), out);
            }
        }
    }

    void top(const std::vector<Form *> &files) {
        std::vector<Form *> input;
        with_includes(files, input);
        // macros and compile-time helpers first, so everything can use them
        std::vector<Form *> forms;
        for (Form *f : input) {
            if (f->is_call("defmacro") || f->is_call("defmeta")) meta.define(f);
            else forms.push_back(f);
        }
        // pass 0: C headers and hand-written externs
        for (Form *f : forms) {
            if (f->is_call("c-include")) {
                std::vector<std::string> hs;
                for (size_t i = 1; i < f->size(); i++) {
                    if (f->at(i)->kind != Form::Str) fatal(f->at(i)->loc, "header names are strings");
                    hs.push_back(f->at(i)->text);
                }
                c_include(c, hs, f->loc);
            }
        }
        // pass 1: struct names, so structs can refer to each other
        std::vector<std::pair<Form *, StructInfo *>> structs;
        for (Form *f : forms) {
            if (!f->is_call("struct") && !f->is_call("union")) continue;
            if (f->size() < 2 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(struct name (field T) ...)");
            auto *st = new StructInfo;
            st->name = f->at(1)->text;
            st->is_union = f->is_call("union");
            if (c.types_.count(st->name)) fatal(f->loc, "type '%s' is already defined", st->name.c_str());
            c.types_[st->name] = t_struct(st);
            structs.push_back({f, st});
        }
        add_fields(structs);
        for (auto &s : structs) lay_out(s.second, s.first->loc);
        // top-level macros run once the structs are known (they may ask about them);
        // structs they produce are added the same way
        std::vector<Form *> expanded;
        for (Form *f : forms) {
            if (f->is_call("struct") || f->is_call("union") || f->is_call("c-include")) expanded.push_back(f);
            else expand_top(f, expanded);
        }
        forms = expanded;
        std::vector<std::pair<Form *, StructInfo *>> more;
        for (Form *f : forms) {
            if ((!f->is_call("struct") && !f->is_call("union")) || f->size() < 2 || f->at(1)->kind != Form::Sym) continue;
            if (c.types_.count(f->at(1)->text) && c.types_[f->at(1)->text]->kind == Type::Struct && c.types_[f->at(1)->text]->st->complete) continue;
            auto *st = new StructInfo;
            st->name = f->at(1)->text;
            st->is_union = f->is_call("union");
            c.types_[st->name] = t_struct(st);
            more.push_back({f, st});
        }
        add_fields(more);
        for (auto &s : more) lay_out(s.second, s.first->loc);
        // defop: a function named after the operator and its types
        std::vector<std::pair<std::string, Form *>> ops;
        for (Form *&f : forms) {
            if (!f->is_call("defop")) continue;
            if (f->size() < 5 || f->at(1)->kind != Form::Sym || !f->at(2)->is_list()) fatal(f->loc, "(defop op ((a T) ...) R body...)");
            std::string name = f->at(1)->text + "(";
            for (size_t i = 0; i < f->at(2)->size(); i++) {
                Form *p = f->at(2)->at(i);
                if (!p->is_list() || p->size() != 2) fatal(p->loc, "parameter must be (name T)");
                name += (i ? "," : "") + type_name(parse_type(p->at(1)));
            }
            name += ")";
            std::vector<Form *> items = f->items;
            items[0] = mk_sym(f->loc, "defn");
            items[1] = mk_sym(f->at(1)->loc, name);
            ops.push_back({f->at(1)->text, f = mk_list(f->loc, items)});
        }
        // pass 2: constants, globals, function signatures
        for (Form *f : forms) {
            const char *h = f->head();
            if (!strcmp(h, "const")) {
                if (f->size() != 3 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(const NAME expr)");
                define(f->at(1), Global{Global::Const, nullptr, nullptr, f->at(2)});
            } else if (!strcmp(h, "def")) {
                def_global(f);
            } else if (!strcmp(h, "defn")) {
                declare_fn(f);
            } else if (!strcmp(h, "extern")) {
                if (f->size() != 4 || f->at(1)->kind != Form::Sym || !f->at(2)->is_list()) fatal(f->loc, "(extern name (T ...) R)");
                Type *ft = parse_type(mk_list(f->loc, {mk_sym(f->loc, "fn"), f->at(2), f->at(3)}));
                c.add_extern_fn(f->at(1)->text, ft);
            } else if (strcmp(h, "c-include") && strcmp(h, "struct") && strcmp(h, "union")) {
                fatal(f->loc, "unknown top-level form %s", show(f).c_str());
            }
        }
        for (auto &o : ops) overloads[o.first].push_back(&c.globals_[o.second->at(1)->text]);
        // pass 3: function bodies
        for (Form *f : forms)
            if (f->is_call("defn")) define_fn(f);
    }

    void add_fields(std::vector<std::pair<Form *, StructInfo *>> &structs) {
        for (auto &s : structs) {
            for (size_t i = 2; i < s.first->size(); i++) {

                Form *fd = s.first->at(i);
                bool bits = fd->is_list() && fd->size() == 4 && fd->at(2)->is_sym(":bits") && fd->at(3)->kind == Form::Int;
                if (!fd->is_list() || (fd->size() != 2 && !bits) || fd->at(0)->kind != Form::Sym)
                    fatal(fd->loc, "field must be (name T) or (name T :bits N)");
                Field field{fd->at(0)->text, parse_type(fd->at(1)), 0};
                if (bits) {
                    field.bit_width = (unsigned)fd->at(3)->ival;
                    if (!field.type->is_int() && field.type->kind != Type::Bool) fatal(fd->loc, "a bit field must be an integer or bool");
                    if (field.bit_width == 0 || field.bit_width > 8 * size_of(field.type)) fatal(fd->at(3)->loc, "bit width must be 1..%lu", 8 * size_of(field.type));
                }
                s.second->fields.push_back(field);
            }
        }
    }

    void lay_out(StructInfo *st, Loc at) {
        if (st->complete) return;
        if (laying_out[st]) fatal(at, "struct %s contains itself", st->name.c_str());
        laying_out[st] = true;
        for (auto &fd : st->fields) {
            Type *t = fd.type;
            while (t->kind == Type::Array) t = t->elem;
            if (t->kind == Type::Struct) lay_out(t->st, at);
        }
        layout_struct(st);
    }

    void define(Form *name, Global g) {
        if (c.globals_.count(name->text)) fatal(name->loc, "'%s' is already defined", name->text.c_str());
        c.globals_[name->text] = g;
    }

    void def_global(Form *f) {
        if (f->size() < 3 || f->size() > 4 || f->at(1)->kind != Form::Sym) fatal(f->loc, "(def name T [init])");
        Type *t = parse_type(f->at(2));
        llvm::Constant *init = llvm::Constant::getNullValue(ll(t));
        if (f->size() == 4) {
            Val v = expr(f->at(3), t);
            init = llvm::dyn_cast<llvm::Constant>(coerce(v, t, f->at(3)->loc, "initializer").v);
            if (!init) fatal(f->at(3)->loc, "a global's initializer must be a constant");
        }
        auto *gv = new llvm::GlobalVariable(mod, ll(t), false, llvm::GlobalValue::InternalLinkage, init, f->at(1)->text);
        gv->setAlignment(llvm::Align(align_of(t)));
        define(f->at(1), Global{Global::Var, t, gv});
    }

    void declare_fn(Form *f) {
        if (f->size() < 4 || f->at(1)->kind != Form::Sym || !f->at(2)->is_list()) fatal(f->loc, "(defn name ((a T) ...) R body...)");
        std::vector<Type *> ps;
        for (Form *p : f->at(2)->items) {
            if (!p->is_list() || p->size() != 2 || p->at(0)->kind != Form::Sym) fatal(p->loc, "parameter must be (name T)");
            ps.push_back(parse_type(p->at(1)));
        }
        Type *ft = t_fn(parse_type(f->at(3)), ps, false);
        const std::string &name = f->at(1)->text;
        auto link = name == "main" ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage;
        llvm::Function *fn0 = llvm::Function::Create(fn_abi(ft).ty, link, name, mod);
        fn0->setAttributes(fn_abi(ft).attrs);
        define(f->at(1), Global{Global::Func, ft, fn0, nullptr, f});
    }

    void define_fn(Form *f) {
        Global &g = c.globals_[f->at(1)->text];
        fn = llvm::cast<llvm::Function>(g.value);
        fn_ret = g.type->elem;
        cur_ft = g.type;
        FnAbi &abi = fn_abi(g.type);
        b.SetInsertPoint(BasicBlock::Create(ctx, "entry", fn));
        scopes.assign(1, {});
        auto arg = fn->arg_begin();
        cur_sret = abi.ret.kind == ArgAbi::Memory ? &*arg++ : nullptr;
        for (size_t i = 0; i < g.type->params.size(); i++) {
            const std::string &name = f->at(2)->at(i)->at(0)->text;
            Type *t = g.type->params[i];
            const ArgAbi &pa = abi.params[i];
            if (pa.kind == ArgAbi::Memory) {  // byval: the copy is ours to use as the variable
                arg->setName(name);
                scopes.back()[key(f->at(2)->at(i)->at(0))] = {&*arg++, t};
                continue;
            }
            llvm::AllocaInst *a = alloca_(t, name);
            if (pa.kind == ArgAbi::Direct) {
                arg->setName(name);
                store({a, t}, &*arg++);
            } else if (pa.kind == ArgAbi::Coerce) {
                std::vector<llvm::Value *> parts;
                for (size_t k = 0; k < pa.parts.size(); k++) parts.push_back(&*arg++);
                coerce_in(pa, parts, a, t);
            } else {
                store({a, t}, llvm::Constant::getNullValue(ll(t)));
            }
            scopes.back()[key(f->at(2)->at(i)->at(0))] = {a, t};
        }
        Val v = body(f, 4, fn_ret == t_void() ? nullptr : fn_ret);
        if (open()) {
            if (fn_ret == t_void()) b.CreateRetVoid();
            else {
                if (v.t == t_void()) fatal(f->loc, "%s must end with a %s value or return", f->at(1)->text.c_str(), type_name(fn_ret).c_str());
                emit_ret(coerce(v, fn_ret, f->at(f->size() - 1)->loc, "result"));
            }
        }
        // blocks left empty by control flow that never falls through
        for (BasicBlock &bb : *fn)
            if (!bb.getTerminator()) {
                b.SetInsertPoint(&bb);
                b.CreateUnreachable();
            }
        scopes.clear();
    }
};

Compiler::Compiler() : ctx_(new llvm::LLVMContext), mod_(new llvm::Module("lisc", *ctx_)), impl_(new Impl(*this)) {}
Compiler::~Compiler() = default;

void Compiler::compile(const std::vector<Form *> &forms) {
    impl_->top(forms);
    std::string err;
    llvm::raw_string_ostream os(err);
    if (llvm::verifyModule(*mod_, &os)) {
        fprintf(stderr, "internal error: invalid LLVM IR:\n%s\n", err.c_str());
        exit(2);
    }
}

void Compiler::add_type(const std::string &name, Type *t) { types_[name] = t; }

void Compiler::add_extern_fn(const std::string &name, Type *fn) {
    if (globals_.count(name)) return;
    llvm::Function *f = mod_->getFunction(name);
    if (!f) {
        f = llvm::Function::Create(llvm::cast<llvm::FunctionType>(impl_->ll(fn)), llvm::GlobalValue::ExternalLinkage, name, *mod_);
        f->setAttributes(impl_->fn_abi(fn).attrs);
    }
    globals_[name] = Global{Global::Func, fn, f};
}

void Compiler::add_extern_var(const std::string &name, Type *t) {
    if (globals_.count(name)) return;
    auto *gv = new llvm::GlobalVariable(*mod_, impl_->ll(t), false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
    globals_[name] = Global{Global::Var, t, gv};
}

void Compiler::add_const_int(const std::string &name, Type *t, uint64_t v) {
    if (globals_.count(name)) return;
    globals_[name] = Global{Global::Const, t, llvm::ConstantInt::get(impl_->ll(t), v)};
}

void Compiler::add_const_float(const std::string &name, Type *t, double v) {
    if (globals_.count(name)) return;
    globals_[name] = Global{Global::Const, t, llvm::ConstantFP::get(impl_->ll(t), v)};
}

void Compiler::add_const_str(const std::string &name, const std::string &s) {
    if (globals_.count(name)) return;
    llvm::Constant *init = llvm::ConstantDataArray::getString(*ctx_, s);
    auto *gv = new llvm::GlobalVariable(*mod_, init->getType(), true, llvm::GlobalValue::PrivateLinkage, init, name);
    globals_[name] = Global{Global::Const, t_ptr(t_int(8, true)), gv};
}
