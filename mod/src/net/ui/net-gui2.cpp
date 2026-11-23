#include "net-gui2.hpp"
#include "match.hpp"
#include "net-matchmaking.hpp"
#include "net-vox.hpp"
#include "net-account.hpp"
#include "net-connect.hpp"
#include "net-config.hpp"
#include "menu/net-menu.hpp"
#include "net-replication.hpp"
#include "net-session.hpp"
#include "net-hooks.hpp"
#include "net-player.hpp"
#include "net-debug.hpp"
#include "engine/framecap.hpp"
#include "engine/local-player.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/pad.hpp"
#include "engine/window.hpp"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "gameplay/perks/perk-registry.hpp"
#include "gameplay/loadout/weapons.hpp"
#include "gameplay/loadout/loadout.hpp"
#include "gameplay/loadout/purchase.hpp"
#include "net/social/friends13.hpp"
#include "net/ui/postmatch-screens.hpp"
#include "net/ui/notify.hpp"
#include "net/online/steam-status.hpp"
#include "net/online/clan-state.hpp"
#include "net/match/match-liveness.hpp"

#define TO1_HUD(name) SID("canvas.hudRoot.hudMenuStack.to1/" name)

namespace {

    net::match::Match& TheMatch() { return net::match::Get(); }
    using net::match::Phase;
    Phase CurPhase()        { return net::match::Get().GetPhase(); }
    void  SetPhase(Phase p) { net::match::Get().SetPhase(p); }

    StringId64 node(const char* rel) { return MENU2::GET_PATH_IN_CURRENT_PAGE(rel, MenuStack::hudMenuStack); }
    StringId64 cnode(const char* rel) { return MENU2::GET_PATH_IN_CURRENT_PAGE(rel, MenuStack::main); }
    StringId64 nodef(const char* fmt, int i) { char b[128]; sprintf_s(b, fmt, i); return node(b); }
    StringId64 focused() { return DC::Call<"menu2-get-focused-component">(); }

    constexpr uint64_t kMouseLeft = 2;

    constexpr float kFrontEndFps = 60.0f;

    bool mouseEdge()
    {
        return DC::Call<"mouse-button-pressed?">(kMouseLeft) != 0;
    }

    bool hovered(StringId64 widgetPath)
    {
        if (!widgetPath) return false;
        auto isHovered = GameFn<char(__fastcall*)(StringId64)>(0x1E91510);
        return isHovered(widgetPath) != 0;
    }

    bool pressed(StringId64 cmd)
    {
        if (DC::JOYPAD_COMMAND_ACTIVE(cmd)) return true;
        return cmd == SID("menu-select") && mouseEdge();
    }

    bool selectTarget(StringId64 widgetPath)
    {
        if (mouseEdge()) return hovered(widgetPath);
        return focused() == widgetPath;
    }
    void       sound(StringId64 sfx) { AUDIO::PLAY_SOUND(sfx); }
    void       setVis(StringId64 w, bool on) { DC::Call<"gui2-set-widget-visible">(w, on); }
    void       setTxt(StringId64 w, const char* t) { DC::Call<"gui2-set-widget-text-devonly">(w, MEMORY::ALLOC_STRING(t)); }
    void       setImg(StringId64 w, const char* p) { DC::Call<"gui2-set-widget-image">(w, MEMORY::ALLOC_STRING(p)); }
    void       setPct(StringId64 w, float pct) { DC::Call<"gui2-set-progress-bar-percent">(w, pct, 0.0f, 12); }


    constexpr int kMapRows = 8;
    constexpr int kPickerRows = 8;
    constexpr int kSkillRows = 10;   // Skill0..Skill9 exist on the picker page
    const char* const kSkillSlots[] = { "SkillOne", "SkillTwo", "SkillThree" };
    const int kSkillSlotCount = int(sizeof(kSkillSlots) / sizeof(kSkillSlots[0]));

    int g_editSkillSlot = -1;   // which slot picker-skill is editing
    bool s_gearWasOpen = false;  // ext g_loState: previous frame's GEAR UP page edge

    StringId64 skillSlotNode(int i, const char* leaf = nullptr)
    {
        char b[160];
        if (leaf) sprintf_s(b, "anim-group.slotList.%s.%s", kSkillSlots[i], leaf);
        else      sprintf_s(b, "anim-group.slotList.%s", kSkillSlots[i]);
        return node(b);
    }

    void setLpTotal()
    {
        char b[32];
        sprintf_s(b, "%d/%d LP", gameplay::loadout::PointsSpent(), gameplay::kMaxLoadoutPoints);
        setTxt(node("anim-group.lpTotal"), b);
    }

    void fillSkillDetail(const gameplay::Perk* p)
    {
        if (!p) {
            setImg(node("anim-group.detailPanel.perkIcon"), "hud/weapon-cross/empty-slot.svg");
            setTxt(node("anim-group.detailPanel.perkName"), "");
            setTxt(node("anim-group.detailPanel.perkCost"), "");
            setTxt(node("anim-group.detailPanel.perkDesc"), "");
            return;
        }
        char cost[32]; sprintf_s(cost, "%u LP", p->lpCost);
        setImg(node("anim-group.detailPanel.perkIcon"), p->icon);
        setTxt(node("anim-group.detailPanel.perkName"), p->name);
        setTxt(node("anim-group.detailPanel.perkCost"), cost);
        setTxt(node("anim-group.detailPanel.perkDesc"), p->description);
    }

    void refreshSkillRows()
    {
        using namespace gameplay;
        const int n = perks::Count();

        for (int i = 0; i < kSkillRows; ++i)
        {
            const Perk* p = (i < n) ? perks::At(i) : nullptr;
            setVis(nodef("anim-group.list.contents.Skill%d", i), p != nullptr);
            if (!p) {
                setImg(nodef("anim-group.list.contents.Skill%d.skillIcon", i), "hud/weapon-cross/empty-slot.svg");
                setTxt(nodef("anim-group.list.contents.Skill%d.label0", i), "");
                setTxt(nodef("anim-group.list.contents.Skill%d.costText", i), "");
                continue;
            }

            setImg(nodef("anim-group.list.contents.Skill%d.skillIcon", i), p->icon);
            setTxt(nodef("anim-group.list.contents.Skill%d.label0", i), p->name);

            // costText doubles as the state readout: the skill rows have no
            // selected-mark node of their own.
            const int held = perks::SlotOf(p->id);
            char cost[48];
            if (held == g_editSkillSlot && held >= 0)             sprintf_s(cost, "EQUIPPED");
            else if (held >= 0)                                   sprintf_s(cost, "IN SKILL %d", held + 1);
            else if (!perks::CanEquipInSlot(g_editSkillSlot, p->id)) sprintf_s(cost, "%u LP |lock|", p->lpCost);
            else                                                  sprintf_s(cost, "%u LP", p->lpCost);
            setTxt(nodef("anim-group.list.contents.Skill%d.costText", i), cost);
        }

        setLpTotal();
    }

    void skillPicker(bool entered)
    {
        using namespace gameplay;

        static StringId64 s_lastFocus = StringId64(0);
        static bool       s_prevSelect = false;

        if (entered) {
            // Fallback if focus tracking on the loadout page missed: first empty slot.
            if (g_editSkillSlot < 0 || g_editSkillSlot >= kMaxEquippedPerks) {
                g_editSkillSlot = 0;
                for (int i = 0; i < kMaxEquippedPerks; ++i)
                    if (perks::EquippedAt(i) == PerkId::NONE) { g_editSkillSlot = i; break; }
                TO1_LOG("[perk] skill picker opened with no slot focused - editing slot%d\n", g_editSkillSlot);
            }

            char title[32]; sprintf_s(title, "SKILL %d", g_editSkillSlot + 1);
            setTxt(node("anim-group.chrome.menuTitle"), title);
            refreshSkillRows();
            s_lastFocus = StringId64(0);
            s_prevSelect = true;   // the press that opened this page is still held
        }

        // Detail panel follows focus. Only rewritten when focus actually moves.
        const StringId64 f = focused();
        if (f != s_lastFocus) {
            s_lastFocus = f;
            const Perk* shown = nullptr;
            for (int i = 0, n = perks::Count(); i < n; ++i)
                if (f == nodef("anim-group.list.contents.Skill%d", i)) { shown = perks::At(i); break; }
            fillSkillDetail(shown);
        }

        // menu-select is level-triggered: without an edge a held button would
        // equip and clear the slot on alternating frames.
        const bool sel = pressed(SID("menu-select"));
        const bool justPressed = sel && !s_prevSelect;
        s_prevSelect = sel;
        if (!justPressed) return;

        for (int i = 0, n = perks::Count(); i < n; ++i)
        {
            if (!selectTarget(nodef("anim-group.list.contents.Skill%d", i))) continue;
            const Perk* p = perks::At(i);
            if (!p) return;

            const int held = perks::SlotOf(p->id);
            if (held == g_editSkillSlot) {
                perks::ClearSlot(g_editSkillSlot);                  // pick it again to empty the slot
                sound(SID("sfx-rl-hub-interact-vendor-purchase-complete"));
            }
            else if (perks::EquipInSlot(g_editSkillSlot, p->id)) { // replaces whatever was here
                sound(SID("sfx-rl-hub-interact-vendor-purchase-complete"));
            }
            else {
                sound(SID("sfx-rl-round-timer-tick"));   // already in another slot, or not enough LP
            }

            refreshSkillRows();
            fillSkillDetail(p);
            return;
        }
    }

    int        g_map = 0;
    StringId64 g_lastPage = StringId64(0);

    constexpr StringId64 kDsReticleMode = SID("to1-reticle-mode");
    constexpr StringId64 kReticleOptionCiphers[] = {
        SID("TO1_RETICLE_MOVING"), SID("TO1_RETICLE_MARKER"), SID("TO1_RETICLE_GREY"), SID("TO1_RETICLE_NONE") };
    constexpr int kReticleModeCount = int(sizeof(kReticleOptionCiphers) / sizeof(kReticleOptionCiphers[0]));
    int g_reticleValue = 1; // default marker widget (was winmm_patches.py: reticle-marker-default)

    constexpr StringId64 kDsMenuFpsCap = SID("to1-menu-fps-cap");
    constexpr StringId64 kOnOffCiphers[] = { SID("MENU_OFF"), SID("MENU_ON") };
    constexpr int kOnOffCount = int(sizeof(kOnOffCiphers) / sizeof(kOnOffCiphers[0]));
    int g_menuFpsCapValue = 1; // default on

    constexpr StringId64 kDsRadarMode = SID("to1-radar-mode");
    constexpr StringId64 kRadarModeCiphers[] = { SID("TO1_RADAR_SQUARE"), SID("TO1_RADAR_CIRCLE") };
    constexpr int kRadarModeCount = int(sizeof(kRadarModeCiphers) / sizeof(kRadarModeCiphers[0]));
    int g_radarValue = 0; // default square

