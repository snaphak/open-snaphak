/* overrides.c -- see overrides.h. The OVERRIDES FILE-SHADOW resource loader.
 *
 * Swaps the engine resource-provider's open-by-name vtable slot (+0xf8) with our override-open hook.
 * On each engine open the resolution is THREE-LAYER:
 *   1. USER    -- overrides/<name> under %USERPROFILE%\snaphak\ on disk (an explicit user act; wins).
 *   2. BUILT-IN -- our baked default decls (overrides_baked.h), served FROM MEMORY. Nothing is ever
 *                  written to the user's folder, so defaults update with every release and "reset to
 *                  default" is simply deleting the user's file.
 *   3. ENGINE  -- chain to the saved original engine open (the packaged resource).
 * A mode>=2 recursion guard goes straight to the original (OG's `param_5 >= 2` branch). For a BUILT-IN
 * name only, a user file that fails a minimal well-formedness check (brace/quote balance) is refused and
 * the built-in default serves instead (logged) -- a garbled file there would take out the "*Custom" tab.
 * The user layer can be disabled for bisecting a broken override set via the snaphak_user_overrides
 * cvar (or by renaming the overrides folder, which also covers opens before the cvar applies).
 *
 * Install-time passes (both logged, both SEH-guarded):
 *   - RECLAIM: earlier releases WROTE the baked defaults to the user's folder if absent. A user file
 *     byte-equal to a baked default (CR bytes ignored) is provably ours-untouched -> deleted, so the
 *     memory layer serves current defaults. A differing file is user-owned -> kept, shadowing.
 *   - AUDIT: enumerate the active user override files into the log, so "what is shadowing what" is
 *     always answerable from the log alone.
 *
 * Clean-room: ported from our own RE (overrides.h header).
 * Zero OG SnapHak bytes. Every disk/engine touch is SEH-guarded -- a shadow failure degrades to a
 * vanilla engine open, never a crash.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */
#include "overrides.h"
#include "backend_log.h"
#include "cvars.h"                  /* sh_cvar_value_int_reg + B2_CVAR_SNAPHAK_USER_OVERRIDES */
#include "overrides_baked.h"        /* the built-in "*Custom"-tab default decls (Timeline + Unknown) */

/* The engine open-by-name vtable method offset within the resource-provider vtable.
 * DIRECT: OG patches engineBase+0x2798598; the vtable is engineBase+0x27984a0 -> slot offset = 0xf8. */
#define OPEN_SLOT_OFFSET 0xf8

/* The engine open method ABI (DIRECT, from OG FUN_18000b370's own call shape):
 *   void* open(void* this, const char* name, uint8 b1, uint8 b2, uint mode)   // __fastcall, returns idFile*
 * OG masks b1/b2 to 0xff when chaining. mode>=2 -> straight to original (recursion guard). */
typedef void *(*open_fn_t)(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode);

static open_fn_t  g_orig_open  = NULL;   /* the saved engine resource-open (the slot's original value) */
static void     **g_slot       = NULL;   /* the live vtable slot we patched (for uninstall) */
static volatile LONG g_shadow_count = 0;

/* The overrides ROOT (holds overrides\ + overrides\shader_includes\). Default %USERPROFILE%\snaphak. */
static char g_root[MAX_PATH] = {0};

/* ============================================================ our idFile-subclass stream ===========
 * A reimplementation of OG's PTR_FUN_18003d050 stream (the engine idFile interface, 24 virtual
 * methods -- every slot decompiled in pb1-overrides). The object layout keeps OG's public head:
 *   +0x00 vtable   +0x08 FILE*   +0x10 name   +0x18 length   +0x20 short flag   +0x21 byte flag
 * The engine reads the resource through this vtable; the dtor (slot 0) frees the object with OUR
 * allocator (HeapFree) -- so we need no engine allocator/free (OG used the engine's only so the engine
 * could free it; here every method incl. the dtor is ours).
 *
 * TWO BACKINGS, one vtable: fp != NULL -> FILE*-backed (a user override file, OG-equivalent);
 * fp == NULL && buf != NULL -> MEMORY-backed (a built-in default, or a validated user file already read
 * whole). The memory form is read-only (Write/printf return 0) and tracks its own cursor in `pos`;
 * owns_buf says the dtor must HeapFree the buffer (a heap copy) vs leave it (the static baked text). */
