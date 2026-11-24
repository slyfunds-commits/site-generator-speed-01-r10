// md2.h — markdown renderer, round 10 (fresh implementation).
// Same semantics as vendor markdown.js (lib/markdown.js + lib/text.js),
// byte-exact port, proven by the differential tests (1,024 practice
// bodies + 113 edge + 4,000 fuzz against the JS reference via md_core).
// Differences vs md_core.c (the round-9 port, kept for reference):
//   - works directly on the source range (no IN_BUF copy) and writes
//     directly into the engine Buf (no OUT_BUF copy, no JOIN_BUF);
//   - output via __builtin_memcpy fast appends (md_memcpy was a 4-byte
//     loop); block classification inlined into the main loop;
//   - single-line paragraphs/items/quotes render inline straight from the
//     source range (no join copy); multi-line ones join into one arena
//     scratch buffer reused across blocks;
//   - the output Buf is pre-reserved to ~2x the body so it grows once.
// Included from engine.c after render_markdown's dependencies (needs Str,
// Buf, Arena, LinkCtx, MdOut, md_toc_push, linkFn, slugFn, scratch_ptr,
// scratch2_ptr, buf_put, buf_reserve, arena_alloc, str_from, str_eq).
// linkFn/slugFn are the engine's (non-static) imports, as in md_core.

typedef struct {
  const uint8_t *src, *end;
  Buf *out;
  LinkCtx *ctx;
  Arena *arena;
  MdOut *res;
  Str *ids; int nids, capids;      // unique-slug set (arena Strs)
  uint8_t *join; size_t join_cap;  // scratch for multi-line blocks
  int excerpt_set;
  size_t excerpt_off, excerpt_len; // offsets into out->buf
  struct { int level; size_t id_off, id_len, html_off, html_len; } *toc;
  int ntoc, captoc;
  uint8_t slug[96]; // current slug working buffer
  int slug_len;
  // classifier state (set by md2_classify / md2_fence_open / md2_heading)
  const uint8_t *fence_lang_p; int fence_lang_len;
  int head_depth; const uint8_t *head_text_p; int head_text_len;
  int list_ordered; int list_start;
} MD2;

#define MD2_PUT(m, p, n) do { \
  Buf *_o = (m)->out; size_t _n = (size_t)(n); \
  if (_n) { \
    if (_o->len + _n <= _o->cap) { __builtin_memcpy(_o->buf + _o->len, (p), _n); _o->len += _n; } \
    else buf_put(_o, (p), _n); \
  } \
} while (0)
#define MD2_LIT(m, s) MD2_PUT(m, (const uint8_t *)(s), sizeof(s) - 1)
#define MD2_CH(m, c) do { \
  Buf *_o = (m)->out; \
  if (_o->len + 1 <= _o->cap) _o->buf[_o->len++] = (uint8_t)(c); \
  else buf_putc(_o, (uint8_t)(c)); \
} while (0)

extern int linkFn(const uint8_t *p, int len);
extern int slugFn(const uint8_t *p, int len);

/* ---- SIMD scanners ---- */
#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#include <immintrin.h>
__attribute__((target("avx2")))
static const uint8_t *md2_find_byte_avx2(const uint8_t *p, const uint8_t *end, uint8_t c) {
  __m256i vc = _mm256_set1_epi8((char)c);
  while (end - p >= 32) {
    unsigned mm = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)p), vc));
    if (mm) return p + __builtin_ctz(mm);
    p += 32;
  }
  while (p < end) { if (*p == c) return p; p++; }
  return end;
}
#endif
static const uint8_t *md2_find_byte(const uint8_t *p, const uint8_t *end, uint8_t c) {
#if defined(__x86_64__) || defined(__i386__)
  if (eng_use_avx2()) return md2_find_byte_avx2(p, end, c);
  __m128i vc = _mm_set1_epi8((char)c);
  while (end - p >= 16) {
    unsigned mm = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)p), vc));
    if (mm) return p + __builtin_ctz(mm);
    p += 16;
  }
