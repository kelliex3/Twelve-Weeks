// Pure profile/file/wire core for build-191 CUSTOMIZE (ported from
// tools/ext/customize_core.h). No engine/Windows dependency; standalone-tested by
// mod/tests/customize_core_test.cpp. Item tables live in customize-items.hpp.
// Append-only indexes and save/wire rules are preserved exactly.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "customize-items.hpp"

namespace net::ui::customize::core {

inline constexpr int kVer = 2;                 // + MASK category, one more index per faction
inline constexpr int kWearCats = 7;            // hat torso lower backpack face melee mask
inline constexpr int kMsgLen = 4 + 2 * (1 + kWearCats);
inline constexpr int kHideMax = 256;
inline constexpr int kGrantMax = 24;

// Compile-time gate for the UNLOCKED ONLY filter. The profile flag and its file
// round-trip always exist (saved indexes are append-only); this controls the
// page input/filter so a shipping build can drop the feature without changing
// the file or wire format.
#ifndef TO1_CZ_UONLY
#define TO1_CZ_UONLY 1
#endif
inline constexpr bool kUonlyEnabled = TO1_CZ_UONLY != 0;

// Page geometry from tools/features/customize_widgets.py (3840 x 2160 canvas).
// Kept beside the row helpers so hover / focus tests can run without the owner.
inline constexpr int   kCols = 3;
inline constexpr int   kRows = 4;
inline constexpr int   kCards = kCols * kRows;
inline constexpr float kHeadX = 146.0f;
inline constexpr float kHeadCy = 166.0f;
inline constexpr float kHeadPt = 66.0f;
inline constexpr float kHeadRuleX1 = 3700.0f;
inline constexpr float kFacY = kHeadCy + 20.0f;
inline constexpr float kFacH = 72.0f;
inline constexpr float kFacPad = 32.0f;
inline constexpr float kFacW = 1240.0f;
inline constexpr float kBarCy = 361.0f;
inline constexpr float kBarIcX0 = 372.0f;
inline constexpr float kBarCell = 88.0f;
inline constexpr float kBarGap = 34.0f;
inline constexpr float kTabAlphaOff = 84.0f / 255.0f;
inline constexpr float kItemX = 191.0f;
inline constexpr float kItemCy = 497.0f;
inline constexpr float kItemPt = 56.0f;
inline constexpr float kItemRuleX1 = 1310.0f;
inline constexpr float kGridX = 205.0f;
inline constexpr float kGridY = 615.0f;
inline constexpr float kCardW = 300.0f;
inline constexpr float kCardH = 275.0f;
inline constexpr float kCardGap = 32.0f;
inline constexpr float kSbX = kGridX + kCols * kCardW + (kCols - 1) * kCardGap + 30.0f;
inline constexpr float kCardTextW = 190.0f;
inline constexpr float kReqDsY = kGridY + kRows * kCardH + (kRows - 1) * kCardGap + 36.0f + 56.0f;
inline constexpr float kReqPt = 32.0f;
inline constexpr float kReqNPt = 28.0f;
inline constexpr float kReqLineH = 60.0f;
inline constexpr int   kReqMaxLines = 3;
inline constexpr float kReqBarH = 6.0f;

// Compatibility names used by existing callers/tests.
inline constexpr int CZ_VER = kVer;
inline constexpr int CZ_NWEAR = kWearCats;
inline constexpr int CZ_MSG_LEN = kMsgLen;
inline constexpr int CZ_HIDE_MAX = kHideMax;
inline constexpr int CZ_NGRANT = kGrantMax;

struct Fstat {
    std::array<uint32_t, CZK_N> st{};
    std::array<uint32_t, CZ_NUMAP> mw{};
    std::array<uint32_t, CZ_NLBL> cnt{};
    std::array<uint32_t, CZ_NLBL> best{};
};

struct Profile {
    std::array<uint8_t, CZ_NFAC> sex{};
    uint8_t it[CZ_NFAC][CZ_NSEX][CZ_NCAT]{};   // [..][CZ_SEX] unused (sex[] holds it)
    std::array<Fstat, CZ_NFAC> fs{};
    uint32_t flags = 0;                          // CZF_* one-time UI state
    std::array<const char*, kGrantMax> grant{};  // labels into item tables (or nullptr)
};

using cz_fstat_t = Fstat;
using cz_profile_t = Profile;

inline constexpr uint32_t kFlagChTip = 1u;
inline constexpr uint32_t CZF_CHTIP = kFlagChTip;
inline constexpr uint32_t kFlagUonly = 2u;
inline constexpr uint32_t CZF_UONLY = kFlagUonly;

inline constexpr std::array<const char*, 2> kFlagKeys{{"chtip", "uonly"}};
inline constexpr int kFlagCount = static_cast<int>(kFlagKeys.size());
inline constexpr const char* const CZF_KEY[] = {"chtip", "uonly"};
inline constexpr int CZF_N = 2;

inline constexpr std::array<const char*, CZ_NFAC> kFacKeys{{"wlf", "scar"}};
inline constexpr std::array<const char*, CZ_NSEX> kSexKeys{{"m", "f"}};
inline constexpr std::array<const char*, kWearCats> kWearKeys{
    {"hat", "torso", "lower", "backpack", "face", "melee", "mask"}};
inline constexpr const char* const CZ_FAC_KEY[CZ_NFAC] = {"wlf", "scar"};
inline constexpr const char* const CZ_SEX_KEY[CZ_NSEX] = {"m", "f"};
inline constexpr const char* const CZ_WEAR_KEY[CZ_NWEAR] = {
    "hat", "torso", "lower", "backpack", "face", "melee", "mask"};

[[nodiscard]] constexpr bool IsScalarKind(int k) noexcept
{
    return k > 0 && k < CZK_N && k != CZK_MAPWINS && k != CZK_COUNT && k != CZK_BEST;
}
inline int cz_scalar(int k) { return IsScalarKind(k) ? 1 : 0; }

[[nodiscard]] inline int Count(int fac, int sex, int cat) noexcept
{
    if (fac < 0 || fac >= CZ_NFAC || sex < 0 || sex >= CZ_NSEX || cat < 0 || cat >= CZ_NCAT)
        return 0;
    return CZ_LISTS[fac][sex][cat].n;
}
inline int cz_count(int fac, int sex, int cat) { return Count(fac, sex, cat); }

[[nodiscard]] inline const cz_item_t* Item(int fac, int sex, int cat, int i) noexcept
{
    return i >= 0 && i < Count(fac, sex, cat) ? &CZ_LISTS[fac][sex][cat].items[i] : nullptr;
}
inline const cz_item_t* cz_item(int fac, int sex, int cat, int i) { return Item(fac, sex, cat, i); }

[[nodiscard]] inline int WearIndex(int f, int s, int c, const char* label) noexcept;
[[nodiscard]] inline int ConflictCat(int fac, int sex, const uint8_t* worn, int cat, int k) noexcept;

// The saved category order stays append-only; the tab bar puts MASK next to
// HEADWEAR. `tabIc<i>` uses the saved category but is drawn at `TabPos(cat)`.
inline constexpr std::array<int, CZ_NCAT> kTabOrder{{
    CZ_HAT, CZ_MASK, CZ_TORSO, CZ_LOWER, CZ_BACKPACK, CZ_FACE, CZ_MELEE, CZ_SEX}};
inline const int CZ_TAB_ORDER[CZ_NCAT] = {CZ_HAT, CZ_MASK, CZ_TORSO, CZ_LOWER,
                                          CZ_BACKPACK, CZ_FACE, CZ_MELEE, CZ_SEX};
inline constexpr const char* const kCatIcon[CZ_NCAT] = {
    "cz-hat", "cz-torso", "cz-lower", "cz-backpack", "cz-face", "cz-melee", "cz-mask", "cz-sex"};
inline constexpr const char* const kFacName[CZ_NFAC] = {"WLF", "SERAPHITES"};
inline constexpr const char* const kFacLogo[CZ_NFAC] = {
    "streaming/photo-mode/logos/wlf-wolf-logo.png",
    "streaming/photo-mode/logos/scar-logo.png"};

[[nodiscard]] inline int TabPos(int cat) noexcept
{
    for (int i = 0; i < CZ_NCAT; ++i) if (kTabOrder[i] == cat) return i;
    return 0;
}
inline int cz_tab_pos(int cat) { return TabPos(cat); }

[[nodiscard]] inline int TabStep(int cat, int d) noexcept
{
    const int at = TabPos(cat) + d;
    return kTabOrder[(at % CZ_NCAT + CZ_NCAT) % CZ_NCAT];
}
inline int cz_tab_step(int cat, int d) { return TabStep(cat, d); }

// Faction whose list holds item it (progress is that faction's).
[[nodiscard]] inline int ItemFac(const cz_item_t* it) noexcept
{
    if (!it) return 0;
    for (int f = 0; f < CZ_NFAC; ++f)
        for (int s = 0; s < CZ_NSEX; ++s)
            for (int c = 0; c < CZ_NCAT; ++c) {
                const cz_list_t& L = CZ_LISTS[f][s][c];
                if (it >= L.items && it < L.items + L.n) return f;
            }
    return 0;
}
inline int cz_item_fac(const cz_item_t* it) { return ItemFac(it); }

[[nodiscard]] inline uint32_t Progress(const Profile& p, const cz_item_t* it) noexcept
{
    if (!it || !it->uk || it->uk >= CZ_NCHAL) return 0;
    const cz_chal_t& c = CZ_CHAL[it->uk];
    const Fstat& q = p.fs[ItemFac(it)];
    switch (c.kind) {
    case CZK_MAPWINS: return it->um < CZ_NUMAP ? q.mw[it->um] : 0;
    case CZK_COUNT:   return c.arg < CZ_NLBL ? q.cnt[c.arg] : 0;
    case CZK_BEST:    return c.arg < CZ_NLBL ? q.best[c.arg] : 0;
    default:          return IsScalarKind(c.kind) ? q.st[c.kind] : 0;
    }
}
inline uint32_t cz_progress(const cz_profile_t* p, const cz_item_t* it)
{
    return p ? Progress(*p, it) : 0;
}

[[nodiscard]] inline bool Granted(const Profile& p, const cz_item_t* it) noexcept
{
    if (!it) return false;
    if (it->uk < CZ_NCHAL && (CZ_CHAL[it->uk].kind == CZK_OG || CZ_CHAL[it->uk].kind == CZK_KELLIE))
        return false;
    for (const char* g : p.grant)
        if (g && std::strcmp(g, it->label) == 0) return true;
    return false;
}
inline int cz_granted(const cz_profile_t* p, const cz_item_t* it)
{
    return p && Granted(*p, it) ? 1 : 0;
}

// The list label a "grant" line names (first row of any list with that label).
[[nodiscard]] inline const char* Label(const char* name) noexcept
{
    if (!name) return nullptr;
    for (int f = 0; f < CZ_NFAC; ++f)
        for (int s = 0; s < CZ_NSEX; ++s)
            for (int c = 0; c < CZ_NCAT; ++c) {
                const cz_list_t& L = CZ_LISTS[f][s][c];
                for (int i = 0; i < L.n; ++i)
                    if (std::strcmp(L.items[i].label, name) == 0) return L.items[i].label;
            }
    return nullptr;
}
inline const char* cz_label(const char* name) { return Label(name); }

// 1 = added, 0 = already there / no such label / no room.
[[nodiscard]] inline bool GrantAdd(Profile& p, const char* label) noexcept
{
    const char* lb = Label(label);
    if (!lb) return false;
    for (const char* g : p.grant) if (g == lb) return false;
    for (auto& g : p.grant) if (!g) { g = lb; return true; }
    return false;
}
inline int cz_grant_add(cz_profile_t* p, const char* label)
{
    return p && GrantAdd(*p, label) ? 1 : 0;
}

[[nodiscard]] inline bool Unlocked(const Profile& p, const cz_item_t* it) noexcept
{
    return it && (!it->uk || Granted(p, it) || (it->uk < CZ_NCHAL && Progress(p, it) >= it->un));
}
inline int cz_unlocked(const cz_profile_t* p, const cz_item_t* it)
{
    return p && Unlocked(*p, it) ? 1 : 0;
}

// Plain rows have a title / line but no challenge bar.
[[nodiscard]] inline bool PlainKind(int kind) noexcept
{
    return kind == CZK_OG || kind == CZK_KELLIE || kind == CZK_ACH;
}
inline int cz_plain_kind(int kind) { return PlainKind(kind) ? 1 : 0; }

// The story gas masks (card pictures from wlf_gasmasks) hide the whole body;
// release builds never list them. Their saved indexes stay valid and fall back
// to GAS MASK on load.
[[nodiscard]] inline bool StoryMask(const cz_item_t* it) noexcept
{
    return it && it->pic && std::strstr(it->pic, "-grp-t2r-gm-") != nullptr;
}
inline int cz_story_mask(const cz_item_t* it) { return StoryMask(it) ? 1 : 0; }

[[nodiscard]] inline int StoryMaskFallback(Profile& p) noexcept
{
    int changed = 0;
    for (int f = 0; f < CZ_NFAC; ++f)
        for (int s = 0; s < CZ_NSEX; ++s) {
            const cz_item_t* it = Item(f, s, CZ_MASK, p.it[f][s][CZ_MASK]);
            if (!StoryMask(it)) continue;
            const int gas = WearIndex(f, s, CZ_MASK, "GAS MASK");
            if (gas < 0) continue;
            p.it[f][s][CZ_MASK] = static_cast<uint8_t>(gas);
            ++changed;
        }
    return changed;
}
inline int cz_story_mask_fallback(cz_profile_t* p)
{
    return p ? StoryMaskFallback(*p) : 0;
}

// One row of a category list the CUSTOMIZE page actually draws. `owner` is the
// hidden-row account (KELLIE); the native port has no such privileged account.
// `storyMasks` is the dev opt-in sidecar (release builds always pass false).
[[nodiscard]] inline bool Unlisted(const cz_item_t* it, bool owner,
                                    bool storyMasks) noexcept
{
    return !it || (it->hid != 0 && !owner) || (StoryMask(it) && !storyMasks);
}
inline int cz_unlisted(const cz_item_t* it, int owner, int storyMasks)
{
    return Unlisted(it, owner != 0, storyMasks != 0) ? 1 : 0;
}

// UNLOCKED ONLY: keep the worn item listed so the grid never loses its check;
// SEX is never filtered. Locked rows are hidden, hidden/story rows stay hidden.
[[nodiscard]] inline bool RowOut(const Profile& p, int fac, int sex, int cat,
                                 int k, bool owner, bool storyMasks) noexcept
{
    const cz_item_t* it = Item(fac, sex, cat, k);
    if (Unlisted(it, owner, storyMasks)) return true;
    if (!kUonlyEnabled || (p.flags & kFlagUonly) == 0 || cat == CZ_SEX) return false;
    const int worn = p.it[fac][sex < CZ_NSEX ? sex : 0][cat];
    if (k == worn) return false;
    return !Unlocked(p, it);
}
inline int cz_row_out(const cz_profile_t* p, int fac, int sex, int cat, int k,
                      int owner, int storyMasks)
{
    return p && RowOut(*p, fac, sex, cat, k, owner != 0, storyMasks != 0) ? 1 : 0;
}

[[nodiscard]] inline int Listed(const Profile& p, int fac, int sex, int cat,
                                 bool owner, bool storyMasks) noexcept
{
    int n = 0;
    for (int k = 0; k < Count(fac, sex, cat); ++k)
        n += !RowOut(p, fac, sex, cat, k, owner, storyMasks);
    return n;
}
inline int cz_listed(const cz_profile_t* p, int fac, int sex, int cat,
                     int owner, int storyMasks)
{
    return p ? Listed(*p, fac, sex, cat, owner != 0, storyMasks != 0) : 0;
}

// Page row position -> table index (and back). Unlisted indexes map to row 0 so
// a saved look that is no longer shown still has a safe focus / check slot.
[[nodiscard]] inline int R2K(const Profile& p, int fac, int sex, int cat, int r,
                             bool owner, bool storyMasks) noexcept
{
    if (r < 0) return -1;
    for (int k = 0; k < Count(fac, sex, cat); ++k)
        if (!RowOut(p, fac, sex, cat, k, owner, storyMasks) && r-- == 0) return k;
    return -1;
}
inline int cz_r2k(const cz_profile_t* p, int fac, int sex, int cat, int r,
                  int owner, int storyMasks)
{
    return p ? R2K(*p, fac, sex, cat, r, owner != 0, storyMasks != 0) : -1;
}

[[nodiscard]] inline int K2R(const Profile& p, int fac, int sex, int cat, int k,
                             bool owner, bool storyMasks) noexcept
{
    int r = 0;
    const int n = Count(fac, sex, cat);
    for (int i = 0; i < n && i < k; ++i)
        r += !RowOut(p, fac, sex, cat, i, owner, storyMasks);
    return k >= 0 && k < n && !RowOut(p, fac, sex, cat, k, owner, storyMasks) ? r : 0;
}
inline int cz_k2r(const cz_profile_t* p, int fac, int sex, int cat, int k,
                  int owner, int storyMasks)
{
    return p ? K2R(*p, fac, sex, cat, k, owner != 0, storyMasks != 0) : 0;
}

[[nodiscard]] inline int Equipped(const Profile& p, int fac, int sex, int cat) noexcept
{
    return cat == CZ_SEX ? (sex < CZ_NSEX ? p.sex[fac] : 0)
                         : p.it[fac][sex < CZ_NSEX ? sex : 0][cat];
}
inline int cz_equipped(const cz_profile_t* p, int fac, int sex, int cat)
{
    return p && fac >= 0 && fac < CZ_NFAC ? Equipped(*p, fac, sex, cat) : 0;
}

[[nodiscard]] inline int EquippedRow(const Profile& p, int fac, int sex, int cat,
                                     bool owner, bool storyMasks) noexcept
{
    return K2R(p, fac, sex, cat, Equipped(p, fac, sex, cat), owner, storyMasks);
}
inline int cz_eq_row(const cz_profile_t* p, int fac, int sex, int cat,
                     int owner, int storyMasks)
{
    return p ? EquippedRow(*p, fac, sex, cat, owner != 0, storyMasks != 0) : 0;
}

// Fill an unlock requirement without the engine / locale layer. The caller
// applies loc_tr() to the completed title and line, as the ext does.
struct LockParts {
    const char* title = "";
    char line[160]{};
    uint32_t done = 0;
    uint32_t need = 0;
};

[[nodiscard]] inline LockParts LockTextFor(const Profile& p, const cz_item_t* it) noexcept
{
    LockParts out;
    if (!it || !it->uk || it->uk >= CZ_NCHAL) return out;
    const cz_chal_t& c = CZ_CHAL[it->uk];
    char t[112];
    out.need = it->un;
    if (c.kind == CZK_SAFES && out.need == 1)
        std::snprintf(t, sizeof t, "CRACK A SAFE IN INTERROGATION");
    else if (c.kind == CZK_MAPWINS && out.need == 1)
        std::snprintf(t, sizeof t, "WIN A MATCH ON %s",
                      it->um < CZ_NUMAP ? CZ_UMAP_NAME[it->um] : "?");
    else if (c.kind == CZK_WEEKS && out.need == 1)
        std::snprintf(t, sizeof t, "SURVIVE A WEEK");
    else if (c.kind == CZK_FLAWLESS && out.need == 1)
        std::snprintf(t, sizeof t, "WIN A MATCH WITHOUT DYING");
    else if (c.kind == CZK_MAPWINS)
        std::snprintf(t, sizeof t, c.line, out.need,
                      it->um < CZ_NUMAP ? CZ_UMAP_NAME[it->um] : "?");
    else
        std::snprintf(t, sizeof t, c.line, out.need);
    std::snprintf(out.line, sizeof out.line, "%s", t);
    out.title = c.title;
    const uint32_t got = Progress(p, it);
    out.done = got < out.need ? got : out.need;
    return out;
}

inline void LockLine(const Profile& p, const cz_item_t* it, char* out, int cap) noexcept
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    const LockParts lp = LockTextFor(p, it);
    if (!lp.title[0] && !lp.line[0]) return;
    std::snprintf(out, static_cast<size_t>(cap), "%s:  %s  %u / %u",
                  lp.title, lp.line, lp.done, lp.need);
}

