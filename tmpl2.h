// tmpl2.h — compiled template renderer (round 10).
// Fresh implementation: compiles each template's (post-inline) op stream to
// bytecode once per case, then renders it with direct field-index access.
// What it removes vs the interpreter (render_range):
//   - one scope_lookup per VAR/RAW/IF/FOR (37,941/run) -> one per distinct
//     base name per render (~3,300/run), fields by precomputed index.
//   - v_field name scans, fold cond/base re-resolution, loop-state Value
//     allocations (loop.index/first/last/length read from a C stack).
//   - render_range recursion for IF/FOR (iterative pc + loop stack).
//   - buf_reserve per append (inline capacity fast path).
// Anything the compiler cannot prove (unknown base/layout, >3-segment
// paths, non-layout fields) compiles to a DYN instruction that resolves
// the path at render time exactly like the interpreter, so output is
// identical for every template, including hidden-case templates.
// Case-constant globals (site.title etc.) are baked as literal text at
// compile time, exactly like the fold does; arrays/objects stay dynamic.
// Included from engine.c after render_range (needs scope_lookup, v_field,
// truthy_v, text_of_buf, esc_html_buf, fold_global_value, fold_render,
// memo_include, hmap_get, render_range). Forward-declared before
// memo_include so render_range's OP_INCLUDE and memo_include's miss path
// can call into it.

#define C2_TEXT 1
#define C2_OUT 2      // VAR/RAW hole, resolved addressing
#define C2_JUMPFALSE 3
#define C2_JUMP 4
#define C2_FOR 5
#define C2_ENDFOR 6
#define C2_INCLUDE 7
#define C2_DYN 8      // VAR/RAW, resolve path at render
#define C2_DYNJF 9    // IF, resolve path at render
#define C2_DYNFOR 10  // FOR, resolve array path at render
#define C2_END 11

#define C2_WHOLE 0xFFFFu   // field: whole base value
#define C2_LEN 0xFFFEu     // field: array .length
#define C2_LOOPBASE 0xFFFEu // base: the loop pseudo-base (loop.*)
#define C2_NOSLOT 0xFFFFu  // DYN: no frame slot for the root

typedef struct {
  uint8_t kind;
  uint8_t esc;   // C2_OUT: escape the value
  uint8_t not;   // C2_JUMPFALSE/C2_DYNJF: negate
  uint8_t dead;  // C2_FOR: array provably missing/non-array (never taken)
  uint16_t base; // frame slot of the base object (C2_LOOPBASE for loop.*)
  uint16_t field;   // field index, C2_WHOLE, or C2_LEN
  uint32_t jump;    // JUMPFALSE/JUMP/FOR/ENDFOR target pc
  const uint8_t *ptr; // TEXT bytes / INCLUDE name
  uint32_t len;       // TEXT len / INCLUDE name len
  uint16_t varslot;   // FOR/DYNFOR: frame slot bound to the loop variable
  uint16_t field2;    // second field index, C2_WHOLE(none)/C2_LEN
  const void *aux;    // DYN/DYNJF/DYNFOR: const Op* (path + varname)
} C2Ins;

typedef struct { Str name; int slot; } C2Base;
typedef struct {
  C2Ins *ins; int nins;
  C2Base *bases; int nbases;
  int nslots;
} C2Prog;