#endif
  while (p < end) { if (*p == c) return p; p++; }
  return end;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static const uint8_t *md2_find_esc_avx2(const uint8_t *p, const uint8_t *end) {
  __m256i am = _mm256_set1_epi8('&'), lt = _mm256_set1_epi8('<'), gt = _mm256_set1_epi8('>'),
          dq = _mm256_set1_epi8('"'), sq = _mm256_set1_epi8('\'');
  while (end - p >= 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    __m256i mm = _mm256_cmpeq_epi8(v, am);
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, lt));
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, gt));
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, dq));
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, sq));
    unsigned mask = (unsigned)_mm256_movemask_epi8(mm);
    if (mask) return p + __builtin_ctz(mask);
    p += 32;
  }
  while (p < end) {
    uint8_t c = *p;
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return p;
    p++;
  }
  return end;
}
#endif
static const uint8_t *md2_find_esc(const uint8_t *p, const uint8_t *end) {
#if defined(__x86_64__) || defined(__i386__)
  if (eng_use_avx2()) return md2_find_esc_avx2(p, end);
  __m128i am = _mm_set1_epi8('&'), lt = _mm_set1_epi8('<'), gt = _mm_set1_epi8('>'),
          dq = _mm_set1_epi8('"'), sq = _mm_set1_epi8('\'');
  while (end - p >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i mm = _mm_cmpeq_epi8(v, am);
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, lt));
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, gt));
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, dq));
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, sq));
    unsigned mask = (unsigned)_mm_movemask_epi8(mm);
    if (mask) return p + __builtin_ctz(mask);
    p += 16;
  }
#endif
  while (p < end) {
    uint8_t c = *p;
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return p;
    p++;
  }
  return end;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2")))
static const uint8_t *md2_find_special_avx2(const uint8_t *p, const uint8_t *end) {
  __m256i bs = _mm256_set1_epi8('\\'), bt = _mm256_set1_epi8('`'), st = _mm256_set1_epi8('*'), ob = _mm256_set1_epi8('['),
          am = _mm256_set1_epi8('&'), lt = _mm256_set1_epi8('<'), gt = _mm256_set1_epi8('>'),
          dq = _mm256_set1_epi8('"'), sq = _mm256_set1_epi8('\'');
  while (end - p >= 32) {
    __m256i v = _mm256_loadu_si256((const __m256i *)p);
    __m256i mm = _mm256_cmpeq_epi8(v, bs);
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, bt));
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, st));
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, ob));
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, am));
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, lt));
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, gt));
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, dq));
    mm = _mm256_or_si256(mm, _mm256_cmpeq_epi8(v, sq));
    unsigned mask = (unsigned)_mm256_movemask_epi8(mm);
    if (mask) return p + __builtin_ctz(mask);
    p += 32;
  }
  while (p < end) {
    uint8_t c = *p;
    if (c == '\\' || c == '`' || c == '*' || c == '[' || c == '&' ||
        c == '<' || c == '>' || c == '"' || c == '\'')
      return p;
    p++;
  }
  return end;
}
#endif
static const uint8_t *md2_find_special(const uint8_t *p, const uint8_t *end) {
#if defined(__x86_64__) || defined(__i386__)
  if (eng_use_avx2()) return md2_find_special_avx2(p, end);
  __m128i bs = _mm_set1_epi8('\\'), bt = _mm_set1_epi8('`'), st = _mm_set1_epi8('*'), ob = _mm_set1_epi8('['),
          am = _mm_set1_epi8('&'), lt = _mm_set1_epi8('<'), gt = _mm_set1_epi8('>'),
          dq = _mm_set1_epi8('"'), sq = _mm_set1_epi8('\'');
  while (end - p >= 16) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i mm = _mm_cmpeq_epi8(v, bs);
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, bt));
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, st));
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, ob));
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, am));
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, lt));
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, gt));
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, dq));
    mm = _mm_or_si128(mm, _mm_cmpeq_epi8(v, sq));
    unsigned mask = (unsigned)_mm_movemask_epi8(mm);
    if (mask) return p + __builtin_ctz(mask);
    p += 16;
  }
#endif
  while (p < end) {
    uint8_t c = *p;
    if (c == '\\' || c == '`' || c == '*' || c == '[' || c == '&' ||
        c == '<' || c == '>' || c == '"' || c == '\'')
      return p;
    p++;
  }
  return end;
}

/* HTML-escape bytes [p, p+len) into the output. */
static void md2_esc(MD2 *m, const uint8_t *p, size_t len) {
  const uint8_t *end = p + len;
  const uint8_t *cur = p;
  while (cur < end) {
    const uint8_t *q = md2_find_esc(cur, end);
    if (q > cur) MD2_PUT(m, cur, (size_t)(q - cur));
    if (q >= end) break;
    switch (*q) {
      case '&': MD2_LIT(m, "&amp;"); break;
      case '<': MD2_LIT(m, "&lt;"); break;
      case '>': MD2_LIT(m, "&gt;"); break;
      case '"': MD2_LIT(m, "&quot;"); break;
      default: MD2_LIT(m, "&#39;"); break;
    }
    cur = q + 1;
  }
}