// cz_req_lines from the generator: greedy wrap on the kill-feed glyph widths.
[[nodiscard]] inline float ReqCharWidth(unsigned char c) noexcept
{
    return c == ' ' ? 0.40f : (c >= 'a' && c <= 'z') ? 0.62f : 0.80f;
}

[[nodiscard]] inline int ReqLines(const char* text, float pt, float width) noexcept
{
    const float sp = ReqCharWidth(' ') * pt * 1.06f;
    const float lim = width * 0.96f;
    float x = 0.0f;
    int lines = 1;
    const unsigned char* q = reinterpret_cast<const unsigned char*>(text ? text : "");
    while (*q) {
        while (*q == ' ') ++q;
        if (!*q) break;
        float ww = 0.0f;
        for (; *q && *q != ' '; ++q)
            if ((*q & 0xC0) != 0x80) ww += ReqCharWidth(*q) * pt * 1.06f;
        if (x > 0.0f && x + sp + ww > lim) { ++lines; x = ww; }
        else x += (x > 0.0f ? sp : 0.0f) + ww;
    }
    return lines;
}

// The equip checks shared by CUSTOMIZE and CHALLENGES.
struct EquipCheck {
    bool ok = false;
    bool locked = false;
    bool tier = false;
    int  conflictCat = -1;
};

