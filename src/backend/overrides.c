/* overrides.c -- see overrides.h. The OVERRIDES FILE-SHADOW resource loader.
 *
 * Swaps the engine resource-provider's open-by-name vtable slot (+0xf8) with our override-open hook.
 * On each engine open, we first test for overrides/<name> under %USERPROFILE%\snaphak\; if the file
 * exists we return our own idFile-subclass stream (a FILE*-backed reimplementation of OG's
 * PTR_FUN_18003d050 stream), else we chain to the saved original engine open. A mode>=2 recursion
 * guard goes straight to the original (OG's `param_5 >= 2` branch).
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
#include "overrides_seed_baked.h"   /* the built-in "*Custom"-tab default decls (Timeline + Unknown) */

/* The open-by-name method's slot within the resource-provider vtable is NOT a constant.
 *
 * It was 0xf8 on the pre-April-2024 build (the original patches engineBase+0x2798598 against a vtable at
 * engineBase+0x27984a0 -> 0xf8). The current build inserted 10 virtuals ahead of it and the same method now
 * sits at 0x148. A fixed index is therefore not merely stale, it is SILENTLY WRONG: swapping it replaces
 * some other virtual, which does not crash -- overrides just hooks the wrong method and file-shadowing
 * quietly does nothing (or worse).
 *
 * So we do not carry a slot index at all. The METHOD is byte-identical across both builds and resolves by
 * signature ("FileSystemOpenByName"), so we scan the vtable for the resolved address and patch whichever
 * slot holds it. That is build-agnostic by construction -- a future DOOM that reorders these virtuals again
 * needs no change here. SLOT_SEARCH_MAX bounds the scan; the vtable is ~53 entries on the current build. */
#define SLOT_SEARCH_MAX 0x400

/* Find the vtable slot holding `method`. Returns the slot address, or NULL if the method is not in this
 * vtable (which would mean the resolved method and the resolved vtable disagree -- refuse, do not guess). */
static void **find_vtable_slot(void *vtable, void *method)
{
    for (size_t off = 0; off < SLOT_SEARCH_MAX; off += sizeof(void *)) {
        void *entry = NULL;
        if (!safe_read_n((const uint8_t *)vtable + off, (uint8_t *)&entry, sizeof entry)) return NULL;
        if (entry == method) return (void **)((uint8_t *)vtable + off);
    }
    return NULL;
}

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
 * A FILE*-backed reimplementation of OG's PTR_FUN_18003d050 stream (the engine idFile interface, 24
 * virtual methods -- every slot decompiled in pb1-overrides). The object layout mirrors OG's:
 *   +0x00 vtable   +0x08 FILE*   +0x10 name   +0x18 length   +0x20 short flag   +0x21 byte flag
 * The engine reads the resource through this vtable; the dtor (slot 0) frees the object with OUR
 * allocator (HeapFree) -- so we need no engine allocator/free (OG used the engine's only so the engine
 * could free it; here every method incl. the dtor is ours). */
typedef struct ov_stream {
    void        *vtable;     /* +0x00 */
    FILE        *fp;         /* +0x08 */
    const char  *name;       /* +0x10 (points at the heap-dup'd name appended after the struct) */
    long long    length;     /* +0x18 */
    short        flag16;     /* +0x20 (OG sets 1) */
    char         flag8;      /* +0x22-ish via +0x21 read in slot 20; OG returns *(this+0x21) */
} ov_stream;

/* --- the 24 vtable methods, faithful to the OG slot semantics (every slot decompiled) --------------
 * All __fastcall(this in RCX). Behaviour matches OG FUN_18000ae00..b070 exactly, expressed in stdio. */