// ---- object layouts (fixed by the engine's constructors) ----
enum { L_PAGE = 0, L_TAG, L_CARD, L_SITE, L_PAGER, L_TAGELEM, L_TOC, L_LINK, L_TAGSUM };
#define C2K_STR 0
#define C2K_NUM 1
#define C2K_BOOL 2
#define C2K_OBJ 3
#define C2K_ARR 4
typedef struct { const char *name; uint8_t kind; int8_t sub; } C2Field;
// sub: for OBJ/ARR, the layout id of the object / array element.
static const C2Field c2_page_fields[] = {
  {"title", C2K_STR, -1}, {"url", C2K_STR, -1}, {"date", C2K_STR, -1},
  {"date_long", C2K_STR, -1}, {"summary", C2K_STR, -1}, {"excerpt", C2K_STR, -1},
  {"reading_minutes", C2K_NUM, -1}, {"tags", C2K_ARR, L_TAGELEM},
  {"slug", C2K_STR, -1}, {"content", C2K_STR, -1}, {"toc", C2K_ARR, L_TOC},
  {"newer", C2K_OBJ, L_LINK}, {"older", C2K_OBJ, L_LINK}, {"is_post", C2K_BOOL, -1},
};
static const C2Field c2_tag_fields[] = {
  {"name", C2K_STR, -1}, {"slug", C2K_STR, -1}, {"url", C2K_STR, -1},
  {"count", C2K_NUM, -1}, {"posts", C2K_ARR, L_CARD},
};
static const C2Field c2_card_fields[] = {
  {"title", C2K_STR, -1}, {"url", C2K_STR, -1}, {"date", C2K_STR, -1},
  {"date_long", C2K_STR, -1}, {"summary", C2K_STR, -1}, {"excerpt", C2K_STR, -1},
  {"reading_minutes", C2K_NUM, -1}, {"tags", C2K_ARR, L_TAGELEM},
};
static const C2Field c2_site_fields[] = {
  {"title", C2K_STR, -1}, {"base_url", C2K_STR, -1},
};
static const C2Field c2_pager_fields[] = {
  {"number", C2K_NUM, -1}, {"total", C2K_NUM, -1}, {"url", C2K_STR, -1},
  {"newer_url", C2K_STR, -1}, {"older_url", C2K_STR, -1},
};
static const C2Field c2_tagelem_fields[] = {
  {"name", C2K_STR, -1}, {"slug", C2K_STR, -1}, {"url", C2K_STR, -1},
};
static const C2Field c2_toc_fields[] = {
  {"level", C2K_NUM, -1}, {"id", C2K_STR, -1}, {"html", C2K_STR, -1},
};
static const C2Field c2_link_fields[] = {
  {"title", C2K_STR, -1}, {"url", C2K_STR, -1}, {"date", C2K_STR, -1},
  {"date_long", C2K_STR, -1},
};
static const C2Field c2_tagsum_fields[] = {
  {"name", C2K_STR, -1}, {"slug", C2K_STR, -1}, {"url", C2K_STR, -1},
  {"count", C2K_NUM, -1},
};
static const C2Field *c2_layout_fields(int layout, int *n) {
  switch (layout) {
    case L_PAGE: *n = 14; return c2_page_fields;
    case L_TAG: *n = 5; return c2_tag_fields;
    case L_CARD: *n = 8; return c2_card_fields;
    case L_SITE: *n = 2; return c2_site_fields;
    case L_PAGER: *n = 5; return c2_pager_fields;
    case L_TAGELEM: *n = 3; return c2_tagelem_fields;
    case L_TOC: *n = 3; return c2_toc_fields;
    case L_LINK: *n = 4; return c2_link_fields;
    case L_TAGSUM: *n = 4; return c2_tagsum_fields;
    default: *n = 0; return NULL;
  }
}
// Field index of name in layout, or -1. Also returns kind/sub.
static int c2_field_lookup(int layout, Str name, int *kind, int *sub) {
  int n = 0; const C2Field *f = c2_layout_fields(layout, &n);
  if (!f) return -1;
  for (int i = 0; i < n; i++) {
    if (name.len == __builtin_strlen(f[i].name) &&
        __builtin_memcmp(name.p, f[i].name, name.len) == 0) {
      if (kind) *kind = f[i].kind;
      if (sub) *sub = f[i].sub;
      return i;
    }
  }
  return -1;
}
// Layout implied by a well-known base name, or -1. For array-valued names
// the layout describes the ELEMENTS; *is_arr is set.
static int c2_known_base(Str name, int *is_arr) {
  *is_arr = 0;
  if (str_eq(name, str_from("page"))) return L_PAGE;
  if (str_eq(name, str_from("tag"))) return L_TAG;
  if (str_eq(name, str_from("p"))) return L_CARD;
  if (str_eq(name, str_from("pager"))) return L_PAGER;
  if (str_eq(name, str_from("site"))) return L_SITE;
  if (str_eq(name, str_from("posts"))) { *is_arr = 1; return L_CARD; }
  if (str_eq(name, str_from("tags"))) { *is_arr = 1; return L_TAGSUM; }
  if (str_eq(name, str_from("recent"))) { *is_arr = 1; return L_CARD; }
  return -1;
}

// ---- compiler state ----
typedef struct { Str name; int slot; int layout; int is_arr; } C2Bind;
typedef struct {
  Build *b; Arena *a;
  C2Ins *ins; int nins, capins;
  C2Base *bases; int nbases, capbases;
  int nslots;
  C2Bind scope[40]; int nscope;
  int loop_depth; // FOR nesting (for loop.* resolution)
  struct { int pc; int kind; int varslot; int saved_nscope; int saved_loop; } fors[16];
  int nfors;
  struct { int jf; int el; int cv; } ifs[16];
  int nifs;
  int failed;
} C2C;

static int c2_emit(C2C *c, C2Ins in) {
  if (c->nins >= c->capins) {
    int nc = c->capins ? c->capins * 2 : 64;
    C2Ins *ni = arena_alloc(c->a, (size_t)nc * sizeof(C2Ins));
    for (int i = 0; i < c->nins; i++) ni[i] = c->ins[i];
    c->ins = ni; c->capins = nc;
  }
  c->ins[c->nins] = in;
  return c->nins++;
}
static int c2_base_slot(C2C *c, Str name) {
  for (int i = 0; i < c->nbases; i++) if (str_eq(c->bases[i].name, name)) return c->bases[i].slot;
  if (c->nbases >= 32) { c->failed = 1; return -1; }
  if (c->nbases >= c->capbases) {
    int nc = c->capbases ? c->capbases * 2 : 8;
    C2Base *nb = arena_alloc(c->a, (size_t)nc * sizeof(C2Base));
    for (int i = 0; i < c->nbases; i++) nb[i] = c->bases[i];
    c->bases = nb; c->capbases = nc;
  }
  int slot = c->nslots++;
  if (slot >= 32) { c->failed = 1; return -1; }
  c->bases[c->nbases].name = name; c->bases[c->nbases].slot = slot; c->nbases++;
  return slot;
}
// Find a binding for name in the current scope (innermost first).
// Returns 1 + slot/layout/is_arr, or 0.
static int c2_scope_find(C2C *c, Str name, int *slot, int *layout, int *is_arr) {
  for (int i = c->nscope - 1; i >= 0; i--) {
    if (str_eq(c->scope[i].name, name)) {
      *slot = c->scope[i].slot; *layout = c->scope[i].layout; *is_arr = c->scope[i].is_arr;
      return 1;
    }
  }
  return 0;
}

