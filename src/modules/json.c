#include "tinypy/tinypy.h"

#include "tinypy/common/utils.h"
#include "tinypy/common/dmath.h"
#include "tinypy/objects/object.h"
#include "tinypy/common/sstream.h"
#include "tinypy/interpreter/vm.h"
#include "tinypy/common/floatconv.h"
#include "tinypy/common/str.h"

#include <string.h>

static bool json_loads(int argc, py_Ref argv) {
    PY_CHECK_ARGC(1);
    PY_CHECK_ARG_TYPE(0, tp_str);
    const char* source = py_tostr(argv);
    return py_json_loads(source);
}

static bool json_dumps(int argc, py_Ref argv) {
    PY_CHECK_ARGC(2);
    PY_CHECK_ARG_TYPE(1, tp_int);
    int indent = py_toint(&argv[1]);
    return py_json_dumps(argv, indent);
}

void pk__add_module_json() {
    py_Ref mod = py_newmodule("json");

    py_setdict(mod, py_name("null"), py_None());
    py_setdict(mod, py_name("true"), py_True());
    py_setdict(mod, py_name("false"), py_False());
    py_TValue tmp;
    py_newfloat(&tmp, DMATH_NAN);
    py_setdict(mod, py_name("NaN"), &tmp);
    py_newfloat(&tmp, DMATH_INFINITY);
    py_setdict(mod, py_name("Infinity"), &tmp);

    py_bindfunc(mod, "loads", json_loads);
    py_bind(mod, "dumps(obj, indent=0)", json_dumps);
}

typedef struct {
    c11_sbuf* buf;
    bool first;
    int indent;
    int depth;
} json__write_dict_kv_ctx;

static bool json__write_object(c11_sbuf* buf, py_TValue* obj, int indent, int depth);

static void json__write_indent(c11_sbuf* buf, int n_spaces) {
    for(int i = 0; i < n_spaces; i++) {
        c11_sbuf__write_char(buf, ' ');
    }
}

static bool json__write_array(c11_sbuf* buf, py_TValue* arr, int length, int indent, int depth) {
    c11_sbuf__write_char(buf, '[');
    if(length == 0) {
        c11_sbuf__write_char(buf, ']');
        return true;
    }
    if(indent > 0) c11_sbuf__write_char(buf, '\n');
    int n_spaces = indent * depth;
    const char* sep = indent > 0 ? ",\n" : ", ";
    for(int i = 0; i < length; i++) {
        if(i != 0) c11_sbuf__write_cstr(buf, sep);
        json__write_indent(buf, n_spaces);
        bool ok = json__write_object(buf, arr + i, indent, depth);
        if(!ok) return false;
    }
    if(indent > 0) {
        c11_sbuf__write_char(buf, '\n');
        json__write_indent(buf, n_spaces - indent);
    }
    c11_sbuf__write_char(buf, ']');
    return true;
}

static bool json__write_dict_kv(py_Ref k, py_Ref v, void* ctx_) {
    json__write_dict_kv_ctx* ctx = ctx_;
    int n_spaces = ctx->indent * ctx->depth;
    const char* sep = ctx->indent > 0 ? ",\n" : ", ";
    if(!ctx->first) c11_sbuf__write_cstr(ctx->buf, sep);
    ctx->first = false;
    if(!py_isstr(k)) return TypeError("keys must be strings");
    json__write_indent(ctx->buf, n_spaces);
    c11_sbuf__write_quoted(ctx->buf, py_tosv(k), '"');
    c11_sbuf__write_cstr(ctx->buf, ": ");
    return json__write_object(ctx->buf, v, ctx->indent, ctx->depth);
}

static bool json__write_namedict_kv(py_Name k, py_Ref v, void* ctx_) {
    json__write_dict_kv_ctx* ctx = ctx_;
    int n_spaces = ctx->indent * ctx->depth;
    const char* sep = ctx->indent > 0 ? ",\n" : ", ";
    if(!ctx->first) c11_sbuf__write_cstr(ctx->buf, sep);
    ctx->first = false;
    json__write_indent(ctx->buf, n_spaces);
    c11_sbuf__write_quoted(ctx->buf, py_name2sv(k), '"');
    c11_sbuf__write_cstr(ctx->buf, ": ");
    return json__write_object(ctx->buf, v, ctx->indent, ctx->depth);
}