[[nodiscard]] inline EquipCheck CheckEquip(const Profile& p, int fac, int sex,
                                           int cat, int k) noexcept
{
    EquipCheck out;
    const cz_item_t* it = Item(fac, sex, cat, k);
    if (!it) return out;
    if (!Unlocked(p, it)) { out.locked = true; return out; }
    if (it->tier > 1) { out.tier = true; return out; }
    if (cat != CZ_SEX && ConflictCat(fac, sex, p.it[fac][sex < CZ_NSEX ? sex : 0], cat, k) >= 0)
        out.conflictCat = ConflictCat(fac, sex, p.it[fac][sex < CZ_NSEX ? sex : 0], cat, k);
    out.ok = out.conflictCat < 0;
    return out;
}

// Focus/offset window over the shown rows (3 x 4 cards).
inline void KeepVisibleRows(int& foc, int& off, int n) noexcept
{
    if (foc >= n) foc = n ? n - 1 : 0;
    if (foc < 0) foc = 0;
    const int row = foc / kCols, top = off / kCols;
    if (row < top) off = row * kCols;
    else if (row >= top + kRows) off = (row - kRows + 1) * kCols;
}
inline void cz_keep_visible_rows(int* foc, int* off, int n)
{
    if (foc && off) KeepVisibleRows(*foc, *off, n);
}