// Resolve a path to compiled addressing.
// Returns 1 if resolved (fields filled) or provably missing (*missing=1);
// returns 0 if it must stay dynamic.
// On success: *base = frame slot, *field/*field2 = indices (C2_WHOLE /
// C2_LEN), *kind = kind of the final value, *elem = element layout when
// the final value is an array.
static int c2_resolve_path(C2C *c, Str *path, int npath, int *base, int *field,
                           int *field2, int *kind, int *elem, int *missing) {
  *missing = 0; *field = C2_WHOLE; *field2 = C2_WHOLE; *kind = C2K_STR; *elem = -1;
  if (npath < 1 || npath > 3) return 0;
  // loop.* pseudo-base (only inside a FOR body)
  if (c->loop_depth > 0 && str_eq(path[0], str_from("loop"))) {
    *base = C2_LOOPBASE;
    if (npath == 1) { *kind = C2K_OBJ; return 1; }
    if (npath == 2) {
      if (str_eq(path[1], str_from("index"))) { *field = 0; *kind = C2K_NUM; return 1; }
      if (str_eq(path[1], str_from("first"))) { *field = 1; *kind = C2K_BOOL; return 1; }
      if (str_eq(path[1], str_from("last"))) { *field = 2; *kind = C2K_BOOL; return 1; }
      if (str_eq(path[1], str_from("length"))) { *field = 3; *kind = C2K_NUM; return 1; }
      *missing = 1; return 1; // loop has no other fields
    }
    return 0;
  }
  int slot, layout, is_arr;
  if (!c2_scope_find(c, path[0], &slot, &layout, &is_arr)) {
    layout = c2_known_base(path[0], &is_arr);
    if (layout < 0) return 0;
    slot = c2_base_slot(c, path[0]);
    if (slot < 0) return 0;
  }
  *base = (uint16_t)slot;
  if (npath == 1) {
    if (is_arr) { *kind = C2K_ARR; *elem = layout; }
    else { *kind = C2K_OBJ; *elem = layout; }
    return 1;
  }
  // walk segments
  int cur_layout = layout;
  int cur_is_arr = is_arr;
  int f1 = C2_WHOLE, f2 = C2_WHOLE;
  for (int k = 1; k < npath; k++) {
    int last = (k == npath - 1);
    if (cur_is_arr) {
      if (str_eq(path[k], str_from("length"))) {
        if (!last) return 0; // length of length: dynamic (never occurs)
        if (k == 1) { *field = C2_LEN; *kind = C2K_NUM; }
        else { *field2 = C2_LEN; *kind = C2K_NUM; }
        return 1;
      }
      // any other segment on an array is missing (scope_lookup: NULL)
      *missing = 1; return 1;
    }
    int fk, fsub;
    int idx = c2_field_lookup(cur_layout, path[k], &fk, &fsub);
    if (idx < 0) { *missing = 1; return 1; }
    if (k == 1) f1 = idx; else f2 = idx;
    if (last) {
      *field = (uint16_t)f1; *field2 = (uint16_t)f2;
      *kind = fk;
      if (fk == C2K_ARR || fk == C2K_OBJ) *elem = fsub;
      return 1;
    }
    // intermediate segment must be an object or array to continue
    if (fk == C2K_OBJ) { cur_layout = fsub; cur_is_arr = 0; }
    else if (fk == C2K_ARR) { cur_layout = fsub; cur_is_arr = 1; }
    else { *missing = 1; return 1; } // field of a scalar: missing
  }
  return 0;
}

// Resolve a path's root to a frame slot for DYN instructions (loop vars and
// named bases have slots; anything else -> C2_NOSLOT, full scope_lookup).
static int c2_dyn_base(C2C *c, Str name) {
  int slot, layout, is_arr;
  if (c2_scope_find(c, name, &slot, &layout, &is_arr)) return slot;
  for (int i = 0; i < c->nbases; i++) if (str_eq(c->bases[i].name, name)) return c->bases[i].slot;
  return C2_NOSLOT;
}

static C2Ins c2_newins(uint8_t kind) {
  C2Ins in;
  __builtin_memset(&in, 0, sizeof(in));
  in.kind = kind; in.field = C2_WHOLE; in.field2 = C2_WHOLE; in.base = C2_NOSLOT;
  return in;
}