typedef struct ov_stream {
    void        *vtable;     /* +0x00 */
    FILE        *fp;         /* +0x08 (NULL for a memory-backed stream) */
    const char  *name;       /* +0x10 (points at the heap-dup'd name appended after the struct) */
    long long    length;     /* +0x18 */
    short        flag16;     /* +0x20 (OG sets 1) */
    char         flag8;      /* +0x22-ish via +0x21 read in slot 20; OG returns *(this+0x21) */
    const unsigned char *buf;/* memory backing (baked static text, or an owned heap copy) */
    long long    pos;        /* memory-backing read cursor */
    int          owns_buf;   /* 1 -> dtor HeapFrees buf */
} ov_stream;

/* --- the 24 vtable methods, faithful to the OG slot semantics (every slot decompiled) --------------
 * All __fastcall(this in RCX). Behaviour matches OG FUN_18000ae00..b070 exactly, expressed in stdio. */

static void  ov_dtor(ov_stream *s)                                   /* [0] close + free(this) */
{
    if (s) {
        if (s->fp) { fclose(s->fp); s->fp = NULL; }
        if (s->buf && s->owns_buf) HeapFree(GetProcessHeap(), 0, (void *)s->buf);
        s->buf = NULL; s->length = 0; s->name = NULL; s->flag16 = 0;
        HeapFree(GetProcessHeap(), 0, s);
    }
}
static long long ov_ret0_a(ov_stream *s)        { (void)s; return 0; }   /* [1] return 0 */
static long long ov_ret0_b(ov_stream *s)        { (void)s; return 0; }   /* [2] return 0 */
static long long ov_length(ov_stream *s)        { return s ? s->length : 0; }            /* [3] *(this+0x18) */
static const char *ov_name(ov_stream *s)        { return s ? s->name : NULL; }           /* [4] *(this+0x10) */
static long long ov_read(ov_stream *s, void *buf, unsigned int n)    /* [5] fread(buf,1,n,fp) */
{
    if (!s || !buf) return 0;
    if (s->fp) return (long long)fread(buf, 1, n, s->fp);
    if (s->buf) {                                        /* memory backing: bounded copy + cursor */
        long long avail = s->length - s->pos;
        long long take  = (avail < (long long)n) ? avail : (long long)n;
        if (take <= 0) return 0;
        memcpy(buf, s->buf + s->pos, (size_t)take);
        s->pos += take;
        return take;
    }
    return 0;
}
static long long ov_write(ov_stream *s, const void *buf, unsigned int n)  /* [6] fwrite(buf,1,n,fp); memory form is read-only */
{
    if (!s || !s->fp || !buf) return 0;
    return (long long)fwrite(buf, 1, n, s->fp);
}
static int       ov_seek(ov_stream *s, long long off, int origin);       /* fwd-decl ([14]) */
/* [7] OG FUN_18000aeb0 -> engine 0x1a1b520 = (Seek(this,off,SEEK_END-ish=2)); (Read(this,buf,len)).
 *     We reproduce the same combo through our own methods (the engine fn only dispatched via the vtable). */
static long long ov_seekread(ov_stream *s, long long off, void *buf, unsigned int n)
{
    if (!s) return 0;
    ov_seek(s, off, 2);
    return ov_read(s, buf, n);
}
/* [8] OG FUN_18000aec0 -> engine 0x1a1c220 (an analogous base helper). Same conservative seek+read
 *     reproduction; the slot is only exercised by engine paths that pre-position then read. */