inline void Defaults(Profile& p) noexcept
{
    p = Profile{};
    for (int f = 0; f < CZ_NFAC; ++f)
        for (int s = 0; s < CZ_NSEX; ++s)
            for (int c = 0; c < CZ_NCAT; ++c)
                p.it[f][s][c] = CZ_DEFAULT[f][s][c];
}
inline void cz_defaults(cz_profile_t* p) { if (p) Defaults(*p); }

// Every index in range (out of range -> default); returns how many were fixed.
inline int Clamp(Profile& p) noexcept
{
    int bad = 0;
    for (int f = 0; f < CZ_NFAC; ++f) {
        if (p.sex[f] >= CZ_NSEX) { p.sex[f] = 0; ++bad; }
        for (int s = 0; s < CZ_NSEX; ++s)
            for (int c = 0; c < CZ_NCAT; ++c)
                if (c != CZ_SEX && p.it[f][s][c] >= Count(f, s, c)) {
                    p.it[f][s][c] = CZ_DEFAULT[f][s][c];
                    ++bad;
                }
    }
    return bad;
}
inline int cz_clamp(cz_profile_t* p) { return p ? Clamp(*p) : 0; }

// Default is always unlocked; locked index -> default.
inline int ClampLocked(Profile& p) noexcept
{
    int bad = 0;
    for (int f = 0; f < CZ_NFAC; ++f)
        for (int s = 0; s < CZ_NSEX; ++s)
            for (int c = 0; c < CZ_NCAT; ++c) {
                const cz_list_t& L = CZ_LISTS[f][s][c];
                if (c != CZ_SEX && p.it[f][s][c] < L.n && !Unlocked(p, &L.items[p.it[f][s][c]])) {
                    p.it[f][s][c] = CZ_DEFAULT[f][s][c];
                    ++bad;
                }
            }
    return bad;
}
inline int cz_clamp_locked(cz_profile_t* p) { return p ? ClampLocked(*p) : 0; }