// Emit the baked text of a global STR/NUM/BOOL value (VAR escapes it).
static void c2_emit_baked(C2C *c, Value *v, int esc) {
  Buf tmp; buf_init(&tmp, c->a, 32);
  if (v->kind == V_STR) {
    if (esc) esc_html_buf(&tmp, v->s);
    else buf_put_str(&tmp, v->s);
  } else {
    Buf t2; buf_init(&t2, c->a, 32);
    text_of_buf(v, &t2, c->a);
    if (esc) esc_html_buf(&tmp, (Str){ t2.buf, t2.len });
    else buf_put(&tmp, t2.buf, t2.len);
  }
  if (tmp.len == 0) return; // renders "" — emit nothing
  C2Ins in = c2_newins(C2_TEXT);
  in.ptr = tmp.buf; in.len = (uint32_t)tmp.len;
  c2_emit(c, in);
}

// Try to bake a global path (site/tags/recent root) that resolves to a
// scalar. Returns 1 if baked (or provably missing -> *missing), else 0.
static int c2_try_bake_global(C2C *c, Op *op, int esc, int *missing) {
  *missing = 0;
  if (!fold_is_global(op->path[0])) return 0;
  Value *v = fold_global_value(c->b, op->path, op->npath);
  if (!v) { *missing = 1; return 1; } // global path that does not resolve
  if (v->kind == V_STR || v->kind == V_NUM || v->kind == V_BOOL) {
    c2_emit_baked(c, v, esc);
    return 1;
  }
  return 0; // array/object global: resolve dynamically
}