static long long ov_seekread2(ov_stream *s, long long off, void *buf, unsigned int n)
{
    if (!s) return 0;
    ov_seek(s, off, 0);
    return ov_read(s, buf, n);
}
static int       ov_lock(ov_stream *s)          { if (s && s->fp) _lock_file(s->fp);   return 1; }   /* [9] */
static int       ov_unlock(ov_stream *s)        { if (s && s->fp) _unlock_file(s->fp); return 1; }   /* [10] */
static long long ov_length_byseek(ov_stream *s)                      /* [11] tell/seek-end/tell/restore */
{
    if (!s) return 0;
    if (!s->fp) return s->buf ? s->length : 0;           /* memory backing: length is already known */
    long long pos = _ftelli64(s->fp);
    _fseeki64(s->fp, 0, SEEK_END);
    long long len = _ftelli64(s->fp);
    _fseeki64(s->fp, pos, SEEK_SET);
    return len;
}
static void      ov_noop(ov_stream *s)          { (void)s; }                                          /* [12] RET 0 */
static long long ov_tell(ov_stream *s)                                                                /* [13] ftell */
{
    if (!s) return 0;
    if (s->fp) return _ftelli64(s->fp);
    return s->buf ? s->pos : 0;
}
static int       ov_seek(ov_stream *s, long long off, int origin)    /* [14] fseek; OG maps 0->SET,1->END(2),else CUR */
{
    if (!s) return -1;
    if (!s->fp) {                                        /* memory backing: move the cursor, clamped */
        if (!s->buf) return -1;
        long long p;
        if (origin == 0)      p = off;                   /* SET */
        else if (origin == 1) p = s->length + off;       /* END */
        else                  p = s->pos + off;          /* CUR */
        if (p < 0) p = 0;
        if (p > s->length) p = s->length;
        s->pos = p;
        return 0;
    }
    int o = SEEK_CUR;
    if (origin == 0) o = SEEK_SET;
    else if (origin == 1) o = SEEK_END;
    return _fseeki64(s->fp, off, o);
}
static long long ov_vprintf(ov_stream *s, const char *fmt, va_list ap)   /* [15] vfprintf */
{
    if (!s || !s->fp || !fmt) return 0;
    return (long long)vfprintf(s->fp, fmt, ap);
}
/* OG slot [15]/[16] are the C-varargs printf forms (vfprintf into fp). The engine resource-READ path
 * never calls them; we provide a faithful vfprintf so a write path stays correct. The vtable stores a
 * single entry; both OG slots resolve to a vfprintf-to-fp, so we point both at this thunk. */
static long long ov_printf_thunk(ov_stream *s, const char *fmt, ...)     /* [15]/[16] varargs entry */
{
    va_list ap; long long r;
    va_start(ap, fmt);
    r = ov_vprintf(s, fmt, ap);
    va_end(ap);
    return r;
}
static long long ov_ret0_c(ov_stream *s)        { (void)s; return 0; }   /* [17] return 0 */
static long long ov_ret0_d(ov_stream *s)        { (void)s; return 0; }   /* [18] return 0 */
static long long ov_ret0_e(ov_stream *s)        { (void)s; return 0; }   /* [19] return 0 */
static char      ov_flag8(ov_stream *s)         { return s ? s->flag8 : 0; }              /* [20] *(this+0x21) */
static void      ov_flush_a(ov_stream *s)       { if (s && s->fp) fflush(s->fp); }        /* [21] fflush */
static void      ov_flush_b(ov_stream *s)       { if (s && s->fp) fflush(s->fp); }        /* [22] fflush */
static long long ov_ret1(ov_stream *s)          { (void)s; return 1; }   /* [23] return 1 */

/* The stream vtable -- one 24-entry table shared by every stream we hand back (the methods are
 * stateless w.r.t. the object beyond `this`). Order MATCHES the OG PTR_FUN_18003d050 slot order
 * exactly (every slot decompiled in pb1-overrides), so the engine calls the right method per slot. */
static void *g_stream_vtable[24] = {
    (void *)ov_dtor,          /* 0  +0x00 close/dtor */
    (void *)ov_ret0_a,        /* 1  +0x08 */
    (void *)ov_ret0_b,        /* 2  +0x10 */
    (void *)ov_length,        /* 3  +0x18 Length */
    (void *)ov_name,          /* 4  +0x20 Name */
    (void *)ov_read,          /* 5  +0x28 Read */
    (void *)ov_write,         /* 6  +0x30 Write */
    (void *)ov_seekread,      /* 7  +0x38 */
    (void *)ov_seekread2,     /* 8  +0x40 */
    (void *)ov_lock,          /* 9  +0x48 Lock */
    (void *)ov_unlock,        /* 10 +0x50 Unlock */
    (void *)ov_length_byseek, /* 11 +0x58 */
    (void *)ov_noop,          /* 12 +0x60 (OG RET 0) */
    (void *)ov_tell,          /* 13 +0x68 Tell */
    (void *)ov_seek,          /* 14 +0x70 Seek */
    (void *)ov_printf_thunk,  /* 15 +0x78 vfprintf */
    (void *)ov_printf_thunk,  /* 16 +0x80 vfprintf */
    (void *)ov_ret0_c,        /* 17 +0x88 */
    (void *)ov_ret0_d,        /* 18 +0x90 */
    (void *)ov_ret0_e,        /* 19 +0x98 */
    (void *)ov_flag8,         /* 20 +0xa0 */
    (void *)ov_flush_a,       /* 21 +0xa8 Flush */
    (void *)ov_flush_b,       /* 22 +0xb0 Flush */
    (void *)ov_ret1,          /* 23 +0xb8 */
};