static bool json__write_object(c11_sbuf* buf, py_TValue* obj, int indent, int depth) {
    switch(obj->type) {
        case tp_NoneType: c11_sbuf__write_cstr(buf, "null"); return true;
        case tp_int: c11_sbuf__write_i64(buf, obj->_i64); return true;
        case tp_float: {
            if(dmath_isnan(obj->_f64)) {
                c11_sbuf__write_cstr(buf, "NaN");
            } else if(dmath_isinf(obj->_f64)) {
                c11_sbuf__write_cstr(buf, obj->_f64 < 0 ? "-Infinity" : "Infinity");
            } else {
                c11_sbuf__write_f64(buf, obj->_f64, -1);
            }
            return true;
        }
        case tp_bool: {
            c11_sbuf__write_cstr(buf, py_tobool(obj) ? "true" : "false");
            return true;
        }
        case tp_str: {
            c11_sbuf__write_quoted(buf, py_tosv(obj), '"');
            return true;
        }
        case tp_list: {
            return json__write_array(buf, py_list_data(obj), py_list_len(obj), indent, depth + 1);
        }
        case tp_tuple: {
            return json__write_array(buf, py_tuple_data(obj), py_tuple_len(obj), indent, depth + 1);
        }
        case tp_dict: {
            c11_sbuf__write_char(buf, '{');
            if(py_dict_len(obj) == 0) {
                c11_sbuf__write_char(buf, '}');
                return true;
            }
            if(indent > 0) c11_sbuf__write_char(buf, '\n');
            json__write_dict_kv_ctx ctx = {.buf = buf,
                                           .first = true,
                                           .indent = indent,
                                           .depth = depth + 1};
            bool ok = py_dict_apply(obj, json__write_dict_kv, &ctx);
            if(!ok) return false;
            if(indent > 0) {
                c11_sbuf__write_char(buf, '\n');
                json__write_indent(buf, indent * depth);
            }
            c11_sbuf__write_char(buf, '}');
            return true;
        }
        case tp_namedict: {
            py_Ref original = py_getslot(obj, 0);
            c11_sbuf__write_char(buf, '{');
            if(PyObject__namespace_length(original->_obj) == 0) {
                c11_sbuf__write_char(buf, '}');
                return true;
            }
            if(indent > 0) c11_sbuf__write_char(buf, '\n');
            json__write_dict_kv_ctx ctx = {.buf = buf,
                                           .first = true,
                                           .indent = indent,
                                           .depth = depth + 1};
            bool ok = py_applydict(original, json__write_namedict_kv, &ctx);
            if(!ok) return false;
            if(indent > 0) {
                c11_sbuf__write_char(buf, '\n');
                json__write_indent(buf, indent * depth);
            }
            c11_sbuf__write_char(buf, '}');
            return true;
        }
        default: return TypeError("'%t' object is not JSON serializable", obj->type);
    }
}

bool py_json_dumps(py_Ref val, int indent) {
    c11_sbuf buf;
    c11_sbuf__ctor(&buf);
    bool ok = json__write_object(&buf, val, indent, 0);
    if(!ok) {
        c11_sbuf__dtor(&buf);
        return false;
    }
    c11_sbuf__py_submit(&buf, py_retval());
    return true;
}

/*
 * Direct JSON parser. It builds the values in place, with every container and
 * pending key rooted on the value stack. Input it does not handle exactly like
 * the historical implementation (which evaluates the text as a Python
 * expression in the json module) makes it give up without side effects, and
 * that implementation then runs: extensions such as Python literal syntax,
 * ints beyond int64, lone surrogates and all error reporting stay unchanged.
 */
#define JSON_MAX_DEPTH 512

typedef struct JsonParser {
    const char* p;
    const char* end;
    int depth;
} JsonParser;

static bool json__parse_value(JsonParser* ps, py_OutRef out);

static void json__skip_ws(JsonParser* ps) {
    while(ps->p < ps->end) {
        char c = *ps->p;
        if(c != ' ' && c != '\n' && c != '\t' && c != '\r') break;
        ps->p++;
    }
}

static bool json__match(JsonParser* ps, const char* word, int n) {
    if(ps->end - ps->p < n || memcmp(ps->p, word, (size_t)n) != 0) return false;
    ps->p += n;
    return true;
}

