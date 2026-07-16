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

/* (The slot to swap is located by find_slot_holding, below -- not by a constant. See its comment.) */

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

/* The engine's seek-origin enum, as its own Seek implementation defines it (see ov_seek). It is NOT the
 * CRT's: the engine passes these values, we translate to SEEK_CUR/SEEK_END/SEEK_SET. */
#define OV_SEEK_CUR 0
#define OV_SEEK_END 1
#define OV_SEEK_ABS 2

/* --- the vtable methods, faithful to the engine's slot semantics ------------------------------------
 * All __fastcall(this in RCX), expressed in stdio over the FILE* the object carries. */

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
/* [7] seek-then-read. The engine's own base implementation of this slot is exactly
 *       Seek(this, offset, SEEK_ABS); return Read(this, buf, len);
 *     (its disassembly loads the origin literal 2 into the 3rd argument, calls the Seek slot, then
 *     TAIL-CALLS the Read slot), so we reproduce that combo through our own methods. */
static long long ov_seekread(ov_stream *s, long long off, void *buf, unsigned int n)
{
    if (!s) return 0;
    ov_seek(s, off, OV_SEEK_ABS);
    return ov_read(s, buf, n);
}
/* [8] seek-then-WRITE. This slot's engine base implementation is byte-for-byte the slot-7 helper except
 *     for its final tail-call, which targets the WRITE slot, not the Read slot -- so it is
 *       Seek(this, offset, SEEK_ABS); return Write(this, buf, len);
 *     (It was previously reproduced here as a second seek-then-READ, which no engine implementation of
 *     this slot does.) The engine's resource-READ path never reaches it; a write path now stays correct. */
