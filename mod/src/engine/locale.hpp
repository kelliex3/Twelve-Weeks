#pragma once

// Minimal port of tools/ext/loc.c language detection. The ext asks the engine's own
// text lookup (probe ids whose English and Polish wording differ, from the generated
// loc_pl.h) so it follows the in-game TEXT language live; that probe needs the
// generated tables and is deferred. Until then the language comes from T2R_LANG=pl|en
// or a one-line t2r-lang.txt ("pl" / "en" / "auto") next to the exe, defaulting to
// English, re-read every few seconds so an edit takes effect without a restart.

namespace engine::locale {

    enum class Lang { English, Polish };

    Lang        Current();
    bool        IsPolish();
    const char* Code();      // "en" / "pl"

    // The ext's loc.c translation layer. Translate() is LocTr (the text of an English
    // string in the active language) and TranslateDraw() is LocTrDraw (the same, with
    // diacritics folded for the raw text draws). The generated loc tables are not ported
    // yet, so both return `s` unchanged -- every caller already treats the returned
    // pointer as "the text to draw" (a pass-through is a valid answer).
    const char* Translate(const char* s);       // ext: LocTr
    const char* TranslateDraw(const char* s);   // ext: LocTrDraw

    // Re-read immediately on the next call (test / level-reload helper).
    void Reset();

}