static int json__hex4(const char* p) {
    int value = 0;
    for(int i = 0; i < 4; i++) {
        char c = p[i];
        int digit;
        if(c >= '0' && c <= '9') digit = c - '0';
        else if(c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if(c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return -1;
        value = value * 16 + digit;
    }
    return value;
}

static void json__write_utf8(c11_sbuf* buf, uint32_t cp) {
    if(cp < 0x80) {
        c11_sbuf__write_char(buf, (char)cp);
    } else if(cp < 0x800) {
        c11_sbuf__write_char(buf, (char)(0xC0 | (cp >> 6)));
        c11_sbuf__write_char(buf, (char)(0x80 | (cp & 0x3F)));
    } else if(cp < 0x10000) {
        c11_sbuf__write_char(buf, (char)(0xE0 | (cp >> 12)));
        c11_sbuf__write_char(buf, (char)(0x80 | ((cp >> 6) & 0x3F)));
        c11_sbuf__write_char(buf, (char)(0x80 | (cp & 0x3F)));
    } else {
        c11_sbuf__write_char(buf, (char)(0xF0 | (cp >> 18)));
        c11_sbuf__write_char(buf, (char)(0x80 | ((cp >> 12) & 0x3F)));
        c11_sbuf__write_char(buf, (char)(0x80 | ((cp >> 6) & 0x3F)));
        c11_sbuf__write_char(buf, (char)(0x80 | (cp & 0x3F)));
    }
}

/* ps->p is just past the opening quote. */
static bool json__parse_string(JsonParser* ps, py_OutRef out) {
    const char* start = ps->p;
    const char* p = start;
    while(p < ps->end && *p != '"' && *p != '\\' && (unsigned char)*p >= 0x20) p++;
    if(p >= ps->end || (unsigned char)*p < 0x20) return false;
    if(*p == '"') {
        py_newstrv(out, (c11_sv){start, (int)(p - start)});
        ps->p = p + 1;
        return true;
    }
    c11_sbuf buf;
    c11_sbuf__ctor(&buf);
    c11_sbuf__write_cstrn(&buf, start, (int)(p - start));
    while(true) {
        if(p >= ps->end || (unsigned char)*p < 0x20) goto fail;
        char c = *p++;
        if(c == '"') break;
        if(c != '\\') {
            c11_sbuf__write_char(&buf, c);
            continue;
        }
        if(p >= ps->end) goto fail;
        char e = *p++;
        switch(e) {
            case '"': c11_sbuf__write_char(&buf, '"'); break;
            case '\\': c11_sbuf__write_char(&buf, '\\'); break;
            case '/': c11_sbuf__write_char(&buf, '/'); break;
            case 'b': c11_sbuf__write_char(&buf, '\b'); break;
            case 'f': c11_sbuf__write_char(&buf, '\f'); break;
            case 'n': c11_sbuf__write_char(&buf, '\n'); break;
            case 'r': c11_sbuf__write_char(&buf, '\r'); break;
            case 't': c11_sbuf__write_char(&buf, '\t'); break;
            case 'u': {
                if(ps->end - p < 4) goto fail;
                int cp = json__hex4(p);
                if(cp < 0) goto fail;
                p += 4;
                if(cp >= 0xD800 && cp <= 0xDBFF) {
                    /* Only a complete surrogate pair is a valid code point. */
                    if(ps->end - p < 6 || p[0] != '\\' || p[1] != 'u') goto fail;
                    int low = json__hex4(p + 2);
                    if(low < 0xDC00 || low > 0xDFFF) goto fail;
                    p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if(cp >= 0xDC00 && cp <= 0xDFFF) {
                    goto fail;
                }
                json__write_utf8(&buf, (uint32_t)cp);
                break;
            }
            default: goto fail;
        }
    }
    ps->p = p;
    c11_sbuf__py_submit(&buf, out);
    return true;
fail:
    c11_sbuf__dtor(&buf);
    return false;
}

static bool json__parse_number(JsonParser* ps, py_OutRef out) {
    bool negative = false;
    if(*ps->p == '-') {
        negative = true;
        ps->p++;
        if(json__match(ps, "Infinity", 8)) {
            py_newfloat(out, -DMATH_INFINITY);
            return true;
        }
    }
    const char* start = ps->p;
    const char* p = start;
    if(p >= ps->end || *p < '0' || *p > '9') return false;
    if(*p == '0' && p + 1 < ps->end && p[1] >= '0' && p[1] <= '9') return false; /* leading 0 */
    while(p < ps->end && *p >= '0' && *p <= '9') p++;
    bool is_float = false;
    if(p < ps->end && *p == '.') {
        is_float = true;
        p++;
        if(p >= ps->end || *p < '0' || *p > '9') return false;
        while(p < ps->end && *p >= '0' && *p <= '9') p++;
    }
    if(p < ps->end && (*p == 'e' || *p == 'E')) {
        is_float = true;
        p++;
        if(p < ps->end && (*p == '+' || *p == '-')) p++;
        if(p >= ps->end || *p < '0' || *p > '9') return false;
        while(p < ps->end && *p >= '0' && *p <= '9') p++;
    }
    if(!is_float) {
        /* Same conversion as the lexer; a minus is a unary negation there too. */
        int64_t value;
        if(c11__parse_uint((c11_sv){start, (int)(p - start)}, &value, 10) != IntParsing_SUCCESS)
            return false;
        py_newint(out, negative ? -value : value);
    } else {
        char* parsed_end;
        double value = strtod1(start, &parsed_end);
        if(parsed_end != p) return false;
        py_newfloat(out, negative ? -value : value);
    }
    ps->p = p;
    return true;
}

static bool json__parse_value(JsonParser* ps, py_OutRef out) {
    json__skip_ws(ps);
    if(ps->p >= ps->end) return false;
    switch(*ps->p) {
        case '{': {
            if(++ps->depth > JSON_MAX_DEPTH) return false;
            ps->p++;
            py_StackRef dict = py_pushtmp();
            py_newdict(dict);
            json__skip_ws(ps);
            if(ps->p < ps->end && *ps->p == '}') {
                ps->p++;
            } else {
                py_StackRef key = py_pushtmp();
                py_StackRef value = py_pushtmp();
                while(true) {
                    json__skip_ws(ps);
                    if(ps->p >= ps->end || *ps->p != '"') return false;
                    ps->p++;
                    if(!json__parse_string(ps, key)) return false;
                    json__skip_ws(ps);
                    if(ps->p >= ps->end || *ps->p != ':') return false;
                    ps->p++;
                    if(!json__parse_value(ps, value)) return false;
                    if(!py_dict_setitem(dict, key, value)) return false;
                    json__skip_ws(ps);
                    if(ps->p >= ps->end) return false;
                    if(*ps->p == ',') {
                        ps->p++;
                        continue;
                    }
                    if(*ps->p != '}') return false;
                    ps->p++;
                    break;
                }
                py_shrink(2);
            }
            *out = *dict;
            py_shrink(1);
            ps->depth--;
            return true;
        }
        case '[': {
            if(++ps->depth > JSON_MAX_DEPTH) return false;
            ps->p++;
            py_StackRef list = py_pushtmp();
            py_newlist(list);
            json__skip_ws(ps);
            if(ps->p < ps->end && *ps->p == ']') {
                ps->p++;
            } else {
                py_StackRef item = py_pushtmp();
                while(true) {
                    if(!json__parse_value(ps, item)) return false;
                    py_list_append(list, item);
                    json__skip_ws(ps);
                    if(ps->p >= ps->end) return false;
                    if(*ps->p == ',') {
                        ps->p++;
                        continue;
                    }
                    if(*ps->p != ']') return false;
                    ps->p++;
                    break;
                }
                py_shrink(1);
            }
            *out = *list;
            py_shrink(1);
            ps->depth--;
            return true;
        }
        case '"': ps->p++; return json__parse_string(ps, out);
        case 't':
            if(!json__match(ps, "true", 4)) return false;
            py_newbool(out, true);
            return true;
        case 'f':
            if(!json__match(ps, "false", 5)) return false;
            py_newbool(out, false);
            return true;
        case 'n':
            if(!json__match(ps, "null", 4)) return false;
            py_newnone(out);
            return true;
        case 'N':
            if(!json__match(ps, "NaN", 3)) return false;
            py_newfloat(out, DMATH_NAN);
            return true;
        case 'I':
            if(!json__match(ps, "Infinity", 8)) return false;
            py_newfloat(out, DMATH_INFINITY);
            return true;
        default: return json__parse_number(ps, out);
    }
}

bool py_json_loads(const char* source) {
    VM* vm = pk_current_vm;
    py_StackRef saved_sp = vm->stack.sp;
    JsonParser ps = {source, source + strlen(source), 0};
    py_StackRef result = py_pushtmp();
    if(json__parse_value(&ps, result)) {
        json__skip_ws(&ps);
        if(ps.p == ps.end) {
            vm->last_retval = *result;
            vm->stack.sp = saved_sp;
            return true;
        }
    }
    vm->stack.sp = saved_sp;
    py_GlobalRef mod = py_getmodule("json");
    return py_exec(source, "<json>", EVAL_MODE, mod);
}