static void  ov_dtor(ov_stream *s)                                   /* [0] close + free(this) */
{
    if (s) {
        if (s->fp) { fclose(s->fp); s->fp = NULL; s->length = 0; s->name = NULL; s->flag16 = 0; }
        HeapFree(GetProcessHeap(), 0, s);
    }
}
static long long ov_ret0_a(ov_stream *s)        { (void)s; return 0; }   /* [1] return 0 */
static long long ov_ret0_b(ov_stream *s)        { (void)s; return 0; }   /* [2] return 0 */
static long long ov_length(ov_stream *s)        { return s ? s->length : 0; }            /* [3] *(this+0x18) */
static const char *ov_name(ov_stream *s)        { return s ? s->name : NULL; }           /* [4] *(this+0x10) */
static long long ov_read(ov_stream *s, void *buf, unsigned int n)    /* [5] fread(buf,1,n,fp) */
{
    if (!s || !s->fp || !buf) return 0;
    return (long long)fread(buf, 1, n, s->fp);
}
static long long ov_write(ov_stream *s, const void *buf, unsigned int n)  /* [6] fwrite(buf,1,n,fp) */
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
    if (!s || !s->fp) return 0;
    long long pos = _ftelli64(s->fp);
    _fseeki64(s->fp, 0, SEEK_END);
    long long len = _ftelli64(s->fp);
    _fseeki64(s->fp, pos, SEEK_SET);
    return len;
}
static void      ov_noop(ov_stream *s)          { (void)s; }                                          /* [12] RET 0 */
static long long ov_tell(ov_stream *s)          { return (s && s->fp) ? _ftelli64(s->fp) : 0; }       /* [13] ftell */
static int       ov_seek(ov_stream *s, long long off, int origin)    /* [14] fseek; OG maps 0->SET,1->END(2),else CUR */
{
    if (!s || !s->fp) return -1;
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

/* The override-open hook -- our value in the engine's open vtable slot. Same ABI as the engine method.
 * mode>=2 (OG param_5>=2) is a recursion/no-shadow guard -> straight to the original. Otherwise try the
 * override file; serve our stream if present, else chain to the saved engine original. SEH-guarded so a
 * shadow path fault degrades to a vanilla open. */
static void *ov_open_hook(void *self, const char *name, unsigned char b1, unsigned char b2, unsigned int mode)
{
    if (g_orig_open == NULL) return NULL;   /* defensive: never happens once installed */

    if (mode < 2 && name != NULL) {
        ov_stream *s = NULL;
        __try {
            s = try_open_override(name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            s = NULL;   /* any fault in the shadow path -> fall through to the original open */
        }
        if (s != NULL) {
            unsigned long n = (unsigned long)InterlockedIncrement(&g_shadow_count);
            char line[MAX_PATH + 96];
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "B1: overrides file-shadow FIRED for '%s' (%lld bytes) [#%lu]",
                        name, s->length, n);
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

/* ====================================================== built-in default-decl self-seed =============
 * Ship the clone's "*Custom" palette-tab default set (the Timeline + Unknown editor-entity/entityDef
 * overrides, g_ov_seed_decls) built-in: on install, write each to <root>\overrides\<name> IF ABSENT, so a
 * clean setup gets the tab + entities with no external files. WRITE-IF-ABSENT -> a user's own file at the
 * same path is never clobbered, and the open-shadow above then serves whichever is on disk. Runs before the
 * engine's decl preload (the hook is installed first), so a freshly-seeded decl is picked up this boot.
 * SEH-guarded; a seed failure just degrades to "no built-in default for that name", never a crash. */
static void seed_baked_overrides(void)
{
    for (size_t i = 0; i < sizeof g_ov_seed_decls / sizeof g_ov_seed_decls[0]; i++) {
        char path[MAX_PATH];
        if (!build_override_path(g_ov_seed_decls[i].name, path, sizeof path)) continue;
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) continue;   /* present -> keep the user's */
        __try {
            char dir[MAX_PATH];
            strncpy_s(dir, sizeof dir, path, _TRUNCATE);
            char *slash = strrchr(dir, '\\');
            if (slash) { *slash = '\0'; SHCreateDirectoryExA(NULL, dir, NULL); }
            FILE *fp = NULL;
            if (fopen_s(&fp, path, "wb") == 0 && fp) {
                fwrite(g_ov_seed_decls[i].text, 1, g_ov_seed_decls[i].len, fp);
                fclose(fp);
                char msg[MAX_PATH + 64];
                _snprintf_s(msg, sizeof msg, _TRUNCATE, "B1: seeded built-in override '%s'", g_ov_seed_decls[i].name);
                backend_log(msg);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* skip this one */ }
    }
}

/* ============================================================ the install (slot swap) ==============*/

int sh_overrides_install(void *ctor_fn, int ctor_status_ok, void *open_fn)
{
    char line[MAX_PATH + 128];

    if (ctor_fn == NULL) {
        backend_log("B1: overrides file-shadow SKIPPED -- ResProviderCtor not resolved");
        return 0;
    }
    if (open_fn == NULL) {
        backend_log("B1: overrides file-shadow SKIPPED -- FileSystemOpenByName not resolved "
                    "(needed to locate its vtable slot; we do not assume a slot index)");
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

    /* Locate the slot by finding the RESOLVED method inside the vtable -- never a fixed index (see the
     * SLOT_SEARCH_MAX comment). If the method is not in this vtable, the two resolutions disagree and we
     * refuse rather than patch something we cannot identify. */
    void **slot = find_vtable_slot(vtable, open_fn);
    if (slot == NULL) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B1: overrides file-shadow SKIPPED -- the resolved open-by-name (%p) is not in the "
                    "resolved vtable (%p); refusing to patch an unidentified slot", open_fn, vtable);
        backend_log(line);
        return 0;
    }

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
    seed_baked_overrides();   /* materialize the built-in "*Custom"-tab default decls if absent (Timeline + Unknown) */
    /* Log the slot we FOUND (not one we assumed) -- it differs per DOOM build, so it is worth seeing. */
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: overrides file-shadow installed (vtable=%p, open-by-name found at slot+0x%x=%p, orig open=%p); "
        "root=%s\\overrides",
        vtable, (unsigned)((uint8_t *)slot - (uint8_t *)vtable), (void *)slot, orig, g_root);
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