    SafetyHookInline g_hkDsGetItemData;
    SafetyHookInline g_hkDsGetRange;
    SafetyHookInline g_hkDsGetValuePtr;

    uint64_t DsSid(void* ds) { return *reinterpret_cast<const uint64_t*>(static_cast<const char*>(ds) + 8); }

    const StringId64* DsOptions(void* ds, int& countOut)
    {
        const uint64_t sid = DsSid(ds);
        if (sid == kDsReticleMode) { countOut = kReticleModeCount;            return kReticleOptionCiphers; }
        if (sid == kDsMenuFpsCap)  { countOut = kOnOffCount;                  return kOnOffCiphers; }
        if (sid == kDsRadarMode)   { countOut = kRadarModeCount;              return kRadarModeCiphers; }
        countOut = 0;
        return nullptr;
    }

    void* __fastcall hk_DsGetItemData(void* ds, int index, void* widget)
    {
        int count = 0;
        const StringId64* options = DsOptions(ds, count);
        if (!options)
            return g_hkDsGetItemData.fastcall<void*>(ds, index, widget);
        auto alloc    = GameFn<void* (__fastcall*)(int)>(0x122DB10);
        auto wrapList = GameFn<void* (__fastcall*)(void*, int)>(0x14CF860);
        if (index < 0 || index >= count) index = 0;
        void** arr = static_cast<void**>(alloc(8));
        char*  box = static_cast<char*>(alloc(16));
        if (!arr || !box) return nullptr;
        *reinterpret_cast<uint64_t*>(box)      = options[index];
        *reinterpret_cast<uint32_t*>(box + 8)  = 0;
        *reinterpret_cast<uint32_t*>(box + 12) = 7;
        *arr = box;
        return wrapList(arr, 1);
    }

    void __fastcall hk_DsGetRange(void* ds, int* minOut, int* maxOut, int* defOut)
    {
        int count = 0;
        if (DsOptions(ds, count))
        {
            *minOut = 0;
            *maxOut = count - 1;
            *defOut = 0;
            return;
        }
        g_hkDsGetRange.fastcall<void>(ds, minOut, maxOut, defOut);
    }

    int* __fastcall hk_DsGetValuePtr(void* ds)
    {
        const uint64_t sid = DsSid(ds);
        if (sid == kDsReticleMode) return &g_reticleValue;
        if (sid == kDsMenuFpsCap)  return &g_menuFpsCapValue;
        if (sid == kDsRadarMode)   return &g_radarValue;
        return g_hkDsGetValuePtr.fastcall<int*>(ds);
    }