/* Construct a stream over an already-open FILE* + its known length. Allocates the object + a copy of
 * `name` after it (so the Name slot returns a stable pointer). NULL on alloc failure (caller fcloses). */
static ov_stream *make_stream(FILE *fp, long long length, const char *name)
{
    size_t namelen = name ? strlen(name) : 0;
    ov_stream *s = (ov_stream *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                          sizeof(ov_stream) + namelen + 1);
    if (!s) return NULL;
    char *namecopy = (char *)(s + 1);
    if (name) memcpy(namecopy, name, namelen);
    namecopy[namelen] = '\0';
    s->vtable = g_stream_vtable;
    s->fp     = fp;
    s->name   = namecopy;
    s->length = length;
    s->flag16 = 1;   /* OG sets the +0x20 short to 1 */
    s->flag8  = 0;
    return s;
}

/* Construct a MEMORY-backed stream over `buf`/`length`. owns_buf=1 hands the (heap) buffer to the
 * stream's dtor; owns_buf=0 leaves it (the static baked text). NULL on alloc failure. */
static ov_stream *make_mem_stream(const unsigned char *buf, long long length, const char *name, int owns_buf)
{
    ov_stream *s = make_stream(NULL, length, name);
    if (!s) return NULL;
    s->buf      = buf;
    s->pos      = 0;
    s->owns_buf = owns_buf;
    return s;
}

/* ====================================================== override-file path resolution ==============
 * OG FUN_18000b110 (DIRECT, XINPUT1_3.dll decompile + disasm 0xb13b..0xb1c8). The branch selector is a ".inc"-SUFFIX test, NOT a '/' test:
 *
 *   match = strstr(name, ".inc");                       // DAT_18003e2d0 = a strstr-style substring find
 *   if (match == NULL || *(match + 4) != '\0')          // ".inc" absent, or NOT at end of the string
 *       fmt = "overrides/%s";                            //   -> COMMON case (the vast majority)
 *   else {                                               // name's first ".inc" sits at its very end
 *       if (strchr(name,'/') != NULL &&
 *           strstr(name,"includes") != name)             //   "includes" is NOT a prefix of name
 *           rel = strchr(name,'/') + 1;                   //   strip up to & incl the first '/'
 *       fmt = "overrides/shader_includes/%s";            //   -> RARE case (shader-include .inc files)
 *   }
 *   sprintf(buf, fmt, rel);  // then prepend <root>\overrides... ; all '/' -> '\'
 *
 * So shader_includes is the EXCEPTION for ".inc"-suffixed shader-include names; overrides/<name> is the
 * common path for every normal resource (env/..., models/..., fonts/... -- none end in ".inc"). DAT_18003e2d0
 * is a runtime-resolved substring-find fn-ptr (null in the static image; proven strstr-semantics by its other
 * call site FUN_180026680: `find(name,"superscriptx64.dll") != 0 -> LoadLibrary`). The full path =
 * <root>\overrides\... with all '/' -> '\'. */

static void default_root(char *out, size_t cap)
{
    char profile[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, profile)))
        _snprintf_s(out, cap, _TRUNCATE, "%s\\snaphak", profile);
    else
        _snprintf_s(out, cap, _TRUNCATE, "snaphak");
}

static void resolve_root(char *out, size_t cap)
{
    if (g_root[0]) strncpy_s(out, cap, g_root, _TRUNCATE);
    else default_root(out, cap);
}

/* Build the on-disk override path for engine resource `name` into `out`. Returns 1 if a path was built
 * (always, for a non-empty name). The selection mirrors OG FUN_18000b110 EXACTLY (see the block comment
 * above): the shader_includes branch is the RARE exception for ".inc"-suffixed shader-include names; every
 * normal resource (no ".inc" suffix) takes the common overrides/<name> path. */
