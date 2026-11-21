#pragma once

// Table-driven localisation for every language the game can be set to (port of
// tools/ext/loc.c). English is the source language: an English string is looked
// up in the active language's generated table and translated; anything the table
// does not know passes through unchanged, so the game's own already-localised
// texts are never touched.
//
// A language is data, not a two-value enum: add a generated `loc_table_<code>.hpp`
// (English -> translated strings, plus the text-lookup probe ids) and one row in
// the registry in locale.cpp to support another language -- no call site changes.
// The Polish table is already ported; other game languages currently ship no
// translation table, so they resolve to English (the ext behaves the same way).
//
// Language selection: T2R_LANG=<code> or a one-line t2r-lang.txt next to the exe
// ("pl" / "en" / "auto" / any registered code), re-read every few seconds so an
// edit takes effect without a restart. "auto" probes the engine's own text lookup
// with the per-language probe ids (that native call is not wired in mod/ yet, so
// auto currently resolves to English -- an honest gate, see locale.cpp).

#include <cstdint>

namespace engine::locale {

    // One generated-table row: an English source string and its translation.
    struct LocEntry { const char* en; const char* tr; };

    // A text-lookup id whose English and translated wording differ, used by
    // auto-detection to identify which language table the engine has loaded.
    struct LocProbe { uint64_t sid; const char* en; const char* tr; };

    // A registered language. `en` (index 0) is the implicit source language and
    // has no table. Every other entry carries its generated table + probes.
    struct Language {
        const char*       code;
        const LocEntry*   entries;
        int               count;
        const LocProbe*   probes;
        int               probeCount;
    };

    // ---- language identity ---------------------------------------------------
    int         CurrentIndex();       // index into the registry (0 = English)
    const char* Code();               // active code ("en" when no table is active)
    bool        IsEnglish();
    bool        IsPolish();           // convenience for the 0/1 protocol byte
    int         LanguageCount();
    const char* LanguageCode(int index);

    // Force a language by code ("" / "auto" -> auto-detect); overrides env/file
    // until ClearOverride()/Reset(). Invalid codes fall back to English.
    void        SetOverride(const char* code);
    void        ClearOverride();

    // Re-read env/file and rebuild the lookup tables on the next call (test /
    // level-reload helper).
    void        Reset();

    // ---- translation ---------------------------------------------------------
    // Translate(): the active language's text for the English string `s`, or `s`
    // unchanged. The result lives in a small ring buffer (valid for the next 16
    // calls) -- copy it if you need to keep it.
    // TranslateDraw(): the same text with diacritics folded to ASCII, for raw
    // text draws whose font may lack them (ext LocTrDraw).
    const char* Translate(const char* s);
    const char* TranslateDraw(const char* s);

    // General one-arg helper: Translate(en) with an English pass-through. This
    // is the multi-language replacement for hard-coded Tr(en, pl) call sites.
    const char* Tr(const char* en);

    // Backwards-compatible pair form: try the active table first, then fall back
    // to `pl` when Polish is active, else `en`. Keeps existing inline Polish
    // strings working while any other language is driven purely by its table.
    const char* Tr(const char* en, const char* pl);

}