    void EnsureReticleDataSource()
    {
        static bool hooked = false;
        if (!hooked)
        {
            hooked = true;
            g_hkDsGetItemData = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1C7B8F0), reinterpret_cast<void*>(hk_DsGetItemData));
            g_hkDsGetRange    = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1C7FF20), reinterpret_cast<void*>(hk_DsGetRange));
            g_hkDsGetValuePtr = safetyhook::create_inline(reinterpret_cast<void*>(g_base + 0x1C82970), reinterpret_cast<void*>(hk_DsGetValuePtr));
            TO1_LOG("[GUI2] data-source selector hooks: items=%d range=%d value=%d\n",
                g_hkDsGetItemData ? 1 : 0, g_hkDsGetRange ? 1 : 0, g_hkDsGetValuePtr ? 1 : 0);
        }
        static bool created = false;
        if (!created)
        {
            auto createDs = GameFn<void* (__fastcall*)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(0x1E47930);
            void* ds = createDs(reinterpret_cast<void*>(g_base + 0x949F7A6),
                SID("Menu2::DataSourceSelector"), kDsReticleMode, SID("TO1_RETICLE"), 0, 0);
            void* fpsDs = createDs(reinterpret_cast<void*>(g_base + 0x949F7A6),
                SID("Menu2::DataSourceSelector"), kDsMenuFpsCap, SID("TO1_MENU_FPS_CAP"), 0, 0);
            void* radarDs = createDs(reinterpret_cast<void*>(g_base + 0x949F7A6),
                SID("Menu2::DataSourceSelector"), kDsRadarMode, SID("TO1_MENU_RADAR"), 0, 0);
            created = (ds != nullptr) && (fpsDs != nullptr) && (radarDs != nullptr);
            TO1_LOG("[GUI2] to1-reticle-mode data source: %p, to1-menu-fps-cap: %p, to1-radar-mode: %p\n", ds, fpsDs, radarDs);
        }
    }

    const char* Username() { return net::account::Name()[0] ? net::account::Name() : "Player"; }
    const char* g_teamSymbol = "hud/interactions/marker-scars.png";
    const int   kLobbyRows  = 8;
    const int   kLobbySlots = kLobbyRows; // was winmm_patches.py: lobby8-* (6 patches)
    bool        g_searching = false;
    enum class Lobby { Idle, Found, MapShown, Countdown };
    Lobby       g_lobby = Lobby::Idle;
    int         g_lastTickCd = 0;
    bool        g_connecting = false;
    bool        g_connectDone = false;
    uint64_t    g_assetWaitMs = 0;
    const uint64_t kAssetSettleMs = 500;
    const uint64_t kMatchConnectGraceMs = 30000;

    void fillLobby(const char* fill, bool spin, const net::matchmaking::MatchInfo* m)
    {
        setTxt(nodef("anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", 0), Username());
        setImg(nodef("anim-group.lobbyPanel.lobbyList.Player%d.teamSymbol", 0), g_teamSymbol);
        setVis(nodef("anim-group.lobbyPanel.lobbyList.Player%d.teamSymbol", 0), true);

        int filled = 1; // slot 0 (you) always counts
        if (m) {
            for (int r = 0; r < m->playerCount && filled < kLobbySlots; ++r)
            {
                if (m->roster[r].slot == m->yourSlot) continue;
                setTxt(nodef("anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", filled), m->roster[r].name);
                setVis(nodef("anim-group.lobbyPanel.lobbyList.Player%d.spinner", filled), false);
                setImg(nodef("anim-group.lobbyPanel.lobbyList.Player%d.teamSymbol", filled), g_teamSymbol);
                setVis(nodef("anim-group.lobbyPanel.lobbyList.Player%d.teamSymbol", filled), true);
                ++filled;
            }
        }
        else if (net::matchmaking::GetState() == net::matchmaking::State::Searching) {
            bool skippedSelf = false;
            for (int i = 0; i < net::matchmaking::LobbyNameCount() && filled < kLobbySlots; ++i)
            {
                const char* nm = net::matchmaking::LobbyName(i);
                if (!nm || !nm[0]) continue;
                if (!skippedSelf && strcmp(nm, Username()) == 0) { skippedSelf = true; continue; }
                setTxt(nodef("anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", filled), nm);
                setVis(nodef("anim-group.lobbyPanel.lobbyList.Player%d.spinner", filled), false);
                setVis(nodef("anim-group.lobbyPanel.lobbyList.Player%d.teamSymbol", filled), false);
                ++filled;
            }
        }

        for (int i = filled; i < kLobbySlots; ++i)
        {
            setTxt(nodef("anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", i), fill);
            setVis(nodef("anim-group.lobbyPanel.lobbyList.Player%d.spinner", i), spin);
            setVis(nodef("anim-group.lobbyPanel.lobbyList.Player%d.teamSymbol", i), false);
        }
        for (int i = kLobbySlots; i < kLobbyRows; ++i)
        {
            setTxt(nodef("anim-group.lobbyPanel.lobbyList.Player%d.nameLabel", i), "");
            setVis(nodef("anim-group.lobbyPanel.lobbyList.Player%d.spinner", i), false);
            setVis(nodef("anim-group.lobbyPanel.lobbyList.Player%d.teamSymbol", i), false);
        }

        char cnt[16]; sprintf_s(cnt, "%d / %d", filled, kLobbySlots);
        setTxt(node("anim-group.lobbyPanel.lobbyCount"), cnt);
    }

    void resetLobbyHold()
    {
        g_lobby = Lobby::Idle;
    }

    void showMatchMap()
    {
        const int matchMap = TheMatch().MapIndex();
        setImg(node("anim-group.mapGroup.mapImage"), net::config::Map(matchMap).preview);
        setTxt(node("anim-group.mapGroup.mapName"), net::config::Map(matchMap).name);
        setVis(node("anim-group.mapGroup"), true);
    }

    void abortToMenu()
    {
        net::matchmaking::Cancel();
        g_searching = false;
        resetLobbyHold();
        net::match::Get().Reset(net::match::ResetReason::AbortToMenu);
    }

    // backpack (in match purchases)
    // Row 0 buys primary ammo, row 1 secondary ammo. Row 2 buys the slotted purchasable, then becomes an ammo row for it.
    constexpr int kBackpackRows = 3;
    constexpr gameplay::weapons::Slot kBackpackSlot[kBackpackRows] = {
        gameplay::weapons::Slot::Primary,
        gameplay::weapons::Slot::Secondary,
        gameplay::weapons::Slot::Purchasable,
    };

    bool backpackRowBuysWeapon(gameplay::weapons::Slot s)
    {
        return s == gameplay::weapons::Slot::Purchasable && !gameplay::purchase::PurchasableOwned();
    }

    int backpackRowSlots(gameplay::weapons::Slot* out)
    {
        using namespace gameplay;
        static constexpr weapons::Slot kOrder[] = {
            weapons::Slot::Primary,
            weapons::Slot::Secondary,
            weapons::Slot::Purchasable,
        };
        int n = 0;
        for (weapons::Slot s : kOrder)
            if (loadout::IsEquipped(s)) out[n++] = s;
        return n;
    }

    void refreshBackpackRows()
    {
        using namespace gameplay;

        char b[128];
        sprintf_s(b, "|coins_small| %d", purchase::PartsBalance());
        setTxt(node("anim-group.partsCount"), b);

        weapons::Slot rows[kBackpackRows];
        const int n = backpackRowSlots(rows);

        for (int i = 0; i < kBackpackRows; ++i)
        {
            const bool used = (i < n);
            setVis(nodef("anim-group.bpList.Item%d", i), used);
            setVis(nodef("anim-group.rowDeco.icon%d", i), used);
            setVis(nodef("anim-group.rowDeco.cost%d", i), used);
            if (!used) {
                setVis(nodef("anim-group.rowDeco.ammoCount%d", i), false);
                continue;
            }

            const weapons::Slot s = rows[i];
            const weapons::View w = loadout::Selected(s);
            const bool buyWeapon = backpackRowBuysWeapon(s);
            const uint32_t cost = buyWeapon ? w.buyCost : w.ammoCost;
            const bool full = !buyWeapon && purchase::AmmoFull(s);
            const bool afford = purchase::CanAfford(static_cast<int>(cost));
            const char* lock = (full || !afford) ? "|lock| " : "";

            char label[160];
            if (buyWeapon)  sprintf_s(label, "%s%s", lock, w.name);
            else if (full)  sprintf_s(label, "%s%s AMMO (FULL)", lock, w.name);
            else            sprintf_s(label, "%sx%u %s AMMO", lock, w.ammoAmount, w.name);

            setImg(nodef("anim-group.rowDeco.icon%d", i), w.icon);
            setTxt(nodef("anim-group.bpList.Item%d.label0", i), label);

            sprintf_s(b, "|coins_small| %u", cost);
            setTxt(nodef("anim-group.rowDeco.cost%d", i), b);

            const int carried = buyWeapon ? -1 : purchase::CurrentAmmo(s);
            setVis(nodef("anim-group.rowDeco.ammoCount%d", i), carried >= 0);
            if (carried >= 0) {
                sprintf_s(b, "%d", carried);
                setTxt(nodef("anim-group.rowDeco.ammoCount%d", i), b);
            }
        }
    }
    void fillBackpackDesc(int row)
    {
        using namespace gameplay;

        weapons::Slot rows[kBackpackRows];
        const int n = backpackRowSlots(rows);

        if (row < 0 || row >= n) {
            setTxt(node("anim-group.descName"), "");
            setTxt(node("anim-group.descText"), "");
            return;
        }

        const weapons::Slot s = rows[row];
        const weapons::View w = loadout::Selected(s);
        setTxt(node("anim-group.descName"), w.name);
        setTxt(node("anim-group.descText"), backpackRowBuysWeapon(s) ? w.desc : w.ammoDesc);
    }

    void backpackMenu(bool entered)
    {
        using namespace gameplay;

        static StringId64 s_lastFocus = 0;
        static bool s_prevSelect = false;
        static int s_lastBalance = -1;

        if (entered) {
            refreshBackpackRows();
            s_lastFocus = 0;
            s_prevSelect = true;
            s_lastBalance = purchase::PartsBalance();
        }

        // A kill while the backpack is open changes what you can afford.
        if (purchase::PartsBalance() != s_lastBalance) {
            s_lastBalance = purchase::PartsBalance();
            refreshBackpackRows();
        }

        const StringId64 f = focused();
        if (f != s_lastFocus) {
            s_lastFocus = f;
            int row = -1;
            for (int i = 0; i < kBackpackRows; ++i)
                if (f == nodef("anim-group.bpList.Item%d", i)) { row = i; break; }
            fillBackpackDesc(row);
        }

        const bool sel = pressed(SID("menu-select"));
        const bool justPressed = sel && !s_prevSelect;
        s_prevSelect = sel;
        if (!justPressed) return;

        weapons::Slot rows[kBackpackRows];
        const int n = backpackRowSlots(rows);

        for (int i = 0; i < n; ++i)
        {
            if (!selectTarget(nodef("anim-group.bpList.Item%d", i))) continue;

            const weapons::Slot s = rows[i];
            const bool ok = backpackRowBuysWeapon(s) ? purchase::BuyWeapon()
                : purchase::BuyAmmo(s);
            sound(ok ? SID("sfx-rl-hub-interact-vendor-purchase-complete")
                : SID("sfx-rl-round-timer-tick"));

            s_lastBalance = purchase::PartsBalance();
            refreshBackpackRows();
            fillBackpackDesc(i);
            return;
        }
    }

    void picker(gameplay::weapons::Slot slot, bool entered)
    {
        using namespace gameplay;
        const int count = weapons::Count(slot);

        auto refreshRows = [&]() {
            const int sel = loadout::SelectedIndex(slot);

            for (int i = 0; i < kPickerRows; ++i)
            {
                const bool used = (i < count);
                setVis(nodef("anim-group.pickerList.Item%d", i), used);
                setVis(nodef("anim-group.rowDeco.cost%d", i), used);
                setVis(nodef("anim-group.rowDeco.icon%d", i), used);
                if (!used) continue;

                const weapons::View w = weapons::Get(slot, i);
                setImg(nodef("anim-group.rowDeco.icon%d", i), w.icon);
                setTxt(nodef("anim-group.pickerList.Item%d.label0", i), w.name);

                char cost[40];
                if (i == sel)                                sprintf_s(cost, "EQUIPPED");
                else if (!loadout::CanSelectWeapon(slot, i)) sprintf_s(cost, "%u LP |lock|", w.lpCost);
                else                                         sprintf_s(cost, "%u LP", w.lpCost);
                setTxt(nodef("anim-group.rowDeco.cost%d", i), cost);
            }
            setLpTotal();
            };

        if (entered)
        {
            setTxt(node("anim-group.chrome.menuTitle"), "GEAR UP");
            const int sel = loadout::SelectedIndex(slot);
            for (int i = 0; i < count; ++i)
                setVis(nodef("anim-group.pickerList.Item%d.selectedMark", i), i == sel);
            refreshRows();
        }

        if (!pressed(SID("menu-select"))) return;

        for (int i = 0; i < count; ++i)
        {
            if (!selectTarget(nodef("anim-group.pickerList.Item%d", i))) continue;

            const int prev = loadout::SelectedIndex(slot);

            if (i == prev)
            {
                if (!loadout::SelectWeapon(slot, weapons::kEmpty)) {
                    sound(SID("sfx-rl-round-timer-tick"));
                    return;
                }
                sound(SID("sfx-rl-hub-interact-vendor-purchase-complete"));
                setVis(nodef("anim-group.pickerList.Item%d.selectedMark", prev), false);
                refreshRows();
                return;
            }

            if (!loadout::SelectWeapon(slot, i)) {
                sound(SID("sfx-rl-round-timer-tick")); // over budget
                return;
            }

            // ext GuListSelect: a mid-match pick is applied at the next respawn and
            // takes g_loState from LO_MATCH back to LO_NONE.
            if (net::match::liveness::Live())
                net::ui::postmatch::NoteLoadoutApplied();

            sound(SID("sfx-rl-hub-interact-vendor-purchase-complete"));
            if (prev >= 0) setVis(nodef("anim-group.pickerList.Item%d.selectedMark", prev), false);
            setVis(nodef("anim-group.pickerList.Item%d.selectedMark", i), true);
            refreshRows();
            return;
        }
    }

    void loadoutPage()
    {
        setTxt(node("anim-group.chrome.menuTitle"), "GEAR UP");

        static const struct { gameplay::weapons::Slot slot; const char* node; } kWeaponSlots[] = {
            { gameplay::weapons::Slot::Primary,     "PrimaryWeapon"   },
            { gameplay::weapons::Slot::Secondary,   "SecondaryWeapon" },
            { gameplay::weapons::Slot::Purchasable, "Purchasable1"    },
        };

        for (const auto& ws : kWeaponSlots)
        {
            char b[160], cost[32];

            const bool slotUsed = gameplay::weapons::Count(ws.slot) > 0;
            sprintf_s(b, "anim-group.slotList.%s", ws.node);
            setVis(node(b), slotUsed);
            if (!slotUsed) continue;

            const bool has = gameplay::loadout::IsEquipped(ws.slot);
            const gameplay::weapons::View w = gameplay::loadout::Selected(ws.slot);

            sprintf_s(b, "anim-group.slotList.%s.weaponIcon", ws.node);
            setImg(node(b), has ? w.icon : "hud/weapon-cross/empty-slot.svg");
            sprintf_s(b, "anim-group.slotList.%s.weaponName", ws.node);
            setTxt(node(b), has ? w.name : "NOTHING");
            sprintf_s(cost, "%u LP", w.lpCost);
            sprintf_s(b, "anim-group.slotList.%s.slotCost", ws.node);
            setTxt(node(b), cost);
        }

        for (int i = 0; i < kSkillSlotCount; ++i)
        {
            const bool used = (i < gameplay::kMaxEquippedPerks);
            setVis(skillSlotNode(i), used);
            if (!used) continue;

            const gameplay::Perk* p = gameplay::perks::Find(gameplay::perks::EquippedAt(i));

            char cost[32];

            setImg(skillSlotNode(i, "weaponIcon"), p ? p->icon : "hud/weapon-cross/empty-slot.svg");
            setTxt(skillSlotNode(i, "weaponName"), p ? p->name : "NOTHING");
            sprintf_s(cost, "%u LP", p ? p->lpCost : 0u);
            setTxt(skillSlotNode(i, "slotCost"), cost);
        }

        setLpTotal();
    }

    void loadoutMenu(bool entered)
    {
        if (entered) loadoutPage();

        // The menu data pushes picker-skill on its own, so we never see that press.
        // Track which skill slot holds focus instead -- whatever it is when the page
        // changes is the slot the picker will edit.
        const StringId64 f = focused();
        for (int i = 0; i < kSkillSlotCount; ++i)
            if (f == skillSlotNode(i)) { g_editSkillSlot = i; return; }
    }

    void creditsPage(bool entered)
    {
        if (entered) setTxt(node("anim-group.chrome.menuTitle"), "CREDITS");
    }

    int openPatchnotesInBrowser()
    {
        constexpr const char COMMAND[] = "start https://twelve-weeks.com";
        return std::system(COMMAND);
    }

    void mainMenu(bool entered)
    {
        if (entered)
        {
            setImg(node("anim-group.mapGroup.mapImage"), net::config::Map(g_map).preview);
            setTxt(node("anim-group.mapGroup.mapName"), net::config::Map(g_map).name);
            fillLobby("", false, nullptr);
        }

        if (pressed(SID("menu-select")) && selectTarget(node("anim-group.menuList.Quit")))
        {
            net::gui2::ResetMultiplayer();
            DC::Call<"quit-to-main-menu">();
        }

        if (pressed(SID("menu-select")) && selectTarget(node("anim-group.menuList.Patchnotes")))
        {
            openPatchnotesInBrowser();
        }
    }

    void mapSelect(bool entered)
    {
        if (entered)
        {
            setTxt(node("anim-group.chrome.menuTitle"), "MAP SELECTION");
            for (int i = 0; i < kMapRows; ++i)
            {
                const bool used = (i < net::config::MapCount());
                setVis(nodef("anim-group.mapScroll.contents.Item%d", i), used);
                if (!used) continue;

                setImg(nodef("anim-group.mapScroll.contents.Item%d.mapPreview", i), net::config::Map(i).preview);
                setTxt(nodef("anim-group.mapScroll.contents.Item%d.mapLabel", i), net::config::Map(i).name);
                setVis(nodef("anim-group.mapScroll.contents.Item%d.selectedMark", i), i == g_map);
            }
        }
        if (!pressed(SID("menu-select"))) return;
        for (int i = 0; i < net::config::MapCount(); ++i)
        {
            if (!selectTarget(nodef("anim-group.mapScroll.contents.Item%d", i))) continue;
            sound(SID("sfx-rl-hub-interact-vendor-purchase-complete"));
            if (i != g_map)
            {
                setVis(nodef("anim-group.mapScroll.contents.Item%d.selectedMark", g_map), false);
                setVis(nodef("anim-group.mapScroll.contents.Item%d.selectedMark", i), true);
                g_map = i;
            }
            return;
        }
    }

    void connectingPage(bool entered)
    {
        if (entered || net::connect::GetPhase() == net::connect::Phase::Idle)
        {
            net::connect::Begin();
            g_connectDone = false;
        }

        net::connect::Tick();

        setPct(cnode("anim-group.progress"), net::connect::Progress());
        GUI2::SET_WIDGET_TEXT(cnode("anim-group.statusText"), net::connect::StatusText());

        if (!g_connectDone)
        {
            if (net::connect::GetPhase() == net::connect::Phase::Complete)
            {
                DEBUG::PLAY_TASK(SID("to1-levels"), SID("to1-main-menu"), 0);
                SetPhase(Phase::FrontEndLoading);
                g_connectDone = true;
            }
            else if (net::connect::GetPhase() == net::connect::Phase::Failed)
            {
                DC::Call<"menu2-pop-page">(SID("main"), SID("to1/connecting"));
                g_connectDone = true;
            }
        }
    }

    void lockToMenu()
    {
        uint64_t taskFind = DC::Call<"task-find">(SID("to1-levels"), SID("to1-main-menu"));
        if (!DC::Call<"task-subnode-active?">(taskFind)) return;

        DC::Call<"disable-pause-menu/f">();
        DC::Call<"joypad-disable-all-commands-except">(SID("menu-back"), SID("menu-select"), SID("menu-left"),
            SID("menu-up"), SID("menu-right"), SID("menu-down"),
            SID("debug-script-press-x"), SID("menu-select-vc"), 0, 0, 0);
        // CAMERA::SET_CAMERA_POSITION({ -1.066, 1.174, -3.950 }, { -0.0044, 0.0689, -0.0003, -0.9976 });
        //CAMERA::OVERRIDE_FOV(67);
        CAMERA::SET_CAMERA_POSITION({ 165.728, 52.138, -65.630 }, { -0.0533, 0.8940, 0.1107, 0.4309 });
        CAMERA::OVERRIDE_FOV(75);
        DC::Call<"override-render-settings">(SID("*med-rs-med-015-ascent-inside*"), 0, 0, 0, 1000, 0, 1, 0, 1.00f);
    }

    bool ScoreboardIsTop()
    {
        return MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack)
            == TO1_HUD("scoreboard");
    }

    void updatePauseMenu()
    {
        const bool pauseOpen = MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, SID("to1/pause-menu"));

        if (pauseOpen && pressed(SID("menu-select")) &&
            selectTarget(node("anim-group.menuList.leaveGame")))
        {
            net::gui2::LeaveMatch();
            return;
        }

        if (DC::Call<"joypad-command-active?">(SID("pause")) == 0)
            return;

        if (pauseOpen)
            DC::Call<"menu2-pop-page">(SID("hudMenuStack"), SID("to1/pause-menu"));
        else if (!ScoreboardIsTop())
        {
            //DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);   // pop every page first
            MENU2::PUSH_PAGE(MenuStack::hudMenuStack, "to1/pause-menu");
        }
    }

}