static int build_override_path(const char *name, char *out, size_t cap)
{
    if (!name || !name[0]) return 0;
    char root[MAX_PATH];
    resolve_root(root, sizeof root);

    /* OG's ".inc"-suffix test: find the first ".inc"; the shader branch is taken only when that match is
     * at the very end of the name (the byte after the 4-char ".inc" is the terminator) -- i.e. `match &&
     * match[4]=='\0'`. (Byte-faithful to OG's `*(strstr(name,".inc")+4) == 0`.) */
    const char *match = strstr(name, ".inc");
    int is_shader_include = (match != NULL && match[4] == '\0');

    const char *rel = name;
    if (is_shader_include) {
        /* OG: within the shader branch, strip up to & incl the first '/' UNLESS the name begins with
         * "includes" (OG: `strchr(name,'/') && strstr(name,"includes") != name -> rel = slash+1`). */
        const char *slash = strchr(name, '/');
        if (slash != NULL && strstr(name, "includes") != name)
            rel = slash + 1;
    }

    if (is_shader_include)
        _snprintf_s(out, cap, _TRUNCATE, "%s\\overrides\\shader_includes\\%s", root, rel);
    else
        _snprintf_s(out, cap, _TRUNCATE, "%s\\overrides\\%s", root, name);

    /* normalize '/' -> '\' (OG does the same on the assembled path). */
    for (char *p = out; *p; ++p)
        if (*p == '/') *p = '\\';
    return 1;
}

/* SEH-guarded open of the override file; returns an ov_stream* (caller returns it to the engine) or
 * NULL if no file / open failed. On success the FILE* is owned by the stream (its dtor fcloses). */
static ov_stream *try_open_override(const char *name)
{
    char path[MAX_PATH];
    if (!build_override_path(name, path, sizeof path)) return NULL;

    /* exists? (cheap negative for the common no-override case before fopen) */
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return NULL;

    FILE *fp = NULL;
    if (fopen_s(&fp, path, "rb") != 0 || fp == NULL) return NULL;

    /* size via seek-end/tell/restore (OG reads size with _ftelli64). */
    long long length = 0;
    if (_fseeki64(fp, 0, SEEK_END) == 0) {
        length = _ftelli64(fp);
        _fseeki64(fp, 0, SEEK_SET);
    }

    ov_stream *s = make_stream(fp, length, name);
    if (!s) { fclose(fp); return NULL; }
    return s;
}

/* ====================================================== built-in default lookup + validation =======*/

/* Path-tolerant name compare for the baked table: case-insensitive, '/' == '\\' (the engine asks with
 * forward slashes; be robust to either). */
static int ov_name_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
}

/* The baked default for `name`, or NULL if `name` is not a built-in. */
static const ov_baked_decl_t *find_baked(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0]; i++)
        if (ov_name_eq(name, g_ov_baked_decls[i].name)) return &g_ov_baked_decls[i];
    return NULL;
}

/* Minimal decl well-formedness: has content; braces balance (never negative, ends at 0) and quotes
 * pair up, counted OUTSIDE quotes and outside // and block comments (the decl grammar allows both).
 * This is a truncation/mangling tripwire, NOT a semantic validator -- a structurally sound decl with
 * bad values still serves (the user's folder is the user's). */
static int decl_well_formed(const unsigned char *buf, size_t len)
{
    long depth = 0;
    int  in_quote = 0, in_line_comment = 0, in_block_comment = 0, seen_brace = 0;
    for (size_t i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (in_line_comment)  { if (c == '\n') in_line_comment = 0;                       continue; }
        if (in_block_comment) { if (c == '*' && i + 1 < len && buf[i+1] == '/') { in_block_comment = 0; i++; } continue; }
        if (in_quote)         { if (c == '"' || c == '\n') in_quote = 0;                  continue; }
        if (c == '"')  { in_quote = 1; continue; }
        if (c == '/' && i + 1 < len && buf[i+1] == '/') { in_line_comment = 1;  i++; continue; }
        if (c == '/' && i + 1 < len && buf[i+1] == '*') { in_block_comment = 1; i++; continue; }
        if (c == '{') { depth++; seen_brace = 1; }
        else if (c == '}') { if (--depth < 0) return 0; }
    }
    return seen_brace && depth == 0 && !in_quote;
}

/* Read a whole file into a heap buffer (cap 8 MiB -- decls are KB-scale; a bigger file is served
 * unvalidated as a plain stream rather than slurped). NULL on absent/oversize/failure. */
#define OV_SLURP_CAP (8u * 1024u * 1024u)
static unsigned char *read_all_file(const char *path, long long *out_len)
{
    FILE *fp = NULL;
    if (fopen_s(&fp, path, "rb") != 0 || fp == NULL) return NULL;
    long long len = 0;
    if (_fseeki64(fp, 0, SEEK_END) == 0) { len = _ftelli64(fp); _fseeki64(fp, 0, SEEK_SET); }
    if (len < 0 || len > (long long)OV_SLURP_CAP) { fclose(fp); return NULL; }
    unsigned char *buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    if ((long long)got != len) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    buf[len] = 0;
    *out_len = len;
    return buf;
}