static C2Prog *tmpl2_compile(Build *b, Template *t) {
  Arena *a = b->arena;
  Tmpl *tm = (Tmpl *)t->ops;
  if (!tm || tm->nops == 0) return NULL;
  C2C c;
  __builtin_memset(&c, 0, sizeof(c));
  c.b = b; c.a = a;
  c.ins = arena_alloc(a, 64 * sizeof(C2Ins)); c.capins = 64;
  c.bases = arena_alloc(a, 8 * sizeof(C2Base)); c.capbases = 8;
  for (int i = 0; i < tm->nops; i++) {
    if (c.failed) return NULL;
    Op *op = &tm->ops[i];
    switch (op->kind) {
      case OP_TEXT: {
        C2Ins in = c2_newins(C2_TEXT);
        in.ptr = op->text.p; in.len = (uint32_t)op->text.len;
        c2_emit(&c, in);
        break;
      }
      case OP_VAR:
      case OP_RAW: {
        int missing = 0;
        if (c2_try_bake_global(&c, op, op->kind == OP_VAR, &missing)) break;
        if (missing) break; // provably missing: renders ""
        int base, field, field2, kind, elem, miss2;
        if (c2_resolve_path(&c, op->path, op->npath, &base, &field, &field2, &kind, &elem, &miss2)) {
          if (miss2) break; // provably missing field: renders ""
          C2Ins in = c2_newins(C2_OUT);
          in.esc = (uint8_t)(op->kind == OP_VAR);
          in.base = (uint16_t)base; in.field = (uint16_t)field; in.field2 = (uint16_t)field2;
          c2_emit(&c, in);
        } else {
          C2Ins in = c2_newins(C2_DYN);
          in.esc = (uint8_t)(op->kind == OP_VAR);
          in.base = (uint16_t)c2_dyn_base(&c, op->path[0]);
          in.aux = op;
          c2_emit(&c, in);
        }
        break;
      }
      case OP_IF: {
        int cv = -1; // -1 unknown, 0 const-false, 1 const-true
        int missing = 0;
        if (fold_is_global(op->path[0])) {
          Value *v = fold_global_value(b, op->path, op->npath);
          if (!v) cv = 0;
          else if (v->kind == V_STR || v->kind == V_NUM || v->kind == V_BOOL || v->kind == V_ARR || v->kind == V_OBJ || v->kind == V_NULL || v->kind == V_UNDEF)
            cv = truthy_v(v) ? 1 : 0;
        }
        if (cv < 0) {
          int base, field, field2, kind, elem, miss2;
          if (c2_resolve_path(&c, op->path, op->npath, &base, &field, &field2, &kind, &elem, &miss2)) {
            if (miss2) cv = 0;
            else if (base == C2_LOOPBASE && field == C2_WHOLE) cv = 1; // loop object is truthy
            else {
              C2Ins in = c2_newins(C2_JUMPFALSE);
              in.not = (uint8_t)op->not;
              in.base = (uint16_t)base; in.field = (uint16_t)field; in.field2 = (uint16_t)field2;
              int pc = c2_emit(&c, in);
              if (c.nifs >= 16) { c.failed = 1; break; }
              c.ifs[c.nifs].jf = pc; c.ifs[c.nifs].el = -1; c.ifs[c.nifs].cv = -1; c.nifs++;
              break;
            }
          } else {
            C2Ins in = c2_newins(C2_DYNJF);
            in.not = (uint8_t)op->not;
            in.base = (uint16_t)c2_dyn_base(&c, op->path[0]);
            in.aux = op;
            int pc = c2_emit(&c, in);
            if (c.nifs >= 16) { c.failed = 1; break; }
            c.ifs[c.nifs].jf = pc; c.ifs[c.nifs].el = -1; c.ifs[c.nifs].cv = -1; c.nifs++;
            break;
          }
        }
        // constant condition: emit a JUMP for const-false (patched at
        // ELSE/ENDIF); const-true emits nothing here.
        if (c.nifs >= 16) { c.failed = 1; break; }
        int jf = -1;
        if (cv == 0) { C2Ins in = c2_newins(C2_JUMP); jf = c2_emit(&c, in); }
        c.ifs[c.nifs].jf = jf; c.ifs[c.nifs].el = -1; c.ifs[c.nifs].cv = cv; c.nifs++;
        break;
      }
      case OP_ELSE: {
        if (c.nifs == 0) { c.failed = 1; break; }
        int t = c.nifs - 1;
        if (c.ifs[t].cv == 1) {
          C2Ins in = c2_newins(C2_JUMP);
          c.ifs[t].el = c2_emit(&c, in);
        } else if (c.ifs[t].cv == 0) {
          c.ins[c.ifs[t].jf].jump = (uint32_t)c.nins;
          c.ifs[t].jf = -1;
        } else {
          c.ins[c.ifs[t].jf].jump = (uint32_t)(c.nins + 1);
          C2Ins in = c2_newins(C2_JUMP);
          c.ifs[t].el = c2_emit(&c, in);
        }
        break;
      }
      case OP_ENDIF: {
        if (c.nifs == 0) { c.failed = 1; break; }
        int t = --c.nifs;
        if (c.ifs[t].el >= 0) c.ins[c.ifs[t].el].jump = (uint32_t)c.nins;
        else if (c.ifs[t].jf >= 0) c.ins[c.ifs[t].jf].jump = (uint32_t)c.nins;
        break;
      }
      case OP_FOR: {
        if (c.nfors >= 16 || c.loop_depth >= 16) { c.failed = 1; break; }
        int base, field, field2, kind, elem, miss2;
        int is_dyn = 0, is_dead = 0;
        if (c2_resolve_path(&c, op->path, op->npath, &base, &field, &field2, &kind, &elem, &miss2)) {
          if (miss2 || kind != C2K_ARR) is_dead = 1;
        } else is_dyn = 1;
        int varslot = c.nslots++;
        if (varslot >= 32) { c.failed = 1; break; }
        int fpc;
        if (is_dead) {
          C2Ins in = c2_newins(C2_JUMP);
          fpc = c2_emit(&c, in);
        } else if (is_dyn) {
          C2Ins in = c2_newins(C2_DYNFOR);
          in.varslot = (uint16_t)varslot;
          in.base = (uint16_t)c2_dyn_base(&c, op->path[0]);
          in.aux = op;
          in.ptr = op->varname.p; in.len = (uint32_t)op->varname.len;
          fpc = c2_emit(&c, in);
        } else {
          C2Ins in = c2_newins(C2_FOR);
          in.base = (uint16_t)base; in.field = (uint16_t)field; in.field2 = (uint16_t)field2;
          in.varslot = (uint16_t)varslot;
          in.ptr = op->varname.p; in.len = (uint32_t)op->varname.len;
          fpc = c2_emit(&c, in);
        }
        // push the loop-var binding (element layout when known)
        if (c.nscope >= 40) { c.failed = 1; break; }
        int elayout = is_dyn ? -1 : (kind == C2K_ARR ? elem : -1);
        c.scope[c.nscope].name = op->varname; c.scope[c.nscope].slot = varslot;
        c.scope[c.nscope].layout = elayout; c.scope[c.nscope].is_arr = 0; c.nscope++;
        c.fors[c.nfors].pc = fpc; c.fors[c.nfors].kind = is_dead ? 1 : (is_dyn ? 2 : 0);
        c.fors[c.nfors].varslot = varslot; c.fors[c.nfors].saved_nscope = c.nscope - 1;
        c.fors[c.nfors].saved_loop = c.loop_depth; c.nfors++;
        c.loop_depth++;
        break;
      }
      case OP_ENDFOR: {
        if (c.nfors == 0) { c.failed = 1; break; }
        int t = --c.nfors;
        c.nscope = c.fors[t].saved_nscope;
        c.loop_depth = c.fors[t].saved_loop;
        if (c.fors[t].kind == 1) {
          c.ins[c.fors[t].pc].jump = (uint32_t)c.nins; // SKIP jumps here
        } else {
          C2Ins in = c2_newins(C2_ENDFOR);
          in.jump = (uint32_t)(c.fors[t].pc + 1); // body start
          int epc = c2_emit(&c, in);
          c.ins[c.fors[t].pc].jump = (uint32_t)(epc + 1); // after ENDFOR
        }
        break;
      }
      case OP_INCLUDE: {
        C2Ins in = c2_newins(C2_INCLUDE);
        in.ptr = op->incname.p; in.len = (uint32_t)op->incname.len;
        c2_emit(&c, in);
        break;
      }
      default: break; // OP_ENDIF/OP_ENDFOR handled above; OP_ELSE above
    }
  }
  if (c.failed || c.nifs != 0 || c.nfors != 0 || c.loop_depth != 0) return NULL;
  C2Ins end = c2_newins(C2_END);
  c2_emit(&c, end);
  C2Prog *pr = arena_alloc(a, sizeof(C2Prog));
  pr->ins = c.ins; pr->nins = c.nins;
  pr->bases = c.bases; pr->nbases = c.nbases;
  pr->nslots = c.nslots;
  return pr;
}