// FNV-1a over every label of every list in order (same as customize_items.py).
[[nodiscard]] inline uint16_t TableId() noexcept
{
    uint32_t h = 2166136261u;
    for (int f = 0; f < CZ_NFAC; ++f)
        for (int s = 0; s < CZ_NSEX; ++s)
            for (int c = 0; c < CZ_NCAT; ++c) {
                const cz_list_t& L = CZ_LISTS[f][s][c];
                for (int i = 0; i < L.n; ++i) {
                    for (const unsigned char* q = reinterpret_cast<const unsigned char*>(L.items[i].label); *q; ++q)
                        h = (h ^ *q) * 16777619u;
                    h = (h ^ 0x1fu) * 16777619u;
                }
                h = (h ^ 0x1eu) * 16777619u;
            }
    return static_cast<uint16_t>(h ^ (h >> 16));
}
inline uint16_t cz_table_id() { return TableId(); }

[[nodiscard]] inline int Key(const char* s, const char* const* keys, int n) noexcept
{
    if (!s) return -1;
    for (int i = 0; i < n; ++i)
        if (keys[i] && std::strcmp(s, keys[i]) == 0) return i;
    return -1;
}
inline int cz_key(const char* s, const char* const* keys, int n) { return Key(s, keys, n); }

inline constexpr uint8_t kLegacyDrop[][4] = {
    {1, 0, CZ_TORSO, 0}, // Seraphite male TAN RAINCOAT removed
};
inline const uint8_t CZ_LEGACY_DROP[][4] = {{1, 0, CZ_TORSO, 0}};

[[nodiscard]] inline int WearIndex(int f, int s, int c, const char* label) noexcept
{
    if (!label) return -1;
    const cz_list_t& L = CZ_LISTS[f][s][c];
    for (int i = 0; i < L.n; ++i)
        if (std::strcmp(L.items[i].label, label) == 0) return i;
    return -1;
}
inline int cz_wear_index(int f, int s, int c, const char* label) { return WearIndex(f, s, c, label); }