/* USER layer open for a BUILT-IN name: slurp + validate the user's file. Well-formed -> a memory
 * stream over the heap copy (stream owns it). Malformed -> refuse (log) and let the caller serve the
 * built-in default -- a garbled file at one of these names would take out the "*Custom" tab. A file
 * the slurp can't handle (oversize/alloc) is served unvalidated as a plain stream (benefit of doubt). */
static ov_stream *open_user_for_baked_name(const char *name, int *malformed)
{
    *malformed = 0;
    char path[MAX_PATH];
    if (!build_override_path(name, path, sizeof path)) return NULL;
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return NULL;

    long long len = 0;
    unsigned char *buf = read_all_file(path, &len);
    if (!buf) return try_open_override(name);            /* unusual size/alloc -> plain file stream */

    if (!decl_well_formed(buf, (size_t)len)) {
        HeapFree(GetProcessHeap(), 0, buf);
        *malformed = 1;
        return NULL;
    }
    ov_stream *s = make_mem_stream(buf, len, name, 1);
    if (!s) { HeapFree(GetProcessHeap(), 0, buf); return NULL; }
    return s;
}

/* The override-open hook -- our value in the engine's open vtable slot. Same ABI as the engine method.
 * mode>=2 (OG param_5>=2) is a recursion/no-shadow guard -> straight to the original. Otherwise resolve
 * three-layer: USER disk file -> BUILT-IN baked default (from memory) -> chain to the engine original.
 * The user layer is gated by the snaphak_user_overrides cvar (default 1; reads as 1 until the cvar is
 * registered, so early-boot opens behave normally). SEH-guarded so a shadow path fault degrades to a
 * vanilla open. */
static void *ov_open_hook(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode)
{
    if (g_orig_open == NULL) return NULL;   /* defensive: never happens once installed */

    if (mode < 2 && name != NULL) {
        ov_stream *s = NULL;
        const char *src = NULL;
        __try {
            const ov_baked_decl_t *baked = find_baked(name);
            int user_on = sh_cvar_value_int_reg(B2_CVAR_SNAPHAK_USER_OVERRIDES, 1);
            if (user_on) {
                if (baked) {
                    int malformed = 0;
                    s = open_user_for_baked_name(name, &malformed);
                    if (s) src = "user";
                    else if (malformed) src = "built-in (user file malformed, refused)";
                } else {
                    s = try_open_override(name);
                    if (s) src = "user";
                }
            }
            if (s == NULL && baked != NULL) {
                s = make_mem_stream((const unsigned char *)baked->text, (long long)baked->len, name, 0);
                if (s && src == NULL) src = user_on ? "built-in" : "built-in (user layer off)";
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s = NULL;   /* any fault in the shadow path -> fall through to the original open */
        }
        if (s != NULL) {
            unsigned long n = (unsigned long)InterlockedIncrement(&g_shadow_count);
            char line[MAX_PATH + 160];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "B1: overrides file-shadow FIRED [%s] for '%s' (%lld bytes) [#%lu]",
                        src ? src : "?", name, s->length, n);
            backend_log(line);
            return s;   /* the engine reads the override bytes through our idFile vtable */
        }
    }
    /* no override (or guard) -> the engine's normal open (OG masks the byte args to 0xff). */
    return g_orig_open(self, name, (unsigned char)(b1 & 0xff), (unsigned char)(b2 & 0xff), mode);
}

/* ============================================================ vtable-global LEA decode ==============
 * The engine resource-provider vtable is a .data global (can't be masked-byte sig-scanned). The ctor
 * ResProviderCtor (resolved by signature) starts with `... 48 8B D9 (MOV RBX,RCX) ; 48 8D 05 <disp32>
 * (LEA RAX,[rip+vtable]) ; 48 89 01 (MOV [RCX],RAX)`. We scan forward from the resolved entry for the
 * FIRST `48 8D 05` and decode its rip-relative disp to recover the vtable VA build-portably. (Same
 * decode the strids op uses for its table global.) */
#define LEA_SCAN_WINDOW 0x40