/* Is the char at p (UTF-8) a JS \s whitespace char? Byte length, 0 if not. */
static int md2_space_len_at(const uint8_t *p, const uint8_t *end) {
  if (p >= end) return 0;
  uint8_t c = *p;
  if (c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32) return 1;
  if (c == 0xC2 && p + 1 < end && p[1] == 0xA0) return 2;
  if (c == 0xE1 && p + 2 < end && p[1] == 0x9A && p[2] == 0x80) return 3;
  if (c == 0xE2 && p + 2 < end && p[1] == 0x80) {
    uint8_t c2 = p[2];
    if ((c2 >= 0x80 && c2 <= 0x8A) || c2 == 0xA8 || c2 == 0xA9 || c2 == 0xAF) return 3;
  }
  if (c == 0xE2 && p + 2 < end && p[1] == 0x81 && p[2] == 0x9F) return 3;
  if (c == 0xE3 && p + 2 < end && p[1] == 0x80 && p[2] == 0x80) return 3;
  if (c == 0xEF && p + 2 < end && p[1] == 0xBB && p[2] == 0xBF) return 3;
  return 0;
}
static int md2_has_ws(const uint8_t *p, size_t len) {
  const uint8_t *end = p + len;
  const uint8_t *i = p;
  while (i < end) {
    int n = md2_space_len_at(i, end);
    if (n > 0) return 1;
    i++;
  }
  return 0;
}

static int md2_is_escapable(uint8_t c) {
  return c == 92 || c == 96 || c == 42 || c == 95 || c == 91 || c == 93 || c == 40 || c == 41 ||
         c == 35 || c == 43 || c == 45 || c == 46 || c == 33 || c == 62;
}
static int md2_escaped_before(const uint8_t *pos, const uint8_t *from) {
  int n = 0;
  for (const uint8_t *k = pos - 1; k >= from && *k == 92; k--) n++;
  return (n & 1) == 1;
}
static const uint8_t *md2_find_unescaped_starstar(const uint8_t *from, const uint8_t *to) {
  const uint8_t *i = from;
  while (i + 1 < to) {
    if (i[0] == 42 && i[1] == 42) {
      if (i + 2 > to) return NULL;
      if (!md2_escaped_before(i, from)) return i;
      i++;
    } else {
      i++;
    }
  }
  return NULL;
}
static const uint8_t *md2_find_unescaped_bracket(const uint8_t *from, const uint8_t *to) {
  const uint8_t *i = from;
  while (i < to) {
    if (*i == 93) {
      if (!md2_escaped_before(i, from)) return i;
      i++;
    } else {
      i++;
    }
  }
  return NULL;
}
static const uint8_t *md2_find_single_star(const uint8_t *from, const uint8_t *to) {
  const uint8_t *i = from;
  while (i < to) {
    if (*i != 42) { i++; continue; }
    const uint8_t *start = i;
    const uint8_t *end = i;
    while (end < to && *end == 42) end++;
    if (start >= to) return NULL;
    int odd = (((end - start) & 1) == 1);
    if (odd != md2_escaped_before(start, from)) return end - 1;
    i = end;
  }
  return NULL;
}

/* ---- slugs ---- */
static int md2_slug_ascii(MD2 *m, const uint8_t *p, int len) {
  int o = 0;
  int gap = 0;
  for (int i = 0; i < len; i++) {
    uint8_t c = p[i];
    if (c > 127) return -1;
    int keep = -1;
    if (c >= 65 && c <= 90) keep = c + 32;
    else if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) keep = c;
    if (keep < 0) {
      if (o > 0) gap = 1;
      continue;
    }
    if (o >= 64) break;
    if (gap) {
      m->slug[o++] = 45;
      gap = 0;
      if (o >= 64) break;
    }
    m->slug[o++] = (uint8_t)keep;
  }
  while (o > 0 && m->slug[o - 1] == 45) o--;
  if (o == 0) {
    static const char unt[] = "untitled";
    __builtin_memcpy(m->slug, unt, 8);
    return 8;
  }
  return o;
}
static int md2_append_suffix(MD2 *m, int base_len, int n) {
  int o = base_len;
  m->slug[o++] = 45;
  int tmp = n, digits = 0, rev = 0;
  while (tmp > 0) { rev = rev * 10 + (tmp % 10); tmp /= 10; digits++; }
  if (digits == 0) { m->slug[o++] = 48; }
  else {
    for (int i = 0; i < digits; i++) { m->slug[o++] = (uint8_t)(48 + (rev % 10)); rev /= 10; }
  }
  return o;
}
/* uniqueSlug: dedupe against the body's ids; final id in m->slug, returns len. */
static int md2_unique_slug(MD2 *m, const uint8_t *p, int len) {
  int slen = md2_slug_ascii(m, p, len);
  if (slen < 0) {
    int n = slugFn(p, len);
    if (n <= 0) {
      static const char unt[] = "untitled";
      __builtin_memcpy(m->slug, unt, 8);
      slen = 8;
    } else {
      uint8_t *sc = scratch2_ptr();
      uint32_t got;
      __builtin_memcpy(&got, sc, 4);
      __builtin_memcpy(m->slug, sc + 4, got);
      slen = (int)got;
    }
  }
  int base_len = slen;
  int n = 2;
  while (1) {
    int found = 0;
    for (int i = 0; i < m->nids; i++) {
      if (m->ids[i].len == (size_t)slen &&
          __builtin_memcmp(m->ids[i].p, m->slug, (size_t)slen) == 0) { found = 1; break; }
    }
    if (!found) break;
    slen = md2_append_suffix(m, base_len, n);
    n++;
  }
  if (m->nids >= m->capids) {
    int nc = m->capids ? m->capids * 2 : 16;
    Str *ni = arena_alloc(m->arena, (size_t)nc * sizeof(Str));
    for (int i = 0; i < m->nids; i++) ni[i] = m->ids[i];
    m->ids = ni; m->capids = nc;
  }
  uint8_t *cp = arena_alloc(m->arena, (size_t)(slen > 0 ? slen : 1));
  __builtin_memcpy(cp, m->slug, (size_t)slen);
  m->ids[m->nids].p = cp; m->ids[m->nids].len = (size_t)slen; m->nids++;
  m->slug_len = slen;
  return slen;
}

