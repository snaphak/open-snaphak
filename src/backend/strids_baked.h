/* strids_baked.h -- the BAKED #str_ string set, compiled into the backend DLL.
 *
 * WHY: the SnapHak override pack's decls reference custom #str_ strings (palette category names, display
 * names) that the engine resolves through its lang table. Historically those rows came ONLY from
 * %USERPROFILE%\snaphak\strings\strids.json -- a runtime file in the user's profile that is empty on a
 * fresh install and trivially missing, so even a correct override decl showed raw "#str_..." tokens (or
 * no category). Baking the canonical set INTO the DLL removes that fragility: sh_strids injects these
 * rows UNCONDITIONALLY at startup (the same engine append+resort path the user file uses). The user's
 * strids.json is still honored afterward as an OPTIONAL add/override layer for end-user extensibility.
 *
 * FORMAT: each entry is { id, text } where `id` is the #str_ id WITHOUT the leading "#str_" prefix
 * (inject_row prepends it, exactly as it does for the user-file keys). Keep this list to strings the
 * SHIPPED pack actually needs and that are NOT stock engine strings (to avoid duplicate-hash rows).
 *
 * Clean-room: our own strings; zero OG SnapHak bytes.
 */
#ifndef B1_STRIDS_BAKED_H
#define B1_STRIDS_BAKED_H

#include <stddef.h>

typedef struct { const char *id; const char *text; } strid_baked_t;

static const strid_baked_t g_strids_baked[] = {
    /* The "unknown" placeholder entity's own palette home. The override pack ships
     * snapeditorentitydef/unknown/unknown.decl referencing these; baking them gives it a clean "Unknown"
     * tab on every install instead of a raw token. */
    { "snapentity_metacategory_unknownroot",      "Unknown" },
    { "snapentity_metacategory_unknownroot_disp", "Unknown" },
    { "snapentity_category_unknownroot",          "Unknown" },

    /* The built-in "*Custom" palette tab + its shipped entities (Timeline + Unknown). Baked so a clean setup
     * shows the tab + names with no external strings file; overrides_seed_baked.h ships the matching decls. */
    { "sh_category_1",    "*Custom" },
    { "sh_timeline",      "Timeline" },
    { "sh_timeline_desc", "A timeline that sequences entity events over time. Place it, then open it in the Timeline Editor to author events.^7 Class: ^OidTarget_Timeline" },
    { "sh_unknown",       "Unknown" },
    { "sh_unknown_desc",  "An unknown entity not normally available in the palette." },

    /* (extend here as the shipped pack / UI grows -- one row per custom #str_ id.) */
};

#define B1_STRIDS_BAKED_COUNT (sizeof(g_strids_baked) / sizeof(g_strids_baked[0]))

#endif /* B1_STRIDS_BAKED_H */