static int safe_read_n(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try { for (size_t i = 0; i < n; i++) dst[i] = src[i]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void *decode_vtable_global(const uint8_t *ctor_fn)
{
    uint8_t b[LEA_SCAN_WINDOW];
    if (!safe_read_n(ctor_fn, b, sizeof b)) return NULL;
    for (int i = 0; i + 7 <= LEA_SCAN_WINDOW; i++) {
        if (b[i] == 0x48 && b[i + 1] == 0x8D && b[i + 2] == 0x05) {     /* LEA RAX,[rip+disp32] */
            int32_t disp;
            memcpy(&disp, &b[i + 3], 4);
            const uint8_t *rip_next = ctor_fn + i + 7;
            return (void *)(rip_next + disp);
        }
    }
    return NULL;
}

/* ====================================================== install-time reclaim + audit ===============
 * RECLAIM: earlier releases wrote the built-in defaults to <root>\overrides\<name> if absent. Such a
 * file, byte-equal to the baked text with CR bytes ignored (some copies picked up CRLF endings), is
 * provably OURS-untouched -> delete it, so the in-memory built-in layer (which updates with every
 * release) serves instead. ANY difference -> the file is user-owned -> kept, and it keeps winning.
 * SEH-guarded; a reclaim failure just leaves the file shadowing (the old behavior). */
static int file_equals_baked_ignoring_cr(const unsigned char *fbuf, size_t flen,
                                         const char *baked, size_t blen)
{
    size_t fi = 0, bi = 0;
    while (fi < flen && (char)fbuf[fi] == '\r') fi++;    /* the baked text is LF-only */
    while (fi < flen && bi < blen) {
        if ((char)fbuf[fi] == '\r') { fi++; continue; }
        if ((char)fbuf[fi] != baked[bi]) return 0;
        fi++; bi++;
        while (fi < flen && (char)fbuf[fi] == '\r') fi++;
    }
    return fi == flen && bi == blen;
}

static void reclaim_baked_overrides(void)
{
    for (size_t i = 0; i < sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0]; i++) {
        __try {
            char path[MAX_PATH];
            if (!build_override_path(g_ov_baked_decls[i].name, path, sizeof path)) continue;
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;   /* nothing on disk */
            long long len = 0;
            unsigned char *buf = read_all_file(path, &len);
            if (!buf) continue;
            int ours = file_equals_baked_ignoring_cr(buf, (size_t)len,
                                                     g_ov_baked_decls[i].text, g_ov_baked_decls[i].len);
            HeapFree(GetProcessHeap(), 0, buf);
            char msg[MAX_PATH + 96];
            if (ours && DeleteFileA(path)) {
                _snprintf_s(msg, sizeof msg, _TRUNCATE,
                            "B1: reclaimed previously-written default '%s' (built-in serves from memory now)",
                            g_ov_baked_decls[i].name);
                backend_log(msg);
            } else if (!ours) {
                _snprintf_s(msg, sizeof msg, _TRUNCATE,
                            "B1: user-owned override kept at built-in name '%s' (it wins over the built-in)",
                            g_ov_baked_decls[i].name);
                backend_log(msg);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* leave the file; old behavior */ }
    }
}

/* AUDIT: enumerate the user's active override files into the log (count + names, bounded), flagging any
 * that fail the well-formedness tripwire -- so "what is shadowing what" is answerable from the log. */
#define OV_AUDIT_MAX_FILES 512
#define OV_AUDIT_MAX_NAMED 24
#define OV_AUDIT_MAX_DEPTH 8
static void audit_walk(const char *dir, const char *rel, int depth, int *count, int *named, int *warned)
{
    if (depth > OV_AUDIT_MAX_DEPTH || *count >= OV_AUDIT_MAX_FILES) return;
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char sub[MAX_PATH], subrel[MAX_PATH];
        _snprintf_s(sub,    sizeof sub,    _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        _snprintf_s(subrel, sizeof subrel, _TRUNCATE, "%s%s%s", rel, rel[0] ? "/" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            audit_walk(sub, subrel, depth + 1, count, named, warned);
        } else {
            (*count)++;
            int bad = 0;
            long long len = 0;
            unsigned char *buf = read_all_file(sub, &len);
            if (buf) { bad = !decl_well_formed(buf, (size_t)len); HeapFree(GetProcessHeap(), 0, buf); }
            if (bad) (*warned)++;
            if (*named < OV_AUDIT_MAX_NAMED || bad) {
                char msg[MAX_PATH + 96];
                _snprintf_s(msg, sizeof msg, _TRUNCATE, "B1:   override %s'%s'",
                            bad ? "STRUCTURALLY-SUSPECT (unbalanced braces/quotes) " : "", subrel);
                backend_log(msg);
                (*named)++;
            }
        }
        if (*count >= OV_AUDIT_MAX_FILES) break;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void audit_user_overrides(void)
{
    __try {
        char root[MAX_PATH], dir[MAX_PATH];
        resolve_root(root, sizeof root);
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\overrides", root);
        int count = 0, named = 0, warned = 0;
        audit_walk(dir, "", 0, &count, &named, &warned);
        char msg[MAX_PATH + 128];
        _snprintf_s(msg, sizeof msg, _TRUNCATE,
                    "B1: overrides audit -- %d user override file(s) active under %s%s%s "
                    "(disable the user layer with snaphak_user_overrides 0 to bisect)",
                    count, dir, warned ? ", " : "", warned ? "with structural warnings above" : "");
        backend_log(msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) { backend_log("B1: overrides audit skipped (fault)"); }
}

/* ============================================================ the install (slot swap) ==============*/

int sh_overrides_install(void *ctor_fn, int ctor_status_ok)
{
    char line[MAX_PATH + 128];

    if (ctor_fn == NULL) {
        backend_log("B1: overrides file-shadow SKIPPED -- ResProviderCtor not resolved");
        return 0;
    }
    if (!ctor_status_ok) {
        /* The ctor is only used to DECODE the vtable LEA; a hooked prologue would corrupt the decode.
         * Refuse on the hook-tolerant known_rva fallback (same conservative policy as the other ops). */
        backend_log("B1: overrides file-shadow SKIPPED -- ResProviderCtor via hook-tolerant fallback "
                    "(prologue may be hooked); not decoding the vtable LEA from a detoured prologue");
        return 0;
    }
    if (g_orig_open != NULL) {
        backend_log("B1: overrides file-shadow already installed");
        return 1;
    }

    void *vtable = decode_vtable_global((const uint8_t *)ctor_fn);
    if (vtable == NULL) {
        backend_log("B1: overrides file-shadow SKIPPED -- could not decode the vtable LEA "
                    "(ResProviderCtor layout shifted?)");
        return 0;
    }

    void **slot = (void **)((uint8_t *)vtable + OPEN_SLOT_OFFSET);

    /* Save the original open + overwrite the slot with our hook. The slot is .data (an 8-byte pointer),
     * so we VirtualProtect RW, store, restore -- NOT install_inline_hook (that patches code). */
    void *orig = NULL;
    if (!safe_read_n((const uint8_t *)slot, (uint8_t *)&orig, sizeof orig) || orig == NULL) {
        backend_log("B1: overrides file-shadow SKIPPED -- open vtable slot unreadable / null");
        return 0;
    }

    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
        backend_log("B1: overrides file-shadow FAIL -- VirtualProtect(slot) failed");
        return 0;
    }
    g_orig_open = (open_fn_t)orig;
    *slot = (void *)ov_open_hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    g_slot = slot;

    if (!g_root[0]) default_root(g_root, sizeof g_root);
    reclaim_baked_overrides();   /* delete OUR untouched previously-written defaults (memory layer serves now) */
    audit_user_overrides();      /* log what the user's folder actively shadows */
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: overrides file-shadow installed (vtable=%p slot+0x%x=%p, orig open=%p); root=%s\\overrides; "
        "built-in defaults: %u from memory",
        vtable, OPEN_SLOT_OFFSET, (void *)slot, orig, g_root,
        (unsigned)(sizeof g_ov_baked_decls / sizeof g_ov_baked_decls[0]));
    backend_log(line);
    return 1;
}

int sh_overrides_set_root(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        default_root(g_root, sizeof g_root);
        return 1;
    }
    strncpy_s(g_root, sizeof g_root, path, _TRUNCATE);
    return g_root[0] != '\0';
}

unsigned long sh_overrides_shadow_count(void)
{
    return (unsigned long)InterlockedCompareExchange(&g_shadow_count, 0, 0);
}

int sh_overrides_uninstall(void)
{
    if (g_slot == NULL || g_orig_open == NULL) return 0;
    DWORD old;
    if (VirtualProtect(g_slot, sizeof(void *), PAGE_READWRITE, &old)) {
        *g_slot = (void *)g_orig_open;
        VirtualProtect(g_slot, sizeof(void *), old, &old);
        FlushInstructionCache(GetCurrentProcess(), g_slot, sizeof(void *));
    }
    backend_log("B1: overrides file-shadow uninstalled (vtable slot restored)");
    g_slot = NULL;
    g_orig_open = NULL;
    return 1;
}