/* ---- inline renderer ---- */
static void md2_inline(MD2 *m, const uint8_t *from, const uint8_t *to);

static void md2_inline(MD2 *m, const uint8_t *from, const uint8_t *to) {
  const uint8_t *i = from;
  const uint8_t *run = from;
  const uint8_t *n = to;
  while (1) {
    i = md2_find_special(i, n);
    if (i >= n) break;
    uint8_t code = *i;
    if (code == 92) {
      if (i + 1 < n && md2_is_escapable(i[1])) {
        if (run < i) MD2_PUT(m, run, (size_t)(i - run));
        uint8_t e = i[1];
        if (e == 38) MD2_LIT(m, "&amp;");
        else if (e == 60) MD2_LIT(m, "&lt;");
        else if (e == 62) MD2_LIT(m, "&gt;");
        else if (e == 34) MD2_LIT(m, "&quot;");
        else if (e == 39) MD2_LIT(m, "&#39;");
        else MD2_CH(m, e);
        i += 2;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (code == 96) {
      const uint8_t *j = md2_find_byte(i + 1, n, 96);
      if (j < n && j > i + 1) {
        if (run < i) MD2_PUT(m, run, (size_t)(i - run));
        MD2_LIT(m, "<code>");
        md2_esc(m, i + 1, (size_t)(j - (i + 1)));
        MD2_LIT(m, "</code>");
        i = j + 1;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (code == 42) {
      if (i + 1 < n && i[1] == 42) {
        const uint8_t *j = md2_find_unescaped_starstar(i + 2, n);
        if (j && j > i + 2) {
          if (run < i) MD2_PUT(m, run, (size_t)(i - run));
          MD2_LIT(m, "<strong>");
          md2_inline(m, i + 2, j);
          MD2_LIT(m, "</strong>");
          i = j + 2;
          run = i;
        } else {
          i += 2;
        }
        continue;
      }
      const uint8_t *j = md2_find_single_star(i + 1, n);
      if (j && j > i + 1) {
        if (run < i) MD2_PUT(m, run, (size_t)(i - run));
        MD2_LIT(m, "<em>");
        md2_inline(m, i + 1, j);
        MD2_LIT(m, "</em>");
        i = j + 1;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (code == 91) {
      const uint8_t *k = md2_find_unescaped_bracket(i + 1, n);
      const uint8_t *m2 = NULL;
      if (k && k + 1 < n && k[1] == 40) m2 = md2_find_byte(k + 2, n, 41);
      const uint8_t *target_ptr = NULL;
      size_t target_len = 0;
      if (m2) { target_ptr = k + 2; target_len = (size_t)(m2 - (k + 2)); }
      if (target_len > 0 && !md2_has_ws(target_ptr, target_len)) {
        if (run < i) MD2_PUT(m, run, (size_t)(i - run));
        MD2_LIT(m, "<a href=\"");
        int r = linkFn(target_ptr, (int)target_len);
        if (r == 1) {
          uint8_t *sc = scratch_ptr();
          uint32_t l;
          __builtin_memcpy(&l, sc, 4);
          md2_esc(m, sc + 4, l);
        } else {
          md2_esc(m, target_ptr, target_len);
        }
        MD2_LIT(m, "\">");
        md2_inline(m, i + 1, k);
        MD2_LIT(m, "</a>");
        i = m2 + 1;
        run = i;
      } else {
        i++;
      }
      continue;
    }
    if (run < i) MD2_PUT(m, run, (size_t)(i - run));
    if (code == 38) MD2_LIT(m, "&amp;");
    else if (code == 60) MD2_LIT(m, "&lt;");
    else if (code == 62) MD2_LIT(m, "&gt;");
    else if (code == 34) MD2_LIT(m, "&quot;");
    else MD2_LIT(m, "&#39;");
    i++;
    run = i;
  }
  if (run < n) MD2_PUT(m, run, (size_t)(n - run));
}

/* ---- line helpers ---- */
static const uint8_t *md2_line_end(const uint8_t *p, const uint8_t *N) {
  return md2_find_byte(p, N, 10);
}
static void md2_trim(const uint8_t *a, const uint8_t *b, const uint8_t **ta, const uint8_t **tb) {
  while (a < b && (*a == 32 || *a == 9)) a++;
  while (b > a && (b[-1] == 32 || b[-1] == 9)) b--;
  *ta = a; *tb = b;
}
static int md2_is_blank(const uint8_t *a, const uint8_t *b) {
  for (const uint8_t *k = a; k < b; k++) { if (*k != 32 && *k != 9) return 0; }
  return 1;
}
/* fence open: lang length (>=0) or -1; lang ptr/len in m->fence_lang. */
static int md2_fence_open(MD2 *m, const uint8_t *a, const uint8_t *b) {
  if (b - a < 3) return -1;
  if (a[0] != 96 || a[1] != 96 || a[2] != 96) return -1;
  const uint8_t *j = a + 3;
  while (j < b) {
    uint8_t c = *j;
    if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) ||
        c == 95 || c == 43 || c == 45) j++;
    else break;
  }
  m->fence_lang_p = a + 3;
  m->fence_lang_len = (int)(j - (a + 3));
  while (j < b && (*j == 32 || *j == 9)) j++;
  return j == b ? m->fence_lang_len : -1;
}
static int md2_is_fence_close(const uint8_t *a, const uint8_t *b) {
  if (b - a < 3) return 0;
  if (a[0] != 96 || a[1] != 96 || a[2] != 96) return 0;
  for (const uint8_t *k = a + 3; k < b; k++) { if (*k != 32 && *k != 9) return 0; }
  return 1;
}
static int md2_heading(MD2 *m, const uint8_t *a, const uint8_t *b) {
  int d = 0;
  while (a + d < b && a[d] == 35) d++;
  if (d == 0 || d > 6 || a + d >= b) return 0;
  uint8_t c = a[d];
  if (c != 32 && c != 9) return 0;
  const uint8_t *st = a + d;
  while (st < b && (*st == 32 || *st == 9)) st++;
  const uint8_t *e = b;
  while (e > st && (e[-1] == 32 || e[-1] == 9)) e--;
  if (e <= st) return 0;
  m->head_depth = d;
  m->head_text_p = st;
  m->head_text_len = (int)(e - st);
  return 1;
}
static int md2_is_hr(const uint8_t *a, const uint8_t *b) {
  if (b - a < 3) return 0;
  uint8_t c = a[0];
  if (c != 45 && c != 42) return 0;
  if (a[1] != c || a[2] != c) return 0;
  for (const uint8_t *k = a + 3; k < b; k++) { if (*k != 32 && *k != 9) return 0; }
  return 1;
}
/* list start: 1 if [a,b) opens a list; sets ordered/start. */
static int md2_list_start(MD2 *m, const uint8_t *a, const uint8_t *b) {
  uint8_t c0 = a[0];
  if (c0 == 45 || c0 == 42) {
    if (b - a >= 2 && a[1] == 32) { m->list_ordered = 0; m->list_start = 1; return 1; }
    return 0;
  }
  if (c0 >= 48 && c0 <= 57) {
    const uint8_t *j = a;
    while (j < b && *j >= 48 && *j <= 57) j++;
    if (j - a >= 1 && j - a <= 9 && j + 1 < b && *j == 46 && j[1] == 32) {
      int v = 0;
      for (const uint8_t *k = a; k < j; k++) v = v * 10 + (*k - 48);
      m->list_ordered = 1;
      m->list_start = v;
      return 1;
    }
  }
  return 0;
}
static int md2_is_continued(const uint8_t *a, const uint8_t *b) {
  if (b - a < 2) return 0;
  if (a[0] != 32 || a[1] != 32) return 0;
  const uint8_t *k = a;
  while (k < b && *k == 32) k++;
  if (k >= b) return 0;
  return md2_space_len_at(k, b) == 0;
}
/* classify: 1 blank, 2 fence, 3 heading, 4 hr, 5 quote, 6 list, 7 other. */
static int md2_classify(MD2 *m, const uint8_t *a, const uint8_t *b) {
  if (a == b) return 1;
  if (md2_is_blank(a, b)) return 1;
  uint8_t c0 = a[0];
  if (c0 == 96) {
    if (md2_fence_open(m, a, b) >= 0) return 2;
  } else if (c0 == 35) {
    if (md2_heading(m, a, b)) return 3;
  } else if (c0 == 45 || c0 == 42) {
    if (md2_is_hr(a, b)) return 4;
  } else if (c0 == 62) {
    return 5;
  }
  if (md2_list_start(m, a, b)) return 6;
  return 7;
}
/* Whether line [a,b) ends a paragraph: blank, fence, heading or hr. */
static int md2_ends_paragraph(MD2 *m, const uint8_t *a, const uint8_t *b) {
  if (a == b) return 1;
  if (md2_is_blank(a, b)) return 1;
  uint8_t c0 = a[0];
  if (c0 == 96) { if (md2_fence_open(m, a, b) >= 0) return 1; }
  else if (c0 == 35) { if (md2_heading(m, a, b)) return 1; }
  else if (c0 == 45 || c0 == 42) { if (md2_is_hr(a, b)) return 1; }
  return 0;
}

/* Joiner: accumulates trimmed parts; a single part stays a source range
 * (no copy), multiple parts join with '\n' in the scratch buffer. */
typedef struct {
  MD2 *m;
  const uint8_t *first_p; size_t first_len;
  int count;
  size_t scratch_len; // bytes used in m->join
} MD2Join;
static void md2_join_init(MD2Join *j, MD2 *m) {
  j->m = m; j->first_p = NULL; j->first_len = 0; j->count = 0; j->scratch_len = 0;
  if (!m->join) {
    size_t cap = (size_t)(m->end - m->src);
    if (cap == 0) cap = 1;
    m->join = arena_alloc(m->arena, cap);
    m->join_cap = cap;
  }
}
static void md2_join_add(MD2Join *j, const uint8_t *p, size_t len) {
  MD2 *m = j->m;
  if (j->count == 0) {
    j->first_p = p; j->first_len = len; j->count = 1;
    return;
  }
  if (j->count == 1) {
    __builtin_memcpy(m->join, j->first_p, j->first_len);
    j->scratch_len = j->first_len;
  }
  if (j->scratch_len + 1 + len > m->join_cap) {
    // cannot happen: joined text is bounded by the body length, and the
    // scratch holds exactly one block at a time; guard anyway.
    return;
  }
  m->join[j->scratch_len++] = 10;
  __builtin_memcpy(m->join + j->scratch_len, p, len);
  j->scratch_len += len;
  j->count++;
}
static void md2_join_text(MD2Join *j, const uint8_t **p, size_t *len) {
  if (j->count <= 1) { *p = j->first_p; *len = j->first_len; }
  else { *p = j->m->join; *len = j->scratch_len; }
}
static void md2_toc_record(MD2 *m, int level, size_t id_off, size_t id_len, size_t html_off, size_t html_len) {
  if (m->ntoc >= m->captoc) {
    int nc = m->captoc ? m->captoc * 2 : 16;
    void *ni = arena_alloc(m->arena, (size_t)nc * sizeof(*m->toc));
    for (int i = 0; i < m->ntoc; i++) ((__typeof__(m->toc))ni)[i] = m->toc[i];
    m->toc = ni; m->captoc = nc;
  }
  m->toc[m->ntoc].level = level;
  m->toc[m->ntoc].id_off = id_off; m->toc[m->ntoc].id_len = id_len;
  m->toc[m->ntoc].html_off = html_off; m->toc[m->ntoc].html_len = html_len;
  m->ntoc++;
}

/* ---- main renderer ---- */
static void md2_render(Str src, LinkCtx *ctx, Buf *out, MdOut *res) {
  MD2 m;
  __builtin_memset(&m, 0, sizeof(m));
  m.src = src.p; m.end = src.p + src.len;
  m.out = out; m.ctx = ctx; m.arena = out->arena; m.res = res;
  g_md_ctx = ctx;
  g_md_arena = out->arena;
  // Pre-reserve the output so the common case grows once (html is usually
  // ~1.2-1.5x the body; the PUT macro still handles growth).
  if (src.len > 0) buf_reserve(out, src.len * 2 + 64);
  const uint8_t *N = m.end;
  const uint8_t *pos = m.src;
  while (pos <= N) {
    const uint8_t *a = pos;
    const uint8_t *b = md2_line_end(pos, N);
    const uint8_t *next_pos = b + 1;
    int k = md2_classify(&m, a, b);
    if (k == 1) { pos = next_pos; if (b == N) break; continue; }
    if (k == 2) {
      /* fence */
      int lang_len = m.fence_lang_len;
      const uint8_t *lang_ptr = m.fence_lang_p;
      MD2_LIT(&m, "<pre><code");
      if (lang_len > 0) { MD2_LIT(&m, " class=\"language-"); md2_esc(&m, lang_ptr, (size_t)lang_len); MD2_LIT(&m, "\""); }
      MD2_LIT(&m, ">");
      const uint8_t *body_start = next_pos;
      const uint8_t *body_end = N;
      int closed = 0;
      const uint8_t *p2 = next_pos;
      while (p2 <= N) {
        const uint8_t *a2 = p2;
        const uint8_t *b2 = md2_line_end(p2, N);
        if (b2 > a2 && md2_is_fence_close(a2, b2)) { body_end = a2; closed = 1; p2 = b2 + 1; break; }
        if (b2 == N) { body_end = N; p2 = N + 1; break; }
        p2 = b2 + 1;
      }
      if (closed) {
        if (body_end > body_start) md2_esc(&m, body_start, (size_t)(body_end - body_start));
      } else if (b < N) {
        md2_esc(&m, body_start, (size_t)(N - body_start));
        MD2_CH(&m, 10);
      }
      MD2_LIT(&m, "</code></pre>\n");
      pos = closed ? p2 : N + 1;
      if (pos > N) break;
      continue;
    }
    if (k == 3) {
      /* heading */
      int depth = m.head_depth;
      const uint8_t *t_ptr = m.head_text_p;
      int t_len = m.head_text_len;
      int slen = md2_unique_slug(&m, t_ptr, t_len);
      MD2_LIT(&m, "<h");
      MD2_CH(&m, (uint8_t)(48 + depth));
      MD2_LIT(&m, " id=\"");
      size_t id_off = out->len;
      MD2_PUT(&m, m.slug, (size_t)slen);
      MD2_LIT(&m, "\">");
      size_t html_start = out->len;
      md2_inline(&m, t_ptr, t_ptr + t_len);
      size_t html_len = out->len - html_start;
      MD2_LIT(&m, "</h");
      MD2_CH(&m, (uint8_t)(48 + depth));
      MD2_LIT(&m, ">\n");
      if (depth == 2 || depth == 3) md2_toc_record(&m, depth, id_off, (size_t)slen, html_start, html_len);
      pos = next_pos;
      if (b == N) break;
      continue;
    }
    if (k == 4) {
      MD2_LIT(&m, "<hr>\n");
      pos = next_pos;
      if (b == N) break;
      continue;
    }
    if (k == 5) {
      /* quote */
      MD2Join j; md2_join_init(&j, &m);
      const uint8_t *p2 = pos;
      while (p2 <= N) {
        const uint8_t *a2 = p2;
        const uint8_t *b2 = md2_line_end(p2, N);
        if (!(b2 > a2 && *a2 == 62)) break;
        const uint8_t *st = (a2 + 1 < b2 && a2[1] == 32) ? a2 + 2 : a2 + 1;
        const uint8_t *ta, *tb;
        md2_trim(st, b2, &ta, &tb);
        if (ta < tb) md2_join_add(&j, ta, (size_t)(tb - ta));
        if (b2 == N) { p2 = N + 1; break; }
        p2 = b2 + 1;
      }
      if (j.count == 0) {
        MD2_LIT(&m, "<blockquote>\n</blockquote>\n");
      } else {
        MD2_LIT(&m, "<blockquote>\n<p>");
        const uint8_t *tp; size_t tl;
        md2_join_text(&j, &tp, &tl);
        md2_inline(&m, tp, tp + tl);
        MD2_LIT(&m, "</p>\n</blockquote>\n");
      }
      pos = p2;
      if (p2 > N) break;
      continue;
    }
    if (k == 6) {
      /* list */
      int is_ordered = m.list_ordered;
      int first_start = m.list_start;
      if (is_ordered && first_start != 1) {
        MD2_LIT(&m, "<ol start=\"");
        int v = first_start;
        if (v == 0) { MD2_CH(&m, 48); }
        else {
          int digits = 0, rev = 0;
          while (v > 0) { rev = rev * 10 + (v % 10); v /= 10; digits++; }
          for (int i = 0; i < digits; i++) { MD2_CH(&m, (uint8_t)(48 + (rev % 10))); rev /= 10; }
        }
        MD2_LIT(&m, "\">\n");
      } else if (is_ordered) {
        MD2_LIT(&m, "<ol>\n");
      } else {
        MD2_LIT(&m, "<ul>\n");
      }
      /* Current item, rendered when the next item begins (or at the end). */
      MD2Join cur; md2_join_init(&cur, &m);
      int have_item = 0;
      const uint8_t *tstart;
      if (!is_ordered) tstart = a + 2;
      else { const uint8_t *jj = a; while (jj < b && *jj >= 48 && *jj <= 57) jj++; tstart = jj + 2; }
      {
        const uint8_t *ta, *tb;
        md2_trim(tstart, b, &ta, &tb);
        md2_join_add(&cur, ta, (size_t)(tb - ta));
        have_item = 1;
      }
      const uint8_t *p2 = next_pos;
      while (p2 <= N) {
        const uint8_t *a2 = p2;
        const uint8_t *b2 = md2_line_end(p2, N);
        if (b2 == N && a2 == N) break;
        int k2 = md2_classify(&m, a2, b2);
        if (k2 == 6 && m.list_ordered == is_ordered) {
          MD2_LIT(&m, "<li>");
          const uint8_t *tp; size_t tl;
          md2_join_text(&cur, &tp, &tl);
          md2_inline(&m, tp, tp + tl);
          MD2_LIT(&m, "</li>\n");
          md2_join_init(&cur, &m);
          const uint8_t *ts;
          if (!is_ordered) ts = a2 + 2;
          else { const uint8_t *jj = a2; while (jj < b2 && *jj >= 48 && *jj <= 57) jj++; ts = jj + 2; }
          const uint8_t *ta, *tb;
          md2_trim(ts, b2, &ta, &tb);
          md2_join_add(&cur, ta, (size_t)(tb - ta));
          have_item = 1;
          if (b2 == N) { p2 = N + 1; break; }
          p2 = b2 + 1;
        } else if (b2 - a2 >= 2 && a2[0] == 32 && a2[1] == 32 && md2_is_continued(a2, b2)) {
          const uint8_t *ta, *tb;
          md2_trim(a2, b2, &ta, &tb);
          md2_join_add(&cur, ta, (size_t)(tb - ta));
          if (b2 == N) { p2 = N + 1; break; }
          p2 = b2 + 1;
        } else break;
      }
      if (have_item) {
        MD2_LIT(&m, "<li>");
        const uint8_t *tp; size_t tl;
        md2_join_text(&cur, &tp, &tl);
        md2_inline(&m, tp, tp + tl);
        MD2_LIT(&m, "</li>\n");
      }
      if (is_ordered) MD2_LIT(&m, "</ol>\n"); else MD2_LIT(&m, "</ul>\n");
      pos = p2;
      if (p2 > N) break;
      continue;
    }
    /* paragraph */
    {
      MD2Join j; md2_join_init(&j, &m);
      const uint8_t *ta, *tb;
      md2_trim(a, b, &ta, &tb);
      md2_join_add(&j, ta, (size_t)(tb - ta));
      const uint8_t *p2 = next_pos;
      while (p2 <= N) {
        const uint8_t *a2 = p2;
        const uint8_t *b2 = md2_line_end(p2, N);
        if (b2 == N && a2 == N) break;
        if (md2_ends_paragraph(&m, a2, b2)) break;
        const uint8_t *ta2, *tb2;
        md2_trim(a2, b2, &ta2, &tb2);
        md2_join_add(&j, ta2, (size_t)(tb2 - ta2));
        if (b2 == N) { p2 = N + 1; break; }
        p2 = b2 + 1;
      }
      MD2_LIT(&m, "<p>");
      const uint8_t *tp; size_t tl;
      md2_join_text(&j, &tp, &tl);
      if (!m.excerpt_set) {
        size_t html_start = out->len;
        md2_inline(&m, tp, tp + tl);
        m.excerpt_off = html_start;
        m.excerpt_len = out->len - html_start;
        m.excerpt_set = 1;
      } else {
        md2_inline(&m, tp, tp + tl);
      }
      MD2_LIT(&m, "</p>\n");
      pos = p2;
      if (p2 > N) break;
    }
  }
  /* Copy excerpt + toc out of the output buffer into the arena. */
  if (m.excerpt_set && m.excerpt_len > 0) {
    uint8_t *ep = arena_alloc(res->arena, m.excerpt_len);
    __builtin_memcpy(ep, out->buf + m.excerpt_off, m.excerpt_len);
    res->excerpt.p = ep; res->excerpt.len = m.excerpt_len; res->has_excerpt = 1;
  }
  for (int i = 0; i < m.ntoc; i++) {
    size_t id_len = m.toc[i].id_len, h_len = m.toc[i].html_len;
    uint8_t *ip = arena_alloc(res->arena, id_len ? id_len : 1);
    __builtin_memcpy(ip, out->buf + m.toc[i].id_off, id_len);
    uint8_t *hp = arena_alloc(res->arena, h_len ? h_len : 1);
    __builtin_memcpy(hp, out->buf + m.toc[i].html_off, h_len);
    md_toc_push(res, m.toc[i].level, (Str){ ip, id_len }, (Str){ hp, h_len });
  }
}