static void tmpl2_compile_all(Build *b) {
  for (int i = 0; i < b->ntemplates; i++) {
    Template *t = &b->templates[i];
    t->c2 = NULL;
    if (t->parse_err) continue;
    t->c2 = tmpl2_compile(b, t);
  }
}

// ---- render ----
static void c2_put(Buf *out, const uint8_t *p, size_t n) {
  if (n == 0) return;
  if (out->len + n <= out->cap) {
    __builtin_memcpy(out->buf + out->len, p, n);
    out->len += n;
  } else buf_put(out, p, n);
}
static void c2_put_num(Buf *out, int64_t x) {
  char tmp[24]; int n = 0;
  if (x == 0) tmp[n++] = '0';
  else {
    int neg = x < 0;
    uint64_t u = neg ? (uint64_t)(-(x + 1)) + 1 : (uint64_t)x;
    char rev[24]; int rn = 0;
    while (u > 0) { rev[rn++] = (char)('0' + u % 10); u /= 10; }
    if (neg) tmp[n++] = '-';
    while (rn > 0) tmp[n++] = rev[--rn];
  }
  c2_put(out, (const uint8_t *)tmp, (size_t)n);
}
// Resolve a normal (non-length, non-loop) path to its Value*, or NULL.
static Value *c2_val(Value **frame, C2Ins *in) {
  Value *v = frame[in->base];
  if (!v) return NULL;
  if (in->field == C2_WHOLE) return v;
  if (in->field == C2_LEN) return NULL;
  if (v->kind != V_OBJ || (uint32_t)in->field >= v->nfields) return NULL;
  v = v->fields[in->field].v;
  if (in->field2 == C2_WHOLE) return v;
  if (in->field2 == C2_LEN) return NULL;
  if (!v || v->kind != V_OBJ || (uint32_t)in->field2 >= v->nfields) return NULL;
  return v->fields[in->field2].v;
}
// Resolve an array path (FOR): returns the array Value* or NULL.
static Value *c2_arr(Value **frame, C2Ins *in) {
  Value *v = frame[in->base];
  if (!v) return NULL;
  if (in->field == C2_WHOLE) return v->kind == V_ARR ? v : NULL;
  if (in->field == C2_LEN) return NULL;
  if (v->kind != V_OBJ || (uint32_t)in->field >= v->nfields) return NULL;
  v = v->fields[in->field].v;
  if (in->field2 == C2_WHOLE) return (v && v->kind == V_ARR) ? v : NULL;
  if (in->field2 == C2_LEN) return NULL;
  if (!v || v->kind != V_OBJ || (uint32_t)in->field2 >= v->nfields) return NULL;
  v = v->fields[in->field2].v;
  return (v && v->kind == V_ARR) ? v : NULL;
}
// Length of a .length path; returns 1 + *n, or 0 if not an array.
static int c2_len(Value **frame, C2Ins *in, int64_t *n) {
  Value *arr = NULL;
  if (in->field == C2_LEN) arr = frame[in->base];
  else if (in->field2 == C2_LEN) {
    Value *v = frame[in->base];
    if (!v || v->kind != V_OBJ || (uint32_t)in->field >= v->nfields) return 0;
    arr = v->fields[in->field].v;
  } else return 0;
  if (!arr || arr->kind != V_ARR) return 0;
  *n = (int64_t)arr->nitems;
  return 1;
}
// Dynamic path resolution at render time (interpreter semantics): root
// from the frame when it has a slot, else a full scope_lookup.
static Value *c2_dyn_value2(Value **frame, int base_slot, Scope *scope, Op *op, Arena *a) {
  Value *v = NULL;
  if (base_slot != C2_NOSLOT) v = frame[base_slot];
  else return scope_lookup(scope, op->path, op->npath, a);
  if (!v) return NULL;
  for (int k = 1; k < op->npath; k++) {
    if (!v) return NULL;
    if (v->kind == V_ARR) {
      if (str_eq(op->path[k], str_from("length"))) { v = v_num(a, (int64_t)v->nitems); continue; }
      return NULL;
    }
    if (v->kind != V_OBJ) return NULL;
    v = v_field(v, op->path[k]);
    if (!v) return NULL;
  }
  return v;
}

typedef struct {
  Value *arr; size_t idx; int varslot;
  // loop object materialized for the scope chain (callees see the loop
  // var + loop exactly as the interpreter provides them); fields are
  // mutated per iteration, mirroring render_range's OP_FOR.
  Value *loop_obj, *lv_index, *lv_first, *lv_last, *lv_length;
  Str names[2]; Value *vals[2]; Scope sc;
} C2Loop;