namespace net::gui2 {

    StringId64 w_scoreBanner = 0;
    StringId64 w_deathBanner = 0;
    StringId64 w_currencyHUD = 0;
    StringId64 w_waiting = 0;   // "WAITING FOR PLAYERS"

    static bool s_deathBannerWanted = false;
    static bool s_deathBannerSuppressed = false;   // native death screen is up
    static bool s_hudHiddenForPause = false;
    static bool s_nativeHudCovered = false;

    using net::match::kStartLives;
    using net::match::EndSeq;

    static constexpr uint64_t kScoreboardHoldMs = 10000;

    static int MyTeamIdx() { const int t = net::matchmaking::MyTeam(); return (t == 1) ? 1 : 0; }

    void SetLives(int blueLives, int redLives)
    {
        TheMatch().teamLives[MyTeamIdx()]     = blueLives;
        TheMatch().teamLives[1 - MyTeamIdx()] = redLives;
    }
    int  GetBlueLives() { return TheMatch().BlueLives(); }
    int  GetRedLives()  { return TheMatch().RedLives(); }

    void DecrementLivesForTeam(int absTeam) { TheMatch().DecrementLivesForTeam(absTeam); }
    int  GetLivesForTeam(int absTeam)       { return TheMatch().LivesForTeam(absTeam); }
    void SetLivesAbsolute(int t0, int t1)   { TheMatch().SetLivesAbsolute(t0, t1); }

    using net::match::PlayerStats;

    static const PlayerStats& StatsFor(int slot)
    {
        static const PlayerStats kEmpty;
        return (slot >= 0 && slot < kMaxNetPlayers) ? TheMatch().stats[slot] : kEmpty;
    }

    void RecordKill(int killerSlot, int victimSlot, bool headshot)
    {
        if (killerSlot >= 0 && killerSlot < kMaxNetPlayers) {
            TheMatch().stats[killerSlot].kills++;
            net::replication::AwardParts(100, killerSlot, "Downed Enemy");
            //net::replication::AwardParts(50, assistSlot, "Assist");
            if (headshot) net::replication::AwardParts(30, killerSlot, "Headshot");;
        }
        if (victimSlot >= 0 && victimSlot < kMaxNetPlayers)
            TheMatch().stats[victimSlot].deaths++;
    }

    using net::match::kMatchSeconds;

    void StartMatch() {
        TheMatch().matchTimer.Start(kMatchSeconds);
        SetLives(kStartLives, kStartLives);
        for (auto& st : TheMatch().stats) st = PlayerStats{};
        gameplay::purchase::OnMatchLive();
    }
    void         ClearMatch() { TheMatch().matchTimer.Clear(); }
    void         TickMatch(float dt) { TheMatch().TickMatch(dt); }
    bool         MatchRunning() { return TheMatch().MatchRunning(); }
    bool         MatchExpired() { return TheMatch().MatchExpired(); }
    NetTimerSync MatchSnapshot() { return TheMatch().MatchSnapshot(); }
    void         ApplyMatchSync(const NetTimerSync& s) { TheMatch().ApplyMatchSync(s); }
    bool         InMatch() { return TheMatch().InLiveMatch(); }