static long long ov_seekwrite(ov_stream *s, long long off, const void *buf, unsigned int n)
{
    if (!s) return 0;
    ov_seek(s, off, OV_SEEK_ABS);
    return ov_write(s, buf, n);
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
/* [14] Seek. The engine passes ITS origin enum, which is NOT the CRT's: its own Seek implementation
 * indexes a 3-entry table by the origin argument and hands the result to fseek --
 *     table[0]=SEEK_CUR(1)  table[1]=SEEK_END(2)  table[2]=SEEK_SET(0)
 * -- and fatal-errors on anything above 2. So the engine's enum is { CUR=0, END=1, ABS=2 }; see the
 * OV_SEEK_* constants. This mapping previously had CUR and ABS swapped, which was harmless only by
 * accident: the engine's seek-then-read helper always passes (offset=0, ABS), and seeking 0 RELATIVE from
 * position 0 lands in the same place as seeking to 0 ABSOLUTE. The chunked read (which seeks to 0, 64K,
 * 128K...) is the first caller that would have turned those relative hops into wrong offsets. */
static int       ov_seek(ov_stream *s, long long off, int origin)
{
    if (!s || !s->fp) return -1;
    int o;
    if (origin == OV_SEEK_CUR)      o = SEEK_CUR;
    else if (origin == OV_SEEK_END) o = SEEK_END;
    else if (origin == OV_SEEK_ABS) o = SEEK_SET;
    else return -1;                 /* the engine fatal-errors here; we just refuse the seek */
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

/* --- the two virtuals the CURRENT engine build added to idFile (absent from the older interface) -------
 * Both are a single shared base implementation that no idFile subclass overrides (verified across all 12
 * of them), so reproducing the base behaviour is exactly right -- there is no per-class variant to miss. */

/* return false. (Engine base: `xor al,al ; ret`.) */
static char ov_ret_false(ov_stream *s)          { (void)s; return 0; }

/* A 64KB chunked SCATTER read: fill an array of 64KB buffers from `base_off`, returning the total read.
 * Faithful to the engine's own base implementation, which loops
 *     total += seekread(this, base_off + off, bufs[i], 0x10000)
 * advancing off by 0x10000 per iteration while off < total_len, and returns the accumulated count. The
 * length-0 guard and the advance-then-compare ordering match the engine's (it is a do/while). This is the
 * method a >64KB resource read goes through -- our built-in settings decl is 70,016 bytes, so this path is
 * live on any normal boot, not an exotic one. */
static long long ov_scatter_read(ov_stream *s, long long base_off, void **bufs, long long total_len)
{
    if (!s || !bufs || total_len <= 0) return 0;
    long long got = 0, off = 0;
    size_t i = 0;
    do {
        got += ov_seekread(s, base_off + off, bufs[i], 0x10000);
        off += 0x10000;
        i++;
    } while (off < total_len);
    return got;
}

/* --- the stream vtables ----------------------------------------------------------------------------
 * The engine calls our stream through ITS idFile slot order, and that order is per-build data: the current
 * build inserted two virtuals into the interface (at index 3 and index 10), shifting everything after them.
 * Getting this wrong does not crash -- it silently runs the wrong method, which is how a mis-ordered table
 * turned a 70KB resource read into a permanent 100%-CPU spin.
 *
 * So we keep one table per known interface shape and CHOOSE by measuring the engine (see
 * engine_idfile_slots): we never assume which build we are in. A shape we do not recognise is refused
 * outright rather than guessed at -- see sh_overrides_install.
 *
 * The methods are stateless w.r.t. the object beyond `this`, so one table per shape is shared by every
 * stream we hand back. */

/* 31-slot idFile: the original interface. */
static void *g_stream_vtable_v31[24] = {
    (void *)ov_dtor,          /*  0 close/dtor */
    (void *)ov_ret0_a,        /*  1 */
    (void *)ov_ret0_b,        /*  2 */
    (void *)ov_length,        /*  3 Length */
    (void *)ov_name,          /*  4 Name */
    (void *)ov_read,          /*  5 Read */
    (void *)ov_write,         /*  6 Write */
    (void *)ov_seekread,      /*  7 seek+read */
    (void *)ov_seekwrite,     /*  8 seek+write */
    (void *)ov_lock,          /*  9 Lock */
    (void *)ov_unlock,        /* 10 Unlock */
    (void *)ov_length_byseek, /* 11 */
    (void *)ov_noop,          /* 12 */
    (void *)ov_tell,          /* 13 Tell */
    (void *)ov_seek,          /* 14 Seek */
    (void *)ov_printf_thunk,  /* 15 vfprintf */
    (void *)ov_printf_thunk,  /* 16 vfprintf */
    (void *)ov_ret0_c,        /* 17 */
    (void *)ov_ret0_d,        /* 18 */
    (void *)ov_ret0_e,        /* 19 */
    (void *)ov_flag8,         /* 20 */
    (void *)ov_flush_a,       /* 21 Flush */
    (void *)ov_flush_b,       /* 22 Flush */
    (void *)ov_ret1,          /* 23 */
};

/* 34-slot idFile: the current interface. Identical methods, re-ordered around the two inserted virtuals
 * (marked NEW below). Each entry's comment gives the index it held in the 31-slot shape. */
static void *g_stream_vtable_v34[26] = {
    (void *)ov_dtor,          /*  0 close/dtor        (was  0) */
    (void *)ov_ret0_a,        /*  1                   (was  1) */
    (void *)ov_ret0_b,        /*  2                   (was  2) */
    (void *)ov_ret_false,     /*  3 NEW: return false            */
    (void *)ov_length,        /*  4 Length            (was  3) */
    (void *)ov_name,          /*  5 Name              (was  4) */
    (void *)ov_read,          /*  6 Read              (was  5) */
    (void *)ov_write,         /*  7 Write             (was  6) */
    (void *)ov_seekread,      /*  8 seek+read         (was  7) */
    (void *)ov_seekwrite,     /*  9 seek+write        (was  8) */
    (void *)ov_scatter_read,  /* 10 NEW: 64KB chunked scatter read */
    (void *)ov_lock,          /* 11 Lock              (was  9) */
    (void *)ov_unlock,        /* 12 Unlock            (was 10) */
    (void *)ov_length_byseek, /* 13                   (was 11) */
    (void *)ov_noop,          /* 14                   (was 12) */
    (void *)ov_tell,          /* 15 Tell              (was 13) */
    (void *)ov_seek,          /* 16 Seek              (was 14) */
    (void *)ov_printf_thunk,  /* 17 vfprintf          (was 15) */
    (void *)ov_printf_thunk,  /* 18 vfprintf          (was 16) */
    (void *)ov_ret0_c,        /* 19                   (was 17) */
    (void *)ov_ret0_d,        /* 20                   (was 18) */
    (void *)ov_ret0_e,        /* 21                   (was 19) */
    (void *)ov_flag8,         /* 22                   (was 20) */
    (void *)ov_flush_a,       /* 23 Flush             (was 21) */
    (void *)ov_flush_b,       /* 24 Flush             (was 22) */
    (void *)ov_ret1,          /* 25                   (was 23) */
};

/* Chosen at install by measuring the engine; NULL until then (and left NULL if the shape is unknown, which
 * keeps the shadow uninstalled rather than serving streams the engine would call wrongly). */
static void **g_stream_vtable = NULL;

/* Construct a stream over an already-open FILE* + its known length. Allocates the object + a copy of
 * `name` after it (so the Name slot returns a stable pointer). NULL on alloc failure (caller fcloses). */
static ov_stream *make_stream(FILE *fp, long long length, const char *name)
{
    /* No confirmed slot order -> no stream. Unreachable in practice (install refuses first), but the cost of
     * being wrong here is a silent wrong-method call, so it is worth stating rather than assuming. */
    if (g_stream_vtable == NULL) return NULL;
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

/* ====================================================== locating the slot to swap ==================
 * We need ONE thing: the vtable slot holding the engine's open-by-name. Everything else is a means to
 * that end, and the means used to be fragile in three separate ways at once:
 *
 *   resolve the ctor by signature -> decode its `LEA RAX,[rip+vtable]` -> add a fixed slot index (+0xf8)
 *
 * All three broke. The ctor was RECOMPILED on the current DOOM build, so no byte pattern can find it and
 * the whole install bailed at step one -- which also skipped the built-in decl seeding below, so the
 * "*Custom" palette tab never appeared. And the fixed index was wrong anyway: the class gained 10 virtuals
 * and open-by-name moved to +0x148, so had step one succeeded, step three would have swapped the WRONG
 * method -- silently, since that neither faults nor logs.
 *
 * So we do none of it. The open-by-name METHOD is byte-identical across builds and resolves by signature;
 * a vtable is just a data slot holding a method's address; and that method's address appears in exactly
 * ONE 8-aligned .rdata slot (verified on both builds -- old 0x2798598, new 0x1FF4DE8, i.e. the same slot
 * the old chain computed, arrived at without computing anything). So: scan .rdata for the resolved
 * method, and the slot that holds it is the slot to patch.
 *
 * No ctor. No LEA decode. No slot index. Nothing in the path that a rebuild can shift, and a class that
 * reorders its virtuals again needs no change here. If the method is found in 0 or >1 slots we refuse:
 * an ambiguous vtable is not something to guess at. */
#define OV_SECTION_NAME_RDATA ".rdata"

static int safe_read_n(const uint8_t *src, uint8_t *dst, size_t n)
{
    __try { for (size_t i = 0; i < n; i++) dst[i] = src[i]; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* The live mapped image's section span by name. The PE headers + section table are present in memory, so
 * this needs no file access and is correct under ASLR (every address derives from the real module base). */
static int section_span(const uint8_t *module_base, const char *want,
                        const uint8_t **out_start, size_t *out_size)
{
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            char nm[9] = { 0 };
            memcpy(nm, sec->Name, 8);
            if (strcmp(nm, want) != 0) continue;
            *out_start = module_base + sec->VirtualAddress;
            *out_size  = sec->Misc.VirtualSize;
            return 1;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

/* ============================================ measuring the engine's idFile interface ==============
 * Our stream is handed to the engine as an idFile, so our vtable's slot ORDER has to be the engine's. That
 * order is per-build data: the current build inserted two virtuals into the interface (at index 3 and index
 * 10), shifting every method after them. Getting it wrong does not crash -- the engine simply calls the
 * wrong method, silently, which is how the old layout turned a >64KB resource read into a permanent spin.
 *
 * So MEASURE the interface instead of assuming a build. MSVC records every polymorphic class's identity as
 * a mangled NAME STRING in its RTTI, and a name survives a recompile even when every byte of every method
 * changes -- which is exactly what happened here. Walk the chain forward:
 *
 *     ".?AVidFile@@"  ->  TypeDescriptor            (the name sits at TD+0x10)
 *                     ->  CompleteObjectLocator      (its +0x0C is the TD's RVA)
 *                     ->  the vtable                 (its [-8] holds the locator's address)
 *                     ->  count the code slots       = the interface's shape
 *
 * The trailing NUL makes the name match exact, so ".?AVidFileSystem@@" cannot alias ".?AVidFile@@". Both
 * the name and the resulting vtable are unique in the image (verified on both builds).
 *
 * Returns the slot count, or -1 if the chain cannot be walked (caller then declines to install rather than
 * serve streams through a layout it could not confirm). */
#define OV_IDFILE_CLASS ".?AVidFile@@"

static int vtable_code_slots(const uint8_t *vt, const uint8_t *text, size_t text_n)
{
    int n = 0;
    for (; n < 512; n++) {
        const uint8_t *p = NULL;
        if (!safe_read_n(vt + (size_t)n * sizeof p, (uint8_t *)&p, sizeof p)) break;
        if (p < text || p >= text + text_n) break;   /* not a method -> the next class's RTTI locator */
    }
    return n;
}

static int engine_idfile_slots(const uint8_t *module_base)
{
    const uint8_t *text = NULL, *rdata = NULL, *dsec = NULL;
    size_t text_n = 0, rdata_n = 0, dsec_n = 0;
    if (!section_span(module_base, ".text", &text, &text_n)) return -1;
    if (!section_span(module_base, OV_SECTION_NAME_RDATA, &rdata, &rdata_n)) return -1;
    if (!section_span(module_base, ".data", &dsec, &dsec_n)) return -1;

    __try {
        /* 1. the mangled name (TypeDescriptors live in .data) -> the TypeDescriptor at name-0x10. */
        static const char cls[] = OV_IDFILE_CLASS;
        const size_t clen = sizeof cls;             /* includes the NUL -> exact, non-prefix match */
        const uint8_t *name_at = NULL;
        for (size_t i = 0; i + clen <= dsec_n; i++) {
            const uint8_t *q = (const uint8_t *)memchr(dsec + i, cls[0], dsec_n - i - clen + 1);
            if (!q) break;
            if (memcmp(q, cls, clen) == 0) { name_at = q; break; }
            i = (size_t)(q - dsec);                 /* resume just past this candidate */
        }
        if (!name_at || (size_t)(name_at - module_base) < 0x10) return -1;
        const uint32_t td_rva = (uint32_t)((name_at - 0x10) - module_base);

        /* 2. the locator pointing at that TypeDescriptor (COL+0x0C holds the TD's RVA).
         *
         * A TypeDescriptor's RVA appears in .rdata in more than one kind of RTTI structure, so "some DWORD
         * equals it" is NOT enough to have found the locator -- treating every such DWORD as a locator
         * finds two candidates for this class and makes the chain look ambiguous when it is not. Identify
         * the real one positively, by the two things only a 64-bit locator satisfies:
         *   signature == 1  (x64; 0 is the 32-bit form, which this image cannot contain), and
         *   +0x14 == the locator's OWN rva  (the x64 self-reference).
         * That is unique in both builds; the impostor has signature 0 and a zero self-reference. */
        const uint8_t *col = NULL;
        for (size_t off = 0x0C; off + sizeof(uint32_t) <= rdata_n; off += 4) {
            uint32_t v = 0;
            if (!safe_read_n(rdata + off, (uint8_t *)&v, sizeof v)) continue;
            if (v != td_rva) continue;
            const uint8_t *c = rdata + off - 0x0C;
            uint32_t sig = 0, pself = 0;
            if (!safe_read_n(c, (uint8_t *)&sig, sizeof sig)) continue;
            if (sig != 1) continue;                             /* x64 locators only */
            if (!safe_read_n(c + 0x14, (uint8_t *)&pself, sizeof pself)) continue;
            if (pself != (uint32_t)(c - module_base)) continue;  /* the x64 self-reference */
            if (col) return -1;                     /* genuinely ambiguous -> refuse rather than pick one */
            col = c;
        }
        if (!col) return -1;

        /* 3. the vtable whose [-8] holds that locator's address. Unique, like the locator: an ambiguous
         *    chain is not something to pick a winner from. */
        const uint8_t *vt = NULL;
        for (size_t s = 0; s + sizeof(void *) <= rdata_n; s += sizeof(void *)) {
            const uint8_t *e = NULL;
            if (!safe_read_n(rdata + s, (uint8_t *)&e, sizeof e)) continue;
            if (e != col) continue;
            if (vt) return -1;                      /* >1 vtable for this class -> refuse */
            vt = rdata + s + sizeof(void *);
        }
        if (!vt) return -1;
        int n = vtable_code_slots(vt, text, text_n);
        return n > 0 ? n : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

/* The unique 8-aligned .rdata slot holding `method`, or NULL if absent/ambiguous. Reads the live mapped
 * image (the PE headers + section table are present in memory), SEH-guarded across uncommitted tails. */
static void **find_slot_holding(const uint8_t *module_base, void *method)
{
    if (!module_base || !method) return NULL;
    __try {
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module_base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(module_base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

        const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
        void **found = NULL;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            char nm[9] = { 0 };
            memcpy(nm, sec->Name, 8);
            if (strcmp(nm, OV_SECTION_NAME_RDATA) != 0) continue;   /* MSVC puts vtables in .rdata */

            const uint8_t *base = module_base + sec->VirtualAddress;
            size_t span = sec->Misc.VirtualSize;
            for (size_t off = 0; off + sizeof(void *) <= span; off += sizeof(void *)) {
                void *entry = NULL;
                if (!safe_read_n(base + off, (uint8_t *)&entry, sizeof entry)) continue;  /* torn page */
                if (entry != method) continue;
                if (found) return NULL;             /* ambiguous -> refuse rather than pick one */
                found = (void **)(base + off);
            }
        }
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
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

int sh_overrides_install(const uint8_t *module_base, void *open_fn)
{
    char line[MAX_PATH + 128];

    if (open_fn == NULL) {
        backend_log("B1: overrides file-shadow SKIPPED -- FileSystemOpenByName not resolved; without it "
                    "we cannot identify the vtable slot to swap, and we do not assume one");
        return 0;
    }
    if (g_orig_open != NULL) {
        backend_log("B1: overrides file-shadow already installed");
        return 1;
    }

    /* Which idFile interface is this engine? Our stream is called back through the engine's slot order, so
     * a layout we cannot confirm means we must not hand out streams at all -- serving one through a guessed
     * order does not fault, it silently runs the wrong method and hangs the game. Refuse instead. */
    int slots = engine_idfile_slots(module_base);
    if (slots == 24 + 7) {           /* the original interface: our 24 methods + 7 slots the engine never calls */
        g_stream_vtable = g_stream_vtable_v31;
    } else if (slots == 26 + 8) {    /* the current interface: two virtuals inserted (index 3 and index 10) */
        g_stream_vtable = g_stream_vtable_v34;
    } else {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B1: overrides file-shadow SKIPPED -- the engine's idFile interface has %d virtual(s), "
                    "which is neither layout we know (31 or 34). Our stream would be called through an "
                    "unverified slot order, so we do not install: no overrides and no \"*Custom\" tab, but "
                    "the game runs. Re-derive the slot order for this build.", slots);
        backend_log(line);
        return 0;
    }

    /* The one lookup: the unique .rdata slot holding the resolved method. See find_slot_holding. */
    void **slot = find_slot_holding(module_base, open_fn);
    if (slot == NULL) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B1: overrides file-shadow SKIPPED -- the resolved open-by-name (%p) was not found in "
                    "exactly one .rdata slot (absent or ambiguous); refusing to guess", open_fn);
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
    /* Log the slot we FOUND. Worth seeing: it moved between DOOM builds (the class gained 10 virtuals), and
     * this line is the evidence that we located it rather than assumed it. */
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "B1: overrides file-shadow installed (open-by-name %p found in slot %p [rva 0x%llX], orig open=%p); "
        "engine idFile has %d virtuals -> stream slot order v%d; root=%s\\overrides",
        open_fn, (void *)slot, (unsigned long long)((const uint8_t *)slot - module_base), orig,
        slots, slots, g_root);
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