inline int Parse(Profile& p, const char* txt) noexcept
{
    Defaults(p);
    bool wear = false;
    if (!txt || std::strncmp(txt, "T2RCZ1", 6) != 0) return 0;
    const char* ln = txt;
    while (ln && *ln) {
        char line[160];
        const char* e = std::strchr(ln, '\n');
        size_t n = e ? static_cast<size_t>(e - ln) : std::strlen(ln);
        if (n >= sizeof line) n = sizeof line - 1;
        std::memcpy(line, ln, n);
        line[n] = 0;
        ln = e ? e + 1 : nullptr;

        if (std::strncmp(line, "grant ", 6) == 0) {
            size_t e2 = std::strlen(line);
            while (e2 > 6 && (line[e2 - 1] == 13 || line[e2 - 1] == ' ')) line[--e2] = 0;
            const char* lb = Label(line + 6);
            for (int i = 0; lb && i < kGrantMax; ++i) {
                if (p.grant[i] == lb) break;
                if (!p.grant[i]) { p.grant[i] = lb; break; }
            }
            continue;
        }
        if (std::strncmp(line, "wear ", 5) == 0) {
            char wf[8]{}, ws[8]{}, wc[12]{};
            int at = 0;
            if (std::sscanf(line, "wear %7s %7s %11s %n", wf, ws, wc, &at) == 3 && at > 0) {
                size_t e2 = std::strlen(line);
                while (e2 > static_cast<size_t>(at) && (line[e2 - 1] == 13 || line[e2 - 1] == ' '))
                    line[--e2] = 0;
                const int f = Key(wf, CZ_FAC_KEY, CZ_NFAC);
                const int s = Key(ws, CZ_SEX_KEY, CZ_NSEX);
                const int c = Key(wc, CZ_WEAR_KEY, kWearCats);
                const int i = (f >= 0 && s >= 0 && c >= 0) ? WearIndex(f, s, c, line + at) : -1;
                if (f >= 0 && s >= 0 && c >= 0) wear = true;
                if (i >= 0) p.it[f][s][c] = static_cast<uint8_t>(i);
            }
            continue;
        }

        char kw[8]{}, fa[8]{}, fs[24]{}, ss[8]{};
        int v[kWearCats]{};
        unsigned long un = 0;
        int f0 = -1, f1 = -1;
        bool got = false;
        if (std::sscanf(line, "%7s %7s %23s %lu", kw, fa, fs, &un) == 4
            && Key(fa, CZ_FAC_KEY, CZ_NFAC) >= 0
            && (!std::strcmp(kw, "stat") || !std::strcmp(kw, "mapwin")
                || !std::strcmp(kw, "count") || !std::strcmp(kw, "best"))) {
            f0 = f1 = Key(fa, CZ_FAC_KEY, CZ_NFAC);
            got = true;
        } else if (std::sscanf(line, "%7s %23s %lu", kw, fs, &un) == 3
                   && (!std::strcmp(kw, "stat") || !std::strcmp(kw, "mapwin")
                       || !std::strcmp(kw, "count") || !std::strcmp(kw, "best")
                       || !std::strcmp(kw, "flag"))) {
            f0 = 0;
            f1 = CZ_NFAC - 1;
            got = true;
        }
        if (got) {
            if (un > 1000000UL) un = 1000000UL;
            if (!std::strcmp(kw, "flag")) {
                const int i = Key(fs, CZF_KEY, CZF_N);
                if (i >= 0 && un) p.flags |= 1u << i;
                continue;
            }
            for (int f = f0; f <= f1; ++f) {
                Fstat& q = p.fs[f];
                if (!std::strcmp(kw, "stat")) {
                    const int i = Key(fs, CZK_KEY, CZK_N);
                    if (IsScalarKind(i)) q.st[i] = static_cast<uint32_t>(un);
                } else if (!std::strcmp(kw, "mapwin")) {
                    const int m = Key(fs, CZ_UMAP_KEY, CZ_NUMAP);
                    if (m >= 0) q.mw[m] = static_cast<uint32_t>(un);
                } else {
                    int i = 0;
                    while (i < CZ_NLBL && !(CZ_LBL_KEY[i] && std::strcmp(fs, CZ_LBL_KEY[i]) == 0))
                        ++i;
                    if (i < CZ_NLBL) (kw[0] == 'c' ? q.cnt : q.best)[i] = static_cast<uint32_t>(un);
                }
            }
            continue;
        }

        const int k = std::sscanf(line, "%7s %7s %7s %d %d %d %d %d %d %d",
                                  kw, fs, ss, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
        if (k < 3) continue;
        const int f = Key(fs, CZ_FAC_KEY, CZ_NFAC);
        const int s = Key(ss, CZ_SEX_KEY, CZ_NSEX);
        if (f < 0 || s < 0) continue;
        if (!std::strcmp(kw, "side")) p.sex[f] = static_cast<uint8_t>(s);
        else if (!std::strcmp(kw, "look"))
            for (int c = 0; c < kWearCats && c < k - 3; ++c)
                p.it[f][s][c] = static_cast<uint8_t>(v[c] >= 0 && v[c] < 256 ? v[c] : 255);
    }

    if (!wear)
        for (const auto& q : kLegacyDrop) {
            uint8_t& slot = p.it[q[0]][q[1]][q[2]];
            if (slot == 255) continue;
            if (slot == q[3]) slot = CZ_DEFAULT[q[0]][q[1]][q[2]];
            else if (slot > q[3]) --slot;
        }

    Clamp(p);
    ClampLocked(p);
    return 1;
}
inline int cz_parse(cz_profile_t* p, const char* txt) { return p ? Parse(*p, txt) : 0; }

inline int Format(const Profile& p, char* out, int cap) noexcept
{
    if (!out || cap <= 0) return 0;
    int n = std::snprintf(out, static_cast<size_t>(cap), "T2RCZ1\n");
    for (int f = 0; f < CZ_NFAC && n < cap; ++f)
        n += std::snprintf(out + n, static_cast<size_t>(cap - n), "side %s %s\n",
                           CZ_FAC_KEY[f], CZ_SEX_KEY[p.sex[f] < CZ_NSEX ? p.sex[f] : 0]);
    for (int f = 0; f < CZ_NFAC && n < cap; ++f)
        for (int s = 0; s < CZ_NSEX && n < cap; ++s) {
            n += std::snprintf(out + n, static_cast<size_t>(cap - n), "look %s %s", CZ_FAC_KEY[f], CZ_SEX_KEY[s]);
            for (int c = 0; c < kWearCats && n < cap; ++c)
                n += std::snprintf(out + n, static_cast<size_t>(cap - n), " %d", p.it[f][s][c]);
            if (n < cap) n += std::snprintf(out + n, static_cast<size_t>(cap - n), "\n");
        }
    for (int f = 0; f < CZ_NFAC && n < cap; ++f)
        for (int s = 0; s < CZ_NSEX && n < cap; ++s)
            for (int c = 0; c < kWearCats && n < cap; ++c) {
                const cz_item_t* x = Item(f, s, c, p.it[f][s][c]);
                if (x) n += std::snprintf(out + n, static_cast<size_t>(cap - n), "wear %s %s %s %s\n",
                                          CZ_FAC_KEY[f], CZ_SEX_KEY[s], CZ_WEAR_KEY[c], x->label);
            }
    for (int f = 0; f < CZ_NFAC; ++f) {
        const Fstat& q = p.fs[f];
        const char* fk = CZ_FAC_KEY[f];
        for (int i = 1; i < CZK_N && n < cap; ++i)
            if (IsScalarKind(i))
                n += std::snprintf(out + n, static_cast<size_t>(cap - n), "stat %s %s %u\n",
                                   fk, CZK_KEY[i], static_cast<unsigned>(q.st[i]));
        for (int m = 0; m < CZ_NUMAP && n < cap; ++m)
            n += std::snprintf(out + n, static_cast<size_t>(cap - n), "mapwin %s %s %u\n",
                               fk, CZ_UMAP_KEY[m], static_cast<unsigned>(q.mw[m]));
        for (int i = 0; i < CZ_NLBL && n < cap; ++i)
            if (CZ_LBL_KEY[i] && q.cnt[i])
                n += std::snprintf(out + n, static_cast<size_t>(cap - n), "count %s %s %u\n",
                                   fk, CZ_LBL_KEY[i], static_cast<unsigned>(q.cnt[i]));
        for (int i = 0; i < CZ_NLBL && n < cap; ++i)
            if (CZ_LBL_KEY[i] && q.best[i])
                n += std::snprintf(out + n, static_cast<size_t>(cap - n), "best %s %s %u\n",
                                   fk, CZ_LBL_KEY[i], static_cast<unsigned>(q.best[i]));
    }
    for (int i = 0; i < kFlagCount && n < cap; ++i)
        if (p.flags & (1u << i))
            n += std::snprintf(out + n, static_cast<size_t>(cap - n), "flag %s 1\n", CZF_KEY[i]);
    for (int i = 0; i < kGrantMax && n < cap; ++i)
        if (p.grant[i])
            n += std::snprintf(out + n, static_cast<size_t>(cap - n), "grant %s\n", p.grant[i]);
    return n < cap ? n : cap - 1;
}
inline int cz_format(const cz_profile_t* p, char* out, int cap) { return p ? Format(*p, out, cap) : 0; }

inline int Msg(const Profile& p, uint8_t counter, uint8_t* b) noexcept
{
    if (!b) return 0;
    const uint16_t id = TableId();
    b[0] = kVer;
    b[1] = static_cast<uint8_t>(id);
    b[2] = static_cast<uint8_t>(id >> 8);
    b[3] = counter;
    for (int f = 0; f < CZ_NFAC; ++f) {
        uint8_t* q = b + 4 + f * (1 + kWearCats);
        const uint8_t s = p.sex[f] < CZ_NSEX ? p.sex[f] : 0;
        q[0] = s;
        for (int c = 0; c < kWearCats; ++c) q[1 + c] = p.it[f][s][c];
    }
    return kMsgLen;
}
inline int cz_msg(const cz_profile_t* p, uint8_t counter, uint8_t* b) { return p ? Msg(*p, counter, b) : 0; }

struct Wire {
    uint8_t ok = 0;
    uint8_t counter = 0;
    std::array<uint8_t, CZ_NFAC> sex{};
    uint8_t it[CZ_NFAC][CZ_NWEAR]{};
};
using cz_wire_t = Wire;

inline int Recv(const uint8_t* b, int len, Wire& out) noexcept
{
    out = Wire{};
    if (!b || len < kMsgLen || b[0] != kVer
        || static_cast<uint16_t>(b[1] | (b[2] << 8)) != TableId())
        return 0;
    out.counter = b[3];
    for (int f = 0; f < CZ_NFAC; ++f) {
        const uint8_t* q = b + 4 + f * (1 + kWearCats);
        if (q[0] >= CZ_NSEX) return 0;
        out.sex[f] = q[0];
        for (int c = 0; c < kWearCats; ++c) {
            if (q[1 + c] >= Count(f, q[0], c)) return 0;
            out.it[f][c] = q[1 + c];
        }
    }
    out.ok = 1;
    return 1;
}
inline int cz_recv(const uint8_t* b, int len, cz_wire_t* out) { return out ? Recv(b, len, *out) : 0; }

// ---- compose helpers -------------------------------------------------------

struct Part { const char* name = nullptr; uint8_t hidden = 0; };
using cz_part_t = Part;

inline int PushedName(const char* n, char* out, int cap) noexcept
{
    if (!n || !out || cap <= 0) return 0;
    static constexpr const char* kRep[][2] = {
        {"-t2r-bp-rigged", "-t2r-bp-armour"},
        {"-male-", "-arm1-"},
        {"-female-", "-farm01-"},
    };
    if (std::strncmp(n, "t2r-fh-f-h", 10) == 0 && n[10] && std::strncmp(n + 11, "ale-head-", 9) == 0) {
        const int k = std::snprintf(out, static_cast<size_t>(cap), "militia-farm01-head-%s", n + 20);
        return k > 0 && k < cap;
    }
    if (std::strncmp(n, "t2r-wh-f-hhale-", 15) == 0 && std::strstr(n, "-torso") && !std::strstr(n, "arm-")) {
        const int k = std::snprintf(out, static_cast<size_t>(cap), "militia-farm01-%s", n + 15);
        return k > 0 && k < cap;
    }
    if (std::strncmp(n, "t2r-wh-m-hhe-", 13) == 0 && std::strstr(n, "-torso") && !std::strstr(n, "arm-")) {
        const int k = std::snprintf(out, static_cast<size_t>(cap), "militia-arm1-%s", n + 13);
        return k > 0 && k < cap;
    }
    if (std::strncmp(n, "t2r-fh-m-h", 10) == 0 && n[10] && std::strncmp(n + 11, "e-head-", 7) == 0) {
        const int k = std::snprintf(out, static_cast<size_t>(cap), "militia-arm1-head-%s", n + 18);
        return k > 0 && k < cap;
    }
    for (const auto& r : kRep) {
        const char* at = std::strstr(n, r[0]);
        if (!at) continue;
        const int k = std::snprintf(out, static_cast<size_t>(cap), "%.*s%s%s",
                                    static_cast<int>(at - n), n, r[1], at + std::strlen(r[0]));
        return k > 0 && k < cap;
    }
    return 0;
}
inline int cz_pushed_name(const char* n, char* out, int cap) { return PushedName(n, out, cap); }

[[nodiscard]] inline bool IsPushed(const char* n) noexcept
{
    return n && (std::strstr(n, "-arm1-") || std::strstr(n, "-farm01-")
                 || std::strstr(n, "-bp-armour") || std::strstr(n, "-pushed-"));
}
inline int cz_is_pushed(const char* n) { return IsPushed(n) ? 1 : 0; }

[[nodiscard]] inline bool InList(const char* const* l, const char* n) noexcept
{
    for (; l && *l; ++l) if (std::strcmp(*l, n) == 0) return true;
    return false;
}
inline int cz_in(const char* const* l, const char* n) { return InList(l, n) ? 1 : 0; }

[[nodiscard]] inline int ConflictCat(int fac, int sex, const uint8_t* worn, int cat, int k) noexcept
{
    const cz_item_t* x = cat >= 0 && cat < kWearCats ? Item(fac, sex, cat, k) : nullptr;
    if (!x || !worn || !x->parts || !x->parts[0]) return -1;
    for (int c = 0; c < kWearCats; ++c) {
        if (c == cat) continue;
        const cz_item_t* w = Item(fac, sex, c, worn[c]);
        if (!w || !w->hide || !w->hide[0]) continue;
        bool all = true;
        for (const char* const* p = x->parts; *p && all; ++p) all = InList(w->hide, *p);
        if (all) return c;
    }
    return -1;
}
inline int cz_conflict_cat(int fac, int sex, const uint8_t* worn, int cat, int k)
{
    return ConflictCat(fac, sex, worn, cat, k);
}

[[nodiscard]] inline bool ItemPart(int fac, int sex, const char* n) noexcept
{
    if (!n) return false;
    for (int c = 0; c < kWearCats; ++c)
        for (int k = 0; k < Count(fac, sex, c); ++k) {
            const cz_item_t* it = Item(fac, sex, c, k);
            if (InList(it->parts, n) || InList(it->hide, n)) return true;
        }
    return false;
}
inline int cz_item_part(int fac, int sex, const char* n) { return ItemPart(fac, sex, n) ? 1 : 0; }

[[nodiscard]] inline bool IsSlotPart(const char* n) noexcept
{
    if (!n) return false;
    static constexpr const char* kSlots[] = {
        "-head-", "-torso", "-legs", "backpack", "-bp-", "-hat-", "-mask-", "-hood"};
    for (const char* s : kSlots) if (std::strstr(n, s)) return true;
    return false;
}
inline int cz_is_slot_part(const char* n) { return IsSlotPart(n) ? 1 : 0; }

[[nodiscard]] inline bool Kept(const Part* sup, const uint8_t* keep, int n, const char* name) noexcept
{
    for (int i = 0; i < n; ++i) if (keep[i] && std::strcmp(sup[i].name, name) == 0) return true;
    return false;
}
inline int cz_kept(const cz_part_t* sup, const uint8_t* keep, int n, const char* name)
{
    return Kept(sup, keep, n, name) ? 1 : 0;
}

inline bool Take(const Part* sup, uint8_t* keep, int n, const char* name, int hidden) noexcept
{
    if (Kept(sup, keep, n, name)) return true;
    for (int i = 0; i < n; ++i)
        if (!keep[i] && std::strcmp(sup[i].name, name) == 0 && (hidden < 0 || sup[i].hidden == hidden)) {
            keep[i] = 1;
            return true;
        }
    return false;
}
inline int cz_take(const cz_part_t* sup, uint8_t* keep, int n, const char* name, int hidden)
{
    return Take(sup, keep, n, name, hidden) ? 1 : 0;
}

inline void Effective(int fac, int sex, const uint8_t* it, uint8_t* out) noexcept
{
    std::memcpy(out, it, CZ_NCAT);
    for (int c = 0; c < kWearCats; ++c)
        if (ConflictCat(fac, sex, it, c, it[c]) >= 0) out[c] = CZ_DEFAULT[fac][sex][c];
}
inline void cz_effective(int fac, int sex, const uint8_t* it, uint8_t* out) { Effective(fac, sex, it, out); }

inline int Select(int fac, int sex, const uint8_t* it0, const Part* sup, int n,
                  const char* const* tx, const char* const* lx, uint8_t* keep) noexcept
{
    uint8_t it[CZ_NCAT];
    Effective(fac, sex, it0, it);
    std::memset(keep, 0, static_cast<size_t>(n));

    const char* hide[kHideMax];
    int nh = 0;
    for (int c = 0; c < kWearCats; ++c) {
        const cz_item_t* x = Item(fac, sex, c, it[c]);
        for (const char* const* h = x ? x->hide : nullptr; h && *h && nh < kHideMax; ++h)
            hide[nh++] = *h;
    }
    for (int c = 0; c < kWearCats; ++c) {
        const cz_item_t* x = Item(fac, sex, c, it[c]);
        for (const char* const* p = x ? x->parts : nullptr; p && *p; ++p) {
            bool hid = false;
            for (int h = 0; h < nh; ++h) if (std::strcmp(hide[h], *p) == 0) { hid = true; break; }
            if (hid) { Take(sup, keep, n, *p, 1); continue; }
            if (!Take(sup, keep, n, *p, 0)) return -1;
        }
    }
    for (int i = 0; i < n; ++i) {
        char q[0x40];
        if (keep[i] && !IsPushed(sup[i].name) && PushedName(sup[i].name, q, sizeof q))
            Take(sup, keep, n, q, -1);
    }
    for (int i = 0; i < n; ++i) {
        const char* nm = sup[i].name;
        if (keep[i] || Kept(sup, keep, n, nm)) continue;
        if (std::strncmp(nm, "slaver-", 7) == 0) { keep[i] = 1; continue; }
        if (IsPushed(nm) || ItemPart(fac, sex, nm) || IsSlotPart(nm)) continue;
        if (std::strstr(nm, "-holster-")) continue;
        if (InList(std::strstr(nm, "chestrig") ? tx : lx, nm)) keep[i] = 1;
    }
    int k = 0;
    for (int i = 0; i < n; ++i) k += keep[i];
    return k;
}
inline int cz_select(int fac, int sex, const uint8_t* it0, const cz_part_t* sup, int n,
                     const char* const* tx, const char* const* lx, uint8_t* keep)
{
    return Select(fac, sex, it0, sup, n, tx, lx, keep);
}

inline int g_noPushedProps = 0;
inline int& cz_no_pushed_props = g_noPushedProps;

inline int SelectProps(int fac, int sex, const uint8_t* it0, const char* const* names, int n, uint8_t* keep) noexcept
{
    uint8_t it[CZ_NCAT];
    Effective(fac, sex, it0, it);
    std::memset(keep, 0, static_cast<size_t>(n));
    int k = 0;
    for (int c = 0; c < kWearCats; ++c) {
        const cz_item_t* x = Item(fac, sex, c, it[c]);
        for (const char* const* p = x ? x->props : nullptr; p && *p; ++p) {
            char q[0x40];
            const bool pq = !g_noPushedProps && PushedName(*p, q, sizeof q) != 0;
            for (int i = 0; i < n; ++i)
                if (!keep[i] && (std::strcmp(names[i], *p) == 0 || (pq && std::strcmp(names[i], q) == 0))) {
                    keep[i] = 1;
                    ++k;
                }
        }
    }
    return k;
}
inline int cz_select_props(int fac, int sex, const uint8_t* it0, const char* const* names, int n, uint8_t* keep)
{
    return SelectProps(fac, sex, it0, names, n, keep);
}

inline int ArmourPairs(const Part* sup, const uint8_t* keep, int n,
                       const char** orig, const char** copy, int* mask, int cap) noexcept
{
    int k = 0;
    for (int i = 0; i < n && k < cap; ++i) {
        char q[0x40];
        if (!keep[i] || IsPushed(sup[i].name) || !PushedName(sup[i].name, q, sizeof q)
            || !Kept(sup, keep, n, q))
            continue;
        if (std::strstr(sup[i].name, "-uparm-") || std::strstr(sup[i].name, "-loarm-")) continue;
        if (sup[i].hidden && !std::strstr(sup[i].name, "-head-")) continue;
        int j = 0;
        while (j < n && !(keep[j] && std::strcmp(sup[j].name, q) == 0)) ++j;
        orig[k] = sup[i].name;
        copy[k] = sup[j].name;
        mask[k] = std::strstr(sup[i].name, "hooddown") ? 3 : std::strstr(sup[i].name, "-head-") ? 1 : 2;
        ++k;
    }
    return k;
}
inline int cz_armour_pairs(const cz_part_t* sup, const uint8_t* keep, int n,
                           const char** orig, const char** copy, int* mask, int cap)
{
    return ArmourPairs(sup, keep, n, orig, copy, mask, cap);
}

inline int FaceLook(const char* head, char* out, int cap) noexcept
{
    const char* at = head ? std::strstr(head, "-head-") : nullptr;
    if (!at) return 0;
    const int k = std::snprintf(out, static_cast<size_t>(cap), "%.*s-%s",
                                static_cast<int>(at - head), head, at + 6);
    return k > 0 && k < cap;
}
inline int cz_face_look(const char* head, char* out, int cap) { return FaceLook(head, out, cap); }

inline int LookName(int fac, int sex, int body, char* out, int cap) noexcept
{
    if (fac < 0 || fac >= CZ_NFAC || sex < 0 || sex >= CZ_NSEX) return 0;
    return std::snprintf(out, static_cast<size_t>(cap), "t2r-cz-%c%c-%d",
                         fac ? 's' : 'w', sex ? 'f' : 'm', body) < cap;
}
inline int cz_look_name(int fac, int sex, int body, char* out, int cap) { return LookName(fac, sex, body, out, cap); }

} // namespace net::ui::customize::core