    bool PauseMenuOpen()
    {
        return MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, SID("to1/pause-menu"));
    }

    bool MenuUp()
    {
        // ext HUD_MENU_UP() / HudStateTick: in-match, the pause menu anywhere on the
        // stack, or a page pushed above the match's base page. The ext compares the top
        // page against the base page's id (0xebfa61ff33c5e658); that id is not identified
        // in the mod, so this names the in-match menu pages it can instead: the options
        // sub-page and the scoreboard. The backpack / TAB page is a menu that is part of
        // play and is deliberately NOT counted (the ext ignores its page too).
        if (!InMatch())
            return false;
        if (MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, SID("to1/pause-menu")))
            return true;
        const StringId64 top = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
        return top == TO1_HUD("options-menu-ingame") || top == TO1_HUD("scoreboard");
    }

    int PlayerMenuActive()
    {
        // ext PlayerMenuActive(): menu2-player-menu-active? is a TYPED binding -- its rec+8
        // is a descriptor (calling it there crashed the ext), rec+0x10 the impl, invoked
        // with the local player object in RCX and answering in the low byte. 1 = open,
        // 0 = closed, -1 = the native or the player is unavailable.
        const uintptr_t me = ::engine::local_player::Ptr();
        if (!me)
            return -1;
        const uint64_t raw = DC::CallTyped<"menu2-player-menu-active?">(me);
        if (raw == static_cast<uint64_t>(-1))
            return -1;
        return static_cast<int>(raw & 0xff);
    }

    bool BackpackOpen()
    {
        // ext g_tabOpen (feature_menus): the backpack (or its buy page) is open. TAB
        // pushes "to1/backpack-menu" on the HUD menu stack.
        return MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, SID("to1/backpack-menu"));
    }

    bool GearUpOpen()
    {
        // ext gearup13.c: g_gu13Up is set when TopPage() == Gu13PageId(), where
        // PageId("gearup13") hashes "canvas.hudRoot.hudMenuStack.to1/gearup13" -- TO1_HUD.
        // Only the page name is needed here; the gearup13 feature stays unported.
        return MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack) == TO1_HUD("gearup13");
    }

    static void SetDeathBannerWeaponIcon(const char* weaponName);

    void Spawn()
    {
        if (!GUI2::WIDGET_EXISTS(w_scoreBanner)) {
            w_scoreBanner = GUI2::SPAWN_WIDGET(SID("canvas.hudRoot"), "to1/score-banner");
            TO1_LOG("[GUI2] score banner spawned: %llx\n",
                static_cast<unsigned long long>(w_scoreBanner));
        }
        if (!GUI2::WIDGET_EXISTS(w_deathBanner)) {
            w_deathBanner = GUI2::SPAWN_WIDGET(SID("canvas.hudRoot"), "to1/death-banner");
            GUI2::SET_WIDGET_VISIBLE(w_deathBanner, false);   // hidden until you die
            SetDeathBannerWeaponIcon("");
            TO1_LOG("[GUI2] death banner spawned: %llx\n",
                static_cast<unsigned long long>(w_deathBanner));
        }
        if (!GUI2::WIDGET_EXISTS(w_currencyHUD)) {
            w_currencyHUD = GUI2::SPAWN_WIDGET(SID("canvas.hudRoot"), "to1/currency-hud");
            TO1_LOG("[GUI2] currency HUD spawned: %llx\n",
                static_cast<unsigned long long>(w_currencyHUD));
        }
    }

    void Despawn()
    {
        w_scoreBanner = 0;
        w_deathBanner = 0;
        w_currencyHUD = 0;
        w_waiting = 0;
    }

    static uintptr_t ScoreBannerPath(const char* leaf)
    {
        return DC::Call<"gui2-build-widget-path">(w_scoreBanner, MEMORY::ALLOC_STRING("fader"),
            MEMORY::ALLOC_STRING("teamPanel"), MEMORY::ALLOC_STRING(leaf),
            0, 0, 0, 0, 0);
    }

    static void RefreshScoreBanner()
    {
        if (!w_scoreBanner) return;

        char allyText[16], enemyText[16];
        snprintf(allyText, sizeof(allyText), "%d", GetBlueLives());
        snprintf(enemyText, sizeof(enemyText), "%d", GetRedLives());
        if (uintptr_t p = ScoreBannerPath("allyLives"))  GUI2::SET_WIDGET_TEXT(p, allyText);
        if (uintptr_t p = ScoreBannerPath("enemyLives")) GUI2::SET_WIDGET_TEXT(p, enemyText);

        int myTeam = net::matchmaking::MyTeam();
        if (myTeam >= 0) {
            int enemyTeam = (myTeam == MM_TEAM_WLF) ? MM_TEAM_SERAPHITES : MM_TEAM_WLF;
            if (uintptr_t p = ScoreBannerPath("allyLabel"))
                GUI2::SET_WIDGET_TEXT(p, MMTeamName(static_cast<uint8_t>(myTeam)));
            if (uintptr_t p = ScoreBannerPath("enemyLabel"))
                GUI2::SET_WIDGET_TEXT(p, MMTeamName(static_cast<uint8_t>(enemyTeam)));
        }

        float secs = TheMatch().matchTimer.Duration() > 0.0f ? TheMatch().matchTimer.Remaining() : kMatchSeconds;
        int   total = static_cast<int>(ceilf(secs));
        char  matchText[16];
        snprintf(matchText, sizeof(matchText), "%d:%02d", total / 60, total % 60);
        if (uintptr_t p = ScoreBannerPath("matchTimer")) GUI2::SET_WIDGET_TEXT(p, matchText);
    }

    static uintptr_t CurrencyHudPath(const char* leaf)
    {
        return DC::Call<"gui2-build-widget-path">(w_currencyHUD, MEMORY::ALLOC_STRING("currencyGroup"),
            MEMORY::ALLOC_STRING("partsRow"), MEMORY::ALLOC_STRING(leaf),
            0, 0, 0, 0, 0);
    }

    static void RefreshCurrencyHUD()
    {
        if (!w_currencyHUD) return;
        char parts[16];
        snprintf(parts, sizeof(parts), "%d", gameplay::purchase::PartsBalance());
        if (uintptr_t p = CurrencyHudPath("partsAmount")) GUI2::SET_WIDGET_TEXT(p, parts);
    }

    static uintptr_t DeathBannerPath(const char* leaf)
    {
        return DC::Call<"gui2-build-widget-path">(w_deathBanner, MEMORY::ALLOC_STRING("fader"),
            MEMORY::ALLOC_STRING("banner"), MEMORY::ALLOC_STRING(leaf),
            0, 0, 0, 0, 0);
    }

    struct DeathBannerIconLeaf { const char* weapon; const char* leaf; };
    constexpr DeathBannerIconLeaf kDeathBannerIconLeaves[] = {
        { "bow-ellie",                  "w_bow"     },
        { "crossbow",                   "w_xbow"    },
        { "flamethrower",               "w_flame"   },
        { "pistol-beretta",             "w_beretta" },
        { "pistol-glock",               "w_glock"   },
        { "pistol-revolver-taurus",     "w_taurus"  },
        { "pistol-xcaliber",            "w_xcal"    },
        { "rifle-assault-firefly",      "w_firefly" },
        { "rifle-galil",                "w_galil"   },
        { "rifle-mpx5",                 "w_mpx5"    },
        { "rifle-remington-bolt",       "w_rembolt" },
        { "rifle-vepr",                 "w_vepr"    },
        { "shotgun-baserri-over-under", "w_baserri" },
        { "shotgun-remington-pump",     "w_rempump" },
    };

    static void SetDeathBannerWeaponIcon(const char* weaponName)
    {
        auto getSystem  = GameFn<uintptr_t(__fastcall*)(int)>(0x1270A80);
        auto findWidget = GameFn<uintptr_t(__fastcall*)(uintptr_t, uint64_t)>(0x1E8F190);
        auto setAlpha   = GameFn<void(__fastcall*)(uintptr_t, float)>(0x1E5B4D0);
        const uintptr_t gui2System = getSystem(23);
        for (const auto& entry : kDeathBannerIconLeaves) {
            const uintptr_t path = DeathBannerPath(entry.leaf);
            const uintptr_t node = path ? findWidget(gui2System, path) : 0;
            const bool name_equals = std::strcmp(weaponName, entry.weapon) == 0;
            if (node)
                setAlpha(node, name_equals ? 1.0f : 0.0f);
            else
                TO1_LOG("[GUI2] death banner: %s lookup failed (path=%llx)\n",
                    entry.leaf, static_cast<unsigned long long>(path));
        }
    }

    void SetDeathBannerKiller(const char* killerName, const char* weaponName)
    {
        if (!GUI2::WIDGET_EXISTS(w_deathBanner)) return;

        if (uintptr_t p = DeathBannerPath("killerNameText"))
            GUI2::SET_WIDGET_TEXT(p, killerName);

        SetDeathBannerWeaponIcon(weaponName);
    }

    void SetDeathBannerRespawn(int seconds)
    {
        if (!GUI2::WIDGET_EXISTS(w_deathBanner)) return;
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", seconds > 0 ? seconds : 0);
        if (uintptr_t p = DeathBannerPath("respawnNumber"))
            GUI2::SET_WIDGET_TEXT(p, buf);
    }

    void SetDeathBannerVisible(bool visible)
    {
        s_deathBannerWanted = visible;
        if (!GUI2::WIDGET_EXISTS(w_deathBanner)) return;
        GUI2::SET_WIDGET_VISIBLE(w_deathBanner,
            visible && !s_deathBannerSuppressed && !PauseMenuOpen());
    }

    void SetDeathBannerSuppressed(bool suppressed)
    {
        if (s_deathBannerSuppressed == suppressed) {
            // The native screen re-asserts every frame; the widget itself is kept up
            // to date by UpdateHudPauseVisibility's own latch path too.
            return;
        }
        s_deathBannerSuppressed = suppressed;
        if (!GUI2::WIDGET_EXISTS(w_deathBanner)) return;
        GUI2::SET_WIDGET_VISIBLE(w_deathBanner,
            s_deathBannerWanted && !suppressed && !PauseMenuOpen());
    }

    void RefreshWidgets()
    {
        RefreshScoreBanner();
        RefreshCurrencyHUD();
    }

    static constexpr int kScoreboardRows = 4;

    static void sbSet(int team, int row, const char* field, const char* text)
    {
        char leaf[80];
        snprintf(leaf, sizeof(leaf), "anim-group.scoreboard.team%d_row%d_%s", team, row, field);
        setTxt(node(leaf), text);
    }

    static constexpr const char* kMnkInputSvg = "pc/menu/keyboard-mouse.svg";
    static constexpr const char* kPadInputSvg = "pc/hud/view-controls/dualsense-tab.svg";

    static void sbSetImg(int team, int row, const char* field, const char* path)
    {
        char leaf[80];
        snprintf(leaf, sizeof(leaf), "anim-group.scoreboard.team%d_row%d_%s", team, row, field);
        setImg(node(leaf), path);
    }

    static void sbSetVis(int team, int row, const char* field, bool vis)
    {
        char leaf[80];
        snprintf(leaf, sizeof(leaf), "anim-group.scoreboard.team%d_row%d_%s", team, row, field);
        setVis(node(leaf), vis);
    }

    static void FillScoreboardTeam(int displayTeam, int realTeam)
    {
        NetPlayer* members[kMaxNetPlayers];
        int n = 0;
        auto consider = [&](NetPlayer& pl) {
            if (n >= kMaxNetPlayers) return;
            const int rowTeam = (&pl == NetPlayers::Local())
                ? net::matchmaking::MyTeam()
                : TheMatch().TeamOf(pl.netSlot);
            if (rowTeam != realTeam) return;
            members[n++] = &pl;
            };
        if (NetPlayer* me = NetPlayers::Local()) consider(*me);
        for (NetPlayer& r : NetPlayers::Remotes()) consider(r);

        for (int i = 1; i < n; ++i) {
            NetPlayer* key = members[i];
            const int keyParts = StatsFor(key->netSlot).parts;
            int j = i - 1;
            while (j >= 0 && StatsFor(members[j]->netSlot).parts < keyParts) {
                members[j + 1] = members[j];
                --j;
            }
            members[j + 1] = key;
        }

        int row = 0;
        auto put = [&](NetPlayer& pl) {
            if (row >= kScoreboardRows) return;
            const PlayerStats& st = StatsFor(pl.netSlot);
            char buf[16];
            sbSet(displayTeam, row, "name", pl.name);
            snprintf(buf, sizeof(buf), "%d", st.parts);  sbSet(displayTeam, row, "parts", buf);
            snprintf(buf, sizeof(buf), "%d", st.kills);  sbSet(displayTeam, row, "downs", buf);
            snprintf(buf, sizeof(buf), "%d", st.kills);  sbSet(displayTeam, row, "executions", buf);
            snprintf(buf, sizeof(buf), "%d", st.deaths); sbSet(displayTeam, row, "deaths", buf);
            sbSet(displayTeam, row, "revives", "0");

            const bool isLocal = (&pl == NetPlayers::Local());
            if (isLocal || pl.hasState) {
                const bool mnk = isLocal
                    ? (DC::Call<"is-mnk-primary-input-device?">() ? true : false)
                    : (pl.State().isMnk != 0);
                sbSetImg(displayTeam, row, "input", mnk ? kMnkInputSvg : kPadInputSvg);
                sbSetVis(displayTeam, row, "input", true);
            }
            else {
                sbSetVis(displayTeam, row, "input", false);
            }

            int ping = -1;
            if (pl.netSlot == 0)                 ping = 0;
            else if (isLocal)                    ping = NetPlayers::BySlot(0).PingMs();
            else if (net::session::IsHost())     ping = pl.PingMs();
            else                                 ping = pl.ReportedPingMs();
            if (ping >= 0) snprintf(buf, sizeof(buf), "%dms", ping);
            else           buf[0] = '\0';
            sbSet(displayTeam, row, "ping", buf);
            ++row;
            };
        for (int i = 0; i < n; ++i) put(*members[i]);

        for (; row < kScoreboardRows; ++row) {
            sbSet(displayTeam, row, "name", "");
            sbSet(displayTeam, row, "parts", "");
            sbSet(displayTeam, row, "downs", "");
            sbSet(displayTeam, row, "executions", "");
            sbSet(displayTeam, row, "deaths", "");
            sbSet(displayTeam, row, "revives", "");
            sbSet(displayTeam, row, "ping", "");
            sbSetVis(displayTeam, row, "input", false);
        }
    }

    static void RefreshScoreboard()
    {
        const int myTeam = net::matchmaking::MyTeam();
        if (myTeam < 0) return;
        const int enemyTeam = (myTeam == MM_TEAM_WLF) ? MM_TEAM_SERAPHITES : MM_TEAM_WLF;

        setTxt(node("anim-group.scoreboard.team0_name"), MMTeamName(static_cast<uint8_t>(myTeam)));
        setTxt(node("anim-group.scoreboard.team1_name"), MMTeamName(static_cast<uint8_t>(enemyTeam)));

        FillScoreboardTeam(0, myTeam);
        FillScoreboardTeam(1, enemyTeam);
    }

    static void RefreshEmbeddedScoreBanner()
    {
        const bool frozen = ScoreboardShowing();
        const int  blue = frozen ? TheMatch().finalBlueLives : GetBlueLives();
        const int  red = frozen ? TheMatch().finalRedLives : GetRedLives();

        char allyText[16], enemyText[16];
        snprintf(allyText, sizeof(allyText), "%d", blue);
        snprintf(enemyText, sizeof(enemyText), "%d", red);
        setTxt(node("anim-group.scoreBanner.fader.teamPanel.allyLives"), allyText);
        setTxt(node("anim-group.scoreBanner.fader.teamPanel.enemyLives"), enemyText);

        const int myTeam = net::matchmaking::MyTeam();
        if (myTeam >= 0) {
            const int enemyTeam = (myTeam == MM_TEAM_WLF) ? MM_TEAM_SERAPHITES : MM_TEAM_WLF;
            setTxt(node("anim-group.scoreBanner.fader.teamPanel.allyLabel"), MMTeamName(static_cast<uint8_t>(myTeam)));
            setTxt(node("anim-group.scoreBanner.fader.teamPanel.enemyLabel"), MMTeamName(static_cast<uint8_t>(enemyTeam)));
        }

        float secs = frozen ? TheMatch().finalSeconds
            : (TheMatch().matchTimer.Duration() > 0.0f ? TheMatch().matchTimer.Remaining() : kMatchSeconds);
        int   total = static_cast<int>(ceilf(secs));
        char  matchText[16];
        snprintf(matchText, sizeof(matchText), "%d:%02d", total / 60, total % 60);
        setTxt(node("anim-group.scoreBanner.fader.teamPanel.matchTimer"), matchText);
    }

    static void RefreshResultText()
    {
        const int myTeam = net::matchmaking::MyTeam();
        const int finalWinner = TheMatch().finalWinner;
        int result;
        if (finalWinner >= 0 && myTeam >= 0) {
            result = (finalWinner == MM_WINNER_DRAW) ? 0
                   : (finalWinner == myTeam)         ? 1
                   :                                   -1;
        } else {
            result = (TheMatch().finalBlueLives > TheMatch().finalRedLives) ? 1
                   : (TheMatch().finalBlueLives < TheMatch().finalRedLives) ? -1
                   : 0;
        }
        setTxt(node("anim-group.resultText"), result == 1 ? "YOU WON" : result == -1 ? "YOU LOST" : "DRAW");

        if (result == 1)
        {
            AUDIO::PLAY_SOUND(SID("sfx-rl-round-end-win"));
        }
        else if (result == -1)
        {
            AUDIO::PLAY_SOUND(SID("sfx-rl-round-end-lose"));
        }
    }

    void UpdateHudPauseVisibility()
    {
        const bool paused = PauseMenuOpen();
        if (paused == s_hudHiddenForPause) return;
        s_hudHiddenForPause = paused;

        const bool show = !paused;
        if (GUI2::WIDGET_EXISTS(w_scoreBanner)) GUI2::SET_WIDGET_VISIBLE(w_scoreBanner, show);
        if (GUI2::WIDGET_EXISTS(w_currencyHUD)) GUI2::SET_WIDGET_VISIBLE(w_currencyHUD, show);
        if (GUI2::WIDGET_EXISTS(w_deathBanner)) GUI2::SET_WIDGET_VISIBLE(w_deathBanner, s_deathBannerWanted && show && !s_deathBannerSuppressed);
    }

    constexpr uint32_t kHud2RootGetRva = 0x1F21500;
    constexpr uint32_t kHud2RootGetHealthAndAmmoRva = 0xF79F90;
    constexpr uintptr_t kGui2WidgetSetVisibleVtblOffset = 744;

    static bool NativeHudCovered()
    {
        const StringId64 top = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
        return top == TO1_HUD("scoreboard")
            || top == TO1_HUD("backpack-menu")
            || top == TO1_HUD("pause-menu")
            || top == TO1_HUD("options-menu-ingame");
    }

    static void SetNativeHealthAmmoVisible(bool visible)
    {
        const uintptr_t root = GameFn<uintptr_t(__fastcall*)()>(kHud2RootGetRva)();
        if (!root) return;
        const uintptr_t widget = GameFn<uintptr_t(__fastcall*)(uintptr_t)>(kHud2RootGetHealthAndAmmoRva)(root);
        if (!widget) return;
        const uintptr_t vtable = *reinterpret_cast<uintptr_t*>(widget);
        auto setVisible = reinterpret_cast<void(__fastcall*)(uintptr_t, bool)>(
            *reinterpret_cast<uintptr_t*>(vtable + kGui2WidgetSetVisibleVtblOffset));
        setVisible(widget, visible);
    }

    static void ReleaseNativeHudCover()
    {
        if (!s_nativeHudCovered) return;
        s_nativeHudCovered = false;
        SetNativeHealthAmmoVisible(true);
    }

    void UpdateNativeHudCover()
    {
        if (!NativeHudCovered()) {
            ReleaseNativeHudCover();
            return;
        }
        s_nativeHudCovered = true;
        SetNativeHealthAmmoVisible(false);
    }

    void ResetHudLatches()
    {
        s_deathBannerWanted = false;
        s_deathBannerSuppressed = false;
        s_hudHiddenForPause = false;
        ReleaseNativeHudCover();
    }

    // ---------------------------------------------------------------- Esc/Start end-scoreboard peek (ported from tools/ext/escscore.c)
    // During the end scoreboard, Esc or pad Start/Options toggles the scoreboard
    // anim-group visibility so the player can peek at the world behind it. The end
    // clock keeps running. Off: t2r-no-end-peek.txt.
    namespace {
        bool g_endPeek = false;
        bool g_endPeekWas = false;
        uint64_t g_endPeekProbeAt = 0;
        int g_endPeekProbeIdx = 0;
        const char* g_endPeekPadCmd = nullptr;

        bool EndPeekOff()
        {
            return !engine::flag::Absent("t2r-no-end-peek.txt");
        }

        void EndPeekSetScoreboard(bool show)
        {
            const StringId64 w = SID("canvas.hudRoot.hudMenuStack.to1/scoreboard.anim-group");
            DC::Call<"gui2-set-widget-visible">(w, show);
            DC::Call<"gui2-set-widget-alpha">(w, show ? 1.0f : 0.0f);
        }

        void EndPeekReset(bool showScoreboard)
        {
            g_endPeek = false;
            g_endPeekWas = false;
            if (showScoreboard)
                EndPeekSetScoreboard(true);
        }

        struct EndPeekXInputState {
            DWORD dwPacketNumber;
            WORD  wButtons;
            BYTE  bLeftTrigger;
            BYTE  bRightTrigger;
            SHORT sThumbLX;
            SHORT sThumbLY;
            SHORT sThumbRX;
            SHORT sThumbRY;
        };

        bool EndPeekPadState(WORD* buttons, BYTE* lt, BYTE* rt, SHORT* lx, SHORT* ly)
        {
            using Fn = DWORD(WINAPI*)(DWORD, EndPeekXInputState*);
            static Fn xs = nullptr;
            static bool tried = false;
            if (!tried) {
                tried = true;
                HMODULE m = GetModuleHandleA("xinput1_4.dll");
                if (!m) m = LoadLibraryA("xinput1_4.dll");
                if (m) xs = (Fn)GetProcAddress(m, "XInputGetState");
            }
            if (!xs || !engine::window::Focused())
                return false;
            static EndPeekXInputState st{};
            static uint64_t frame_read = ~0ULL, next_probe = 0;
            static int have = 0, slot = -1;
            const uint64_t frame = engine::frame::Count();
            if (frame_read != frame) {
                frame_read = frame;
                const uint64_t now = GetTickCount64();
                have = 0;
                if (slot >= 0 && xs((DWORD)slot, &st) == 0)
                    have = 1;
                else if (now >= next_probe) {
                    slot = -1;
                    next_probe = now + 2000;
                    for (DWORD u = 0; u < 4 && !have; ++u)
                        if (xs(u, &st) == 0) { have = 1; slot = (int)u; }
                }
                if (!have) slot = slot >= 0 ? -1 : slot;
            }
            if (!have) return false;
            *buttons = st.wButtons; *lt = st.bLeftTrigger; *rt = st.bRightTrigger; *lx = st.sThumbLX; *ly = st.sThumbLY;
            return true;
        }

        bool EndPeekEdge()
        {
            bool on = engine::window::KeyDown(VK_ESCAPE);

            if (!on) {
                WORD pb = 0; BYTE lt = 0, rt = 0; SHORT lx = 0, ly = 0;
                if (EndPeekPadState(&pb, &lt, &rt, &lx, &ly) && (pb & (0x0010 | 0x0020)))
                    on = true; // XInput Start / Back
            }

            if (!on) {
                if (g_endPeekPadCmd) {
                    on = engine::pad::Held(g_endPeekPadCmd);
                } else {
                    static const char* const kNames[] = {
                        "pause", "start", "pause-menu", "menu-pause", "options"
                    };
                    const uint64_t now = GetTickCount64();
                    if (now >= g_endPeekProbeAt) {
                        g_endPeekProbeAt = now + 2000;
                        const char* nm = kNames[g_endPeekProbeIdx++ %
                            (int)(sizeof(kNames) / sizeof(kNames[0]))];
                        engine::pad::Watch(nm);
                        engine::pad::ProbeRead(nm);
                        if (engine::pad::Held(nm)) {
                            g_endPeekPadCmd = nm;
                            TO1_LOG("[endpeek] pad button learned from command '%s'\n", nm);
                            on = true;
                        }
                    }
                    for (const char* nm : kNames) {
                        if (g_endPeekPadCmd) break;
                        (void)engine::pad::Watch(nm);
                        if (engine::pad::Held(nm)) {
                            g_endPeekPadCmd = nm;
                            TO1_LOG("[endpeek] pad button learned from command '%s'\n", nm);
                            on = true;
                            break;
                        }
                    }
                }
            }

            const bool edge = on && !g_endPeekWas;
            g_endPeekWas = on;
            return edge;
        }

        void UpdateEndPeek()
        {
            if (TheMatch().endSeq != EndSeq::Scoreboard) {
                if (g_endPeek)
                    EndPeekReset(true);
                return;
            }
            if (EndPeekOff()) {
                if (g_endPeek)
                    EndPeekReset(true);
                return;
            }
            if (EndPeekEdge()) {
                g_endPeek = !g_endPeek;
                TO1_LOG("[endpeek] %s\n", g_endPeek ? "scoreboard hidden" : "scoreboard shown");
            }
            EndPeekSetScoreboard(!g_endPeek);
        }
    }

    void ResetMultiplayer()
    {
        net::matchmaking::Cancel();
        net::connect::Reset();
        g_searching = false;
        g_connecting = false;
        g_connectDone = false;
        SetPhase(Phase::Idle);
        g_lastPage = StringId64(0);
        s_gearWasOpen = false;
        resetLobbyHold();
        SetLives(kStartLives, kStartLives);
        TheMatch().matchTimer.Clear();
        EndPeekReset(false);
        TheMatch().endSeq = EndSeq::Idle;
        Despawn();
        TheMatch().matchArmed = false;
    }

    void LeaveMatch()
    {
        EndPeekReset(false);
        net::match::Get().Reset(net::match::ResetReason::LeaveToMenu);
        Despawn();
        net::matchmaking::Cancel();
        g_searching = false;
        g_lobby = Lobby::Idle;
    }

    bool ScoreboardShowing() { return TheMatch().endSeq == EndSeq::Scoreboard; }

    ReticleMode GetReticleMode()
    {
        const int v = g_reticleValue;
        return ReticleMode((v >= 0 && v < kReticleModeCount) ? v : 0);
    }

    RadarMode GetRadarMode()
    {
        const int val = g_radarValue;
        return RadarMode((val >= 0 && val < kRadarModeCount) ? val : 0);
    }

    void HideAllWidgets()
    {
        if (GUI2::WIDGET_EXISTS(w_scoreBanner)) GUI2::SET_WIDGET_VISIBLE(w_scoreBanner, false);
        if (GUI2::WIDGET_EXISTS(w_currencyHUD)) GUI2::SET_WIDGET_VISIBLE(w_currencyHUD, false);
        if (GUI2::WIDGET_EXISTS(w_deathBanner)) GUI2::SET_WIDGET_VISIBLE(w_deathBanner, false);
    }

    void BeginMatchOver(int winnerAbs)
    {
        if (TheMatch().endSeq != EndSeq::Idle) return;
        g_endPeek = false;
        g_endPeekWas = true; // held key at transition must not trigger a peek edge
        TheMatch().finalBlueLives = GetBlueLives();
        TheMatch().finalRedLives = GetRedLives();
        TheMatch().finalSeconds = TheMatch().matchTimer.Duration() > 0.0f ? TheMatch().matchTimer.Remaining() : kMatchSeconds;
        TheMatch().finalWinner = winnerAbs;
        TheMatch().matchTimer.Stop();

        if (winnerAbs >= 0 && net::session::IsHost() && net::matchmaking::GetMatch())
            net::matchmaking::ReportResult(winnerAbs);

        // Arm the native post-match page latch without touching the wire: the
        // ext still owns the 0xE2 report while it is the loaded bridge, and the
        // award tally was accumulated locally by net::clan::NoteAward() from
        // AwardParts(). ArmPost() itself guards on a living clan (ext PmReport).
        net::clan::ArmPost(static_cast<uint32_t>(StatsFor(net::session::MySlot()).parts));

        HideAllWidgets();
        DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
        MENU2::PUSH_PAGE(MenuStack::hudMenuStack, "to1/scoreboard");
        TheMatch().scoreboardShownMs = GetTickCount64();
        TheMatch().endSeq = EndSeq::Scoreboard;
    }

    bool UpdateMatchEnd()
    {
        if (TheMatch().endSeq == EndSeq::Idle)
            return false;

        RefreshScoreboard();
        RefreshEmbeddedScoreBanner();
        RefreshResultText();
        UpdateEndPeek();

        if (ScoreboardIsTop())
            DC::Call<"joypad-disable-all-commands-except">(SID("move"));

        if (GetTickCount64() - TheMatch().scoreboardShownMs >= kScoreboardHoldMs)
        {
            EndPeekReset(true);
            TheMatch().endSeq = EndSeq::Idle;
            LeaveMatch();
        }
        return true;
    }

    bool g_menuMusicPlaying = false;

    constexpr StringId64 kMenuMusicPlaylist = SID("*music-playlist-t2r-credits-in-menu*");
    const uint64_t kMenuMusicRefireGraceMs = 2000;
    uint64_t g_menuMusicFiredMs = 0;

    void UpdateMenuMusic(bool want)
    {
        if (want != g_menuMusicPlaying)
        {
            g_menuMusicPlaying = want;
            if (want) {
                TO1_LOG("[music] playing main menu music\n");
                AUDIO::PLAY_MUSIC_PLAYLIST(kMenuMusicPlaylist);
                g_menuMusicFiredMs = GetTickCount64();
            }
            else {
                AUDIO::STOP_MUSIC_PLAYLIST();
            }
            return;
        }
        if (!want || GetTickCount64() - g_menuMusicFiredMs < kMenuMusicRefireGraceMs)
            return;

        uint64_t cur = DC::Call<"get-current-music-playlist">();
        if (cur != kMenuMusicPlaylist)
        {
            TO1_LOG("[music] lobby playlist ended (cur=%016llX), re-firing\n", cur);
            AUDIO::PLAY_MUSIC_PLAYLIST(kMenuMusicPlaylist);
            g_menuMusicFiredMs = GetTickCount64();
        }
    }

    void UpdateMenu()
    {
        boot::TickExt(); // was tools/features/ext_loader.py (loads t2r-ext.dll, t2r_tick once/frame)
        UpdateMenuMusic(CurPhase() == Phase::FrontEnd);

        {
            const Phase phase = CurPhase();
            const bool frontEnd = phase == Phase::FrontEndLoading || phase == Phase::FrontEnd;
            engine::framecap::SetLimit(frontEnd && g_menuFpsCapValue ? kFrontEndFps : 0.0f);
        }

        {
            constexpr StringId64 kVersionPages[] = {
                TO1_HUD("main-menu"),
                TO1_HUD("find-match-menu"),
                TO1_HUD("pause-menu"),
                TO1_HUD("options-menu-ingame"),
                TO1_HUD("leaderboard"),
            };
            const StringId64 top = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
            for (StringId64 p : kVersionPages)
                if (top == p) { setTxt(node("anim-group.versionText"), kModVersion); break; }
        }

        if (CurPhase() != Phase::Idle)
            DC::Call<"disable-pause-menu/f">();

            char bmsg[MM_BROADCAST_MAX + 1];
            if (net::matchmaking::TakeBroadcast(bmsg, sizeof(bmsg)) && bmsg[0])
                HUD::SHOW_NOTIFICATION(bmsg, 8.0f);

        if (CurPhase() == Phase::FrontEndLoading)
        {
            lockToMenu();

            if (DC::Call<"task-want-load-levels-fully-spawned?">() &&
                ENTITY::DOES_EXIST(SID("player")) &&
                DC::Call<"get-black-fade">() == 0)   // 0 = screen no longer black
            {
                if (MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, SID("to1/main-menu")))
                {
                    net::match::Get().FinishDeferredHookRemoval();
                    SetPhase(Phase::FrontEnd);
                }
                else
                    MENU2::PUSH_PAGE(MenuStack::hudMenuStack, "to1/main-menu");
            }
            return;
        }

        if (GUI2::WIDGET_EXISTS(w_waiting) && CurPhase() != Phase::Live)
            DC::Call<"joypad-disable-all-commands-except">(SID("debug-cheat"));

        if (CurPhase() == Phase::LoadingPlaceSpawn)
        {
            if (net::matchmaking::Aborted()) { abortToMenu(); return; }
            if (net::matchmaking::GetState() != net::matchmaking::State::Matched) { abortToMenu(); return; }

            if (DC::Call<"task-want-load-levels-fully-spawned?">() &&
                ENTITY::DOES_EXIST(SID("player")) &&
                DC::Call<"get-black-fade">() == 0)   // 0 = screen no longer black
            {
                if (const net::matchmaking::MatchInfo* m = net::matchmaking::GetMatch())
                    net::replication::PlaceLocalAtSpawn(m->yourSpawn);

                if (!GUI2::WIDGET_EXISTS(w_waiting)) {
                    w_waiting = GUI2::SPAWN_WIDGET(SID("canvas.hudRoot"), "to1/waiting-for-players");
                    TO1_LOG("[GUI2] waiting-for-players spawned: %llx\n",
                        static_cast<unsigned long long>(w_waiting));
                }

                net::menu::LoadMpAssets();
                net::hooks::InstallMultiplayerHooks();
                g_assetWaitMs = GetTickCount64();
                SetPhase(Phase::LoadingAssets);
            }
            return;
        }

        if (CurPhase() == Phase::LoadingAssets)
        {
            if (net::matchmaking::Aborted()) { abortToMenu(); return; }
            if (net::matchmaking::GetState() != net::matchmaking::State::Matched) { abortToMenu(); return; }

            if (GetTickCount64() - g_assetWaitMs >= kAssetSettleMs &&
                DC::Call<"task-want-load-levels-fully-spawned?">())
            {
                net::matchmaking::SendReady();
                SetPhase(Phase::LoadingWaitStart);
            }
            return;
        }

        if (CurPhase() == Phase::LoadingWaitStart)
        {
            if (net::matchmaking::Aborted()) { abortToMenu(); return; }
            if (net::matchmaking::GetState() != net::matchmaking::State::Matched) { abortToMenu(); return; }

            if (net::matchmaking::StartReceived())
            {
                if (GUI2::WIDGET_EXISTS(w_waiting))
                    GUI2::SET_WIDGET_VISIBLE(w_waiting, false);

                Spawn();
                StartMatch();
                net::menu::ApplyLocalModel();
                NetPlayers::GiveLocalLoadout();

                net::vox::OnMatchLive();
                net::player::OnMatchLive();

                TheMatch().matchLiveMs = GetTickCount64();
                SetPhase(Phase::Live);
            }
            return;
        }

        if (CurPhase() == Phase::Live)
        {
            UpdateNativeHudCover();

            if (net::matchmaking::GetState() != net::matchmaking::State::Matched) { LeaveMatch(); return; }

            if (net::matchmaking::GetMatch() && !net::session::IsConnected() &&
                GetTickCount64() - TheMatch().matchLiveMs > kMatchConnectGraceMs)
            {
                TO1_LOG("[NET] no relay connection within %llums -> leaving to MP main menu\n",
                    static_cast<unsigned long long>(kMatchConnectGraceMs));
                HUD::SHOW_NOTIFICATION("Could not connect to the match - returning to menu", 6.0f);
                LeaveMatch();
                return;
            }

            if (UpdateMatchEnd())
                return;

            if (net::matchmaking::GetMatch())
            {
                int seraph = 0, wlf = 0;
                NetPlayers::CountActivePerTeam(seraph, wlf);
                if (!TheMatch().matchArmed)
                {
                    if (seraph > 0 && wlf > 0) TheMatch().matchArmed = true;
                }
                else if (seraph == 0 || wlf == 0)
                {
                    TO1_LOG("[NET] a team emptied (SERAPHITES=%d WLF=%d) -> leaving to MP main menu\n", seraph, wlf);
                    LeaveMatch();
                    return;
                }
            }

            static StringId64 s_lastLiveTop = 0;
            const StringId64 liveTop = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
            const bool liveEntered = (liveTop != s_lastLiveTop);
            s_lastLiveTop = liveTop;
            if (liveTop == TO1_HUD("backpack-menu"))
                backpackMenu(liveEntered);

            updatePauseMenu();
            if (PauseMenuOpen()) {
                EnsureReticleDataSource();
                RefreshScoreboard();
                RefreshEmbeddedScoreBanner();
            }
            UpdateHudPauseVisibility();
            return;
        }

        if (CurPhase() == Phase::Idle)
        {
            StringId64 top = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::main);
            bool entered = (top != g_lastPage);
            g_lastPage = top;

            if (top == SID("canvas.menuRoot.main.to1/connecting"))
            {
                connectingPage(entered);
                g_connecting = true;
            }
            else if (g_connecting)
            {
                net::connect::Reset();
                g_connecting = false;
            }
            return;
        }

        if (net::matchmaking::GetState() == net::matchmaking::State::Failed && g_searching)
        {
            ResetMultiplayer();
            DC::Call<"quit-to-main-menu">();
            return;
        }

        if (net::matchmaking::TokenRejected())
        {
            ResetMultiplayer();
            DC::Call<"quit-to-main-menu">();
            return;
        }

        lockToMenu();
        EnsureReticleDataSource();

        net::matchmaking::Connect(Username());

        StringId64 top = MENU2::GET_TOP_PAGE_IN_STACK(MenuStack::hudMenuStack);
        bool entered = (top != g_lastPage);
        g_lastPage = top;

        if (top == TO1_HUD("main-menu") ||
            top == TO1_HUD("find-match-menu")) {
            net::matchmaking::RequestOnlineCount();
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", net::matchmaking::OnlineCount());
            setTxt(node("anim-group.onlineGroup.onlineCount"), buf);
        }

        if (top == TO1_HUD("leaderboard")) {
            if (entered) net::matchmaking::RequestLeaderboard();
            for (int row = 0; row < MM_LEADERBOARD_MAX; ++row) {
                char leaf[96], nameBuf[MM_NAME_LEN + 1] = "", winsBuf[16] = "", lossBuf[16] = "", ratioBuff[16] = "", eloBuff[16] = "";
                if (const MMLeaderEntry* e = net::matchmaking::LeaderboardEntry(row)) {
                    memcpy(nameBuf, e->name, MM_NAME_LEN); nameBuf[MM_NAME_LEN] = 0;
                    snprintf(winsBuf, sizeof(winsBuf), "%u", e->wins);
                    snprintf(lossBuf, sizeof(lossBuf), "%u", e->losses);
                    snprintf(ratioBuff, sizeof(ratioBuff), "%.2f", e->winloss_ratio);
                    snprintf(eloBuff, sizeof(eloBuff), "%u", e->elo);
                    TO1_LOG("%u %u %.2f %u", e->wins, e->losses, e->winloss_ratio, e->elo);
                }
                snprintf(leaf, sizeof(leaf), "anim-group.table.row%d_onlineid", row); setTxt(node(leaf), nameBuf);
                snprintf(leaf, sizeof(leaf), "anim-group.table.row%d_wins", row);     setTxt(node(leaf), winsBuf);
                snprintf(leaf, sizeof(leaf), "anim-group.table.row%d_losses", row);   setTxt(node(leaf), lossBuf);
                snprintf(leaf, sizeof(leaf), "anim-group.table.row%d_ratio", row);    setTxt(node(leaf), ratioBuff);
                snprintf(leaf, sizeof(leaf), "anim-group.table.row%d_elo", row);      setTxt(node(leaf), eloBuff);
            }
        }

        if (MENU2::IS_PAGE_ON_STACK(MenuStack::hudMenuStack, SID("to1/find-match-menu")))
        {
            if (!g_searching)
            {
                if (net::friends13::SearchGate())
                {
                    // A party member's FIND MATCH is a ready toggle, not a search (the
                    // ext's fr_search_gate + modepick -> fr_member_ready_press): toggle
                    // ready, pop the page and never send the search request.
                    // ext MmreqHook also sets g_mmBack on this refusal so requeue's
                    // settle holds for a few frames; the mod's own page pop below is
                    // why the latch's pop seam stays gated.
                    net::ui::postmatch::NoteMmRefused();
                    net::friends13::MemberReadyPress();
                    DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, SID("to1/find-match-menu"));
                }
                else if (net::steam::RefuseFindMatch())
                {
                    // ext MmreqHook: `g_steamOnly && !SteamConnected()` -> g_mmBack = 1
                    // and the refusal is logged; the request still goes out so the
                    // daemon's native panel can show the reason and the page is popped
                    // a few frames later by the g_mmBack handler. The mod has no such
                    // daemon panel and the search page is ours, so show the same text as
                    // a styled toast and pop the page here instead of queueing a player
                    // the helper cannot host. The latch still holds requeue's settle.
                    net::ui::postmatch::NoteSteamRefused();
                    TO1_LOG("find match refused: Steam not connected\n");
                    net::notify::Push(net::notify::NTF_WARN,
                        "Steam is not connected: open Steam, then start the game with Play.bat", 4.0f);
                    DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, SID("to1/find-match-menu"));
                }
                else
                {
                    net::session::SetLocalName(Username());
                    net::matchmaking::FindMatch(Username(), 0, 0,
                        PrimaryWeaponSid(), SecondaryWeaponSid());
                    g_searching = true;
                }
            }

            const bool visible = (top == TO1_HUD("find-match-menu"));

            const net::matchmaking::MatchInfo* m =
                (net::matchmaking::GetState() == net::matchmaking::State::Matched)
                ? net::matchmaking::GetMatch() : nullptr;

            const int phase = net::matchmaking::LobbyPhase();
            const int cd = net::matchmaking::LobbyCountdown();
            // ext loadout flow: phase 2 / secs 100..130 (the daemon's 100 + seconds
            // left) is the CHOOSE LOADOUT stage; leaving it returns g_loState to
            // LO_NONE. The mod's lobby countdown is that same encoded value.
            net::ui::postmatch::NoteCountdownChoose(phase, cd);

            if (!m && g_lobby != Lobby::Idle)
            {
                g_lobby = Lobby::Idle;
            }

            if (m && g_lobby == Lobby::Idle)
            {
                g_lobby = Lobby::Found;
                if (visible) setVis(node("anim-group.mapGroup"), false);
            }

            switch (g_lobby)
            {
            case Lobby::Idle:
                if (visible) {
                    setVis(node("anim-group.mapGroup"), false);
                    if (net::matchmaking::VersionRejected()) {
                        setTxt(node("anim-group.statusLabel"), "NEW UPDATE AVAILABLE. DOWNLOAD THE LATEST UPDATE.");
                        fillLobby("", false, m);
                    }
                    else {
                        setTxt(node("anim-group.statusLabel"), "FINDING MATCH...");
                        fillLobby("SEARCHING", true, m);
                    }
                }
                break;

            case Lobby::Found:
                if (visible) {
                    setTxt(node("anim-group.statusLabel"), "MATCH FOUND");
                    fillLobby("", false, m);
                }
                if (phase >= 1)
                {
                    if (visible) showMatchMap();
                    g_lobby = Lobby::MapShown;
                }
                break;

            case Lobby::MapShown:
                if (visible) {
                    showMatchMap();
                    setTxt(node("anim-group.statusLabel"), "MATCH FOUND");
                    fillLobby("", false, m);
                }
                if (phase >= 2) {
                    g_lastTickCd = 0;
                    g_lobby = Lobby::Countdown;
                }
                break;

            case Lobby::Countdown:
                if (visible) {
                    showMatchMap();
                    fillLobby("", false, m);
                }
                if (cd > 0)
                {
                    if (cd != g_lastTickCd) {
                        sound(SID("sfx-rl-round-timer-tick-small"));
                        g_lastTickCd = cd;
                    }
                    if (visible) {
                        char b[32]; sprintf_s(b, "STARTING MATCH IN %d...", cd);
                        setTxt(node("anim-group.statusLabel"), b);
                    }
                }
                else if (phase == 2)
                {
                    DEBUG::PLAY_TASK(SID("to1-levels"), SID(net::config::Map(TheMatch().MapIndex()).task), 0);
                    SetPhase(Phase::LoadingPlaceSpawn);
                    g_lobby = Lobby::Idle;
                }
                break;
            }

            if (visible && (g_lobby == Lobby::MapShown || g_lobby == Lobby::Countdown))
                DC::Call<"joypad-disable-commands">(SID("menu-back"));
        }
        else if (g_searching)
        {
            net::matchmaking::Cancel();
            g_searching = false;
            resetLobbyHold();
        }

        // ext gearup13.c g_loState edges: opening the GEAR UP list is LO_LIST (or
        // LO_MATCH mid-match), a picker/editor is LO_EDIT, and leaving them is
        // LO_NONE. postmatch::Tick samples the stack as a fallback; the explicit
        // open / edit / applied edges cover what a page name alone cannot tell.
        const bool gearPage = top == TO1_HUD("gearup13") || top == TO1_HUD("loadout-menu")
            || top == TO1_HUD("picker-skill") || top == TO1_HUD("picker-primary")
            || top == TO1_HUD("picker-secondary") || top == TO1_HUD("picker-purchase");
        if (gearPage && !s_gearWasOpen)
            net::ui::postmatch::NoteLoadoutOpen(net::match::liveness::Live());
        else if (!gearPage && s_gearWasOpen)
            net::ui::postmatch::NoteLoadoutClose();
        s_gearWasOpen = gearPage;

        if (top == TO1_HUD("picker-primary"))        { if (entered) net::ui::postmatch::NoteLoadoutEdit(); picker(gameplay::weapons::Slot::Primary, entered); }
        else if (top == TO1_HUD("picker-secondary")) { if (entered) net::ui::postmatch::NoteLoadoutEdit(); picker(gameplay::weapons::Slot::Secondary, entered); }
        else if (top == TO1_HUD("picker-purchase"))  { if (entered) net::ui::postmatch::NoteLoadoutEdit(); picker(gameplay::weapons::Slot::Purchasable, entered); }
        else if (top == TO1_HUD("picker-skill"))     { if (entered) net::ui::postmatch::NoteLoadoutEdit(); skillPicker(entered); }
        else if (top == TO1_HUD("loadout-menu"))     loadoutMenu(entered);
        else if (top == TO1_HUD("map-select"))       mapSelect(entered);
        else if (top == TO1_HUD("credits"))          creditsPage(entered);
        else if (top == TO1_HUD("main-menu"))        mainMenu(entered);
    }

    StringId64 PrimaryWeaponSid() { const auto w = gameplay::loadout::Selected(gameplay::weapons::Slot::Primary);   return w.sid[0] ? SID(w.sid) : 0; }
    StringId64 SecondaryWeaponSid() { const auto w = gameplay::loadout::Selected(gameplay::weapons::Slot::Secondary); return w.sid[0] ? SID(w.sid) : 0; }

    int PrimaryWeaponStartingAmmo() { const auto w = gameplay::loadout::Selected(gameplay::weapons::Slot::Primary);   return w.startingAmmoAmount ? w.startingAmmoAmount : 0; }
    int SecondaryWeaponStartingAmmo() { const auto w = gameplay::loadout::Selected(gameplay::weapons::Slot::Secondary);   return w.startingAmmoAmount ? w.startingAmmoAmount : 0; }
    int PurchasableWeaponStartingAmmo() { const auto w = gameplay::loadout::Selected(gameplay::weapons::Slot::Purchasable);   return w.startingAmmoAmount ? w.startingAmmoAmount : 0; }
}