static void c2_run(C2Prog *pr, Build *b, Scope *scope, TmplCtx *tctx, int depth, Buf *out) {
  Arena *a = b->arena;
  Value *frame[32];
  for (int i = 0; i < 32; i++) frame[i] = NULL;
  for (int i = 0; i < pr->nbases; i++)
    frame[pr->bases[i].slot] = scope_lookup(scope, &pr->bases[i].name, 1, a);
  C2Ins *ins = pr->ins;
  int pc = 0;
  C2Loop loops[16]; int lsp = 0;
  Scope *cur_scope = scope;
  // Enter a FOR: build the child scope (loop var + loop object) and the
  // loop state. The loop object's fields are mutated per iteration.
  // Returns 1 if the loop runs, 0 if it is empty (caller jumps).
  // (Implemented inline in the C2_FOR / C2_DYNFOR cases below via macro.)
  for (;;) {
    C2Ins *in = &ins[pc];
    switch (in->kind) {
      case C2_TEXT:
        c2_put(out, in->ptr, in->len);
        pc++;
        break;
      case C2_OUT: {
        if (in->base == C2_LOOPBASE) {
          // loop.* as a value: index/length are numbers, first/last bools,
          // whole loop renders "" (object -> text_of_buf default).
          if (in->field == 0 || in->field == 3) {
            int64_t n = 0;
            if (lsp > 0) {
              if (in->field == 0) n = (int64_t)(loops[lsp - 1].idx + 1);
              else n = (int64_t)loops[lsp - 1].arr->nitems;
            }
            c2_put_num(out, n);
          } else if (in->field == 1 || in->field == 2) {
            int bv = 0;
            if (lsp > 0) {
              if (in->field == 1) bv = (loops[lsp - 1].idx == 0);
              else bv = (loops[lsp - 1].idx == loops[lsp - 1].arr->nitems - 1);
            }
            if (bv) c2_put(out, (const uint8_t *)"true", 4);
            else c2_put(out, (const uint8_t *)"false", 5);
          }
          // field == C2_WHOLE: renders "" — nothing to append.
          pc++;
          break;
        }
        if (in->field == C2_LEN || in->field2 == C2_LEN) {
          int64_t n = 0;
          if (c2_len(frame, in, &n)) c2_put_num(out, n);
          pc++;
          break;
        }
        Value *v = c2_val(frame, in);
        if (v && v->kind == V_STR) {
          if (in->esc) esc_html_buf(out, v->s);
          else c2_put(out, v->s.p, v->s.len);
        } else if (v && v->kind == V_NUM) {
          c2_put_num(out, v->num);
        } else if (v && v->kind == V_BOOL) {
          if (v->boolean) c2_put(out, (const uint8_t *)"true", 4);
          else c2_put(out, (const uint8_t *)"false", 5);
        } else if (v) {
          text_of_buf(v, out, a);
        }
        pc++;
        break;
      }
      case C2_DYN: {
        Op *op = (Op *)in->aux;
        Value *v = (op->npath == 1 && in->base == C2_NOSLOT)
          ? scope_lookup(cur_scope, op->path, 1, a)
          : c2_dyn_value2(frame, in->base, cur_scope, op, a);
        if (v && v->kind == V_STR) {
          if (in->esc) esc_html_buf(out, v->s);
          else c2_put(out, v->s.p, v->s.len);
        } else if (v) {
          Buf tmp; buf_init(&tmp, a, 64);
          text_of_buf(v, &tmp, a);
          if (in->esc) esc_html_buf(out, (Str){ tmp.buf, tmp.len });
          else c2_put(out, tmp.buf, tmp.len);
        }
        pc++;
        break;
      }
      case C2_JUMPFALSE: {
        int tv;
        if (in->base == C2_LOOPBASE) {
          if (in->field == C2_WHOLE) tv = 1;
          else if (in->field == 0) tv = (lsp > 0); // index >= 1
          else if (in->field == 1) tv = (lsp > 0 && loops[lsp - 1].idx == 0);
          else if (in->field == 2) tv = (lsp > 0 && loops[lsp - 1].idx == loops[lsp - 1].arr->nitems - 1);
          else tv = (lsp > 0 && loops[lsp - 1].arr->nitems != 0); // length
        } else if (in->field == C2_LEN || in->field2 == C2_LEN) {
          int64_t n = 0;
          tv = (c2_len(frame, in, &n) && n != 0);
        } else {
          Value *v = c2_val(frame, in);
          tv = truthy_v(v);
        }
        if ((tv != 0) == (in->not != 0)) pc = (int)in->jump; // condition fails: jump
        else pc++;
        break;
      }
      case C2_DYNJF: {
        Op *op = (Op *)in->aux;
        Value *v = (op->npath == 1 && in->base == C2_NOSLOT)
          ? scope_lookup(cur_scope, op->path, 1, a)
          : c2_dyn_value2(frame, in->base, cur_scope, op, a);
        int tv = truthy_v(v);
        if ((tv != 0) == (in->not != 0)) pc = (int)in->jump;
        else pc++;
        break;
      }
      case C2_JUMP:
        pc = (int)in->jump;
        break;
      case C2_FOR: {
        Value *arr = c2_arr(frame, in);
        if (!arr || arr->nitems == 0 || lsp >= 16) { pc = (int)in->jump; break; }
        C2Loop *lp = &loops[lsp];
        lp->arr = arr; lp->idx = 0; lp->varslot = in->varslot;
        lp->lv_index = v_num(a, 1); lp->lv_first = v_bool(a, 1);
        lp->lv_last = v_bool(a, arr->nitems == 1); lp->lv_length = v_num(a, (int64_t)arr->nitems);
        Field *lf = arena_alloc(a, 4 * sizeof(Field));
        lf[0].name = str_from("index"); lf[0].v = lp->lv_index;
        lf[1].name = str_from("first"); lf[1].v = lp->lv_first;
        lf[2].name = str_from("last"); lf[2].v = lp->lv_last;
        lf[3].name = str_from("length"); lf[3].v = lp->lv_length;
        lp->loop_obj = v_obj(a, lf, 4);
        lp->names[0] = (Str){ in->ptr, in->len }; lp->vals[0] = arr->items[0];
        lp->names[1] = str_from("loop"); lp->vals[1] = lp->loop_obj;
        lp->sc.names = lp->names; lp->sc.vals = lp->vals; lp->sc.n = 2; lp->sc.parent = cur_scope;
        cur_scope = &lp->sc;
        frame[in->varslot] = arr->items[0];
        lsp++;
        pc++;
        break;
      }
      case C2_DYNFOR: {
        Op *op = (Op *)in->aux;
        Value *arr = (op->npath == 1 && in->base == C2_NOSLOT)
          ? scope_lookup(cur_scope, op->path, 1, a)
          : c2_dyn_value2(frame, in->base, cur_scope, op, a);
        if (!arr || arr->kind != V_ARR || arr->nitems == 0 || lsp >= 16) { pc = (int)in->jump; break; }
        C2Loop *lp = &loops[lsp];
        lp->arr = arr; lp->idx = 0; lp->varslot = in->varslot;
        lp->lv_index = v_num(a, 1); lp->lv_first = v_bool(a, 1);
        lp->lv_last = v_bool(a, arr->nitems == 1); lp->lv_length = v_num(a, (int64_t)arr->nitems);
        Field *lf = arena_alloc(a, 4 * sizeof(Field));
        lf[0].name = str_from("index"); lf[0].v = lp->lv_index;
        lf[1].name = str_from("first"); lf[1].v = lp->lv_first;
        lf[2].name = str_from("last"); lf[2].v = lp->lv_last;
        lf[3].name = str_from("length"); lf[3].v = lp->lv_length;
        lp->loop_obj = v_obj(a, lf, 4);
        lp->names[0] = (Str){ in->ptr, in->len }; lp->vals[0] = arr->items[0];
        lp->names[1] = str_from("loop"); lp->vals[1] = lp->loop_obj;
        lp->sc.names = lp->names; lp->sc.vals = lp->vals; lp->sc.n = 2; lp->sc.parent = cur_scope;
        cur_scope = &lp->sc;
        frame[in->varslot] = arr->items[0];
        lsp++;
        pc++;
        break;
      }
      case C2_ENDFOR: {
        C2Loop *lp = &loops[lsp - 1];
        if (++lp->idx < lp->arr->nitems) {
          frame[lp->varslot] = lp->arr->items[lp->idx];
          lp->vals[0] = lp->arr->items[lp->idx];
          lp->lv_index->num = (int64_t)(lp->idx + 1);
          lp->lv_first->boolean = 0;
          lp->lv_last->boolean = (lp->idx == lp->arr->nitems - 1);
          pc = (int)in->jump;
        } else {
          cur_scope = lp->sc.parent;
          lsp--;
          pc++;
        }
        break;
      }
      case C2_INCLUDE: {
        if (depth < 8) {
          Str name = { in->ptr, in->len };
          tmpl2_do_include(b, name, cur_scope, tctx, depth, out);
        }
        pc++;
        break;
      }
      case C2_END:
      default:
        return;
    }
  }
}

static int tmpl2_render_template(Build *b, Template *t, Scope *scope, TmplCtx *tctx, int depth, Buf *out) {
  C2Prog *pr = (C2Prog *)t->c2;
  if (!pr) return 0;
  c2_run(pr, b, scope, tctx, depth, out);
  return 1;
}

// Include handling shared by the interpreter and the compiled renderer:
// fold first, then the render memo, then the compiled program, then the
// interpreter. (Mirrors the old OP_INCLUDE body, plus the compiled step.)
static int tmpl2_do_include(Build *b, Str name, Scope *scope, TmplCtx *tctx, int depth, Buf *out) {
  Template *t = hmap_get(&b->template_map, name);
  if (!t || t->parse_err) return 0;
  Tmpl *tm = (Tmpl *)t->ops;
  if (t->fold) {
#ifdef ENGINE_TEST_MAIN
    c_fold_renders++;
#endif
    if (fold_render((Folded *)t->fold, scope, tctx, out)) return 1;
#ifdef ENGINE_TEST_MAIN
    c_fold_fallback++;
#endif
  }
  if (memo_include(tm, t, scope, tctx, depth, out)) return 1;
  if (tmpl2_render_template(b, t, scope, tctx, depth + 1, out)) return 1;
  render_range(tm->ops, 0, tm->nops, scope, tctx, depth + 1, out);
  return 1;
}
