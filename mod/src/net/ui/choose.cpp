// Native owner for build-191 CHOOSE YOUR SIDE (tools/ext/choose.c + side_tick in
// t2r-ext.c). The pure stage/focus/tween/intro curves live in choose-core.hpp;
// this file owns the widgets, input, RPC and file seams.
//
// Yields while t2r-ext.dll is loaded. The private PCM clips (choose_intro.pcm,
// choose_vox.pcm), the game's under-black mixer and the final render-camera track
// are not available to a native owner: those calls stay explicit inert seams,
// and the authored camera model is applied here to the five parallax widgets
// instead. No toggle sidecar is ever written; t2r-intro-seen.txt is a watched-
// through data stamp and is the only file this owner creates.

#include "choose.hpp"

#include <winsock2.h>
#include <windows.h>

#include "boot.hpp"
#include "engine/exe_side.hpp"
#include "engine/flag.hpp"
#include "engine/frame.hpp"
#include "engine/locale.hpp"
#include "engine/menu-input.hpp"
#include "log.hpp"             // TO1_LOG must precede engine/script.hpp
#include "engine/script.hpp"
#include "engine/widget.hpp"
#include "engine/window.hpp"
#include "net/base/net-account.hpp"
#include "net/online/clan-state.hpp"
#include "net/online/daemon-rpc.hpp"
#include "net/protocol/net-protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace net::ui::choose {

    namespace {

        using namespace core;

        constexpr size_t kTokenLen = 64;
        constexpr int    kReplyCap = 4096;
        constexpr int    kConnMs   = 2500;
        constexpr int    kIoMs     = 3500;
        constexpr int    kReplyMs  = 4000;

        // choose.c: CHI_ART_MS (the layers / shade / UI stay hidden until the
        // tarp lifts, except on a skip).
        constexpr int64_t kIntroArtMs = 2500;

        // t2r-ext.c CH_OLD: the frames + figures kept for the opt-out look.
        const char* const kOld[] = {
            "glowW", "glowS", "sideTitle", "sideTitleLine", "sideDivider",
            "frameBgW", "frameW", "figW", "frameBgS", "frameS", "figS",
            "sideName", "sideDesc", "sideWarn", "sideHint", "sideHintPad",
            "sideSelectIcon", "sideSelectText"
        };
        const char* const K[2]  = { "W", "S" };
        const char* const N[2]  = { "wlf", "ser" };
        const char* const SIDE_NAME[3] = { "", "WLF", "SERAPHITES" };
        const char* const SIDE_DESC[3] = {
            "",
            "Soldiers who drove FEDRA out of Seattle and took the city for themselves. Disciplined, well armed and dug in around the stadium. They trust training, numbers and firepower, and burn out anyone who threatens their walls.",
            "Followers of the Prophet, living off the land on the islands around the city. They turned their backs on the old world and its machines. Patient, silent and deadly with a bow - they speak in whistles and strike from the trees."
        };
        const char* const INTRO_TEXT[4] = {
            "It's been 25 years since the Outbreak. In Seattle, the Washington Liberation Front and the Seraphites fight over territory, supplies, and lives to grow their size and influence. There is safety in numbers - each faction is trying to rebuild its population by finding supplies.",
            "",
            "Your clan is gone. The last of your survivors starved, sickened or walked away. The war goes on without them - choose a side and start again.",
            "Your clan survived twelve weeks. Others will tell stories about the camp you kept alive. Choose a side for the next fight."
        };
        const char kChWarn[]    = "Your faction choice lasts until your clan dies or survives long enough to win.";
        const char kRcNote[]    = "New look. Choose your side again - your clan keeps its progress.";
        const char kHintOver[]  = "A / D  OR CLICK A SIDE";
        const char kHintFocus[] = "A / D  SWITCH     ESC  BACK";
        const char kSeenFile[]  = "t2r-intro-seen.txt";

        bool ArtDisabled()   { return engine::flag::Present("t2r-no-choose-art.txt"); }
        bool IntroDisabled() { return engine::flag::Present("t2r-no-choose-intro.txt"); }
        const char* ArtTest()
        {
            static char tm[16];
            static uint64_t chk = 0;
            const uint64_t now = engine::frame::T60();
            if (!chk || now - chk > 180) {   // 3 s of wall time, as ch_art_test()
                chk = now;
                tm[0] = 0;
                if (FILE* f = engine::exe_side::Open("t2r-choose-art-test.txt", "r")) {
                    if (std::fscanf(f, "%15s", tm) != 1) tm[0] = 0;
                    std::fclose(f);
                }
            }
            return tm;
        }

        // chi_* state. The lore / end-tag PCM voices and the game's mixer duck
        // have no native producer; the visible part of the cinematic (lines,
        // beats, SKIP, UI fade, tarp, camera height) is driven here.
        struct Intro {
            bool     on = false, skippable = false, seen = false;
            bool     skipping = false, jumped = false, uiOn = false;
            bool     artOn = false, linesOff = false;
            uint64_t t0 = 0, skipAt = 0;
        };

        struct Runtime {
            Page   page;
            Art    art;
            Payoff payoff;
            Intro  intro;
            bool   entered = false;
            bool   rc = false;

            bool   hSel = true, hBack = true, hLeft = true, hRight = true;
            bool   hPrev = true, hNext = true, rbHeld = false;
            int    hintArt = -1, hintOld = -1, hHov = -1;
            int    descTgt = -1, wasTg = -2, wasFocus = -2;
            uint64_t descTAt = 0;

            float    gl[2] = { 0.0f, 0.0f };
            uint64_t glAt = 0;
            uint64_t grainAt = 0;
            uint32_t grain = 91;
            uint64_t sideWaitUntil = 0;
        } g;

        // ---- widget helpers (ext ch_w / ch_a / ui_text / ui_image) ------------

        uint64_t SidePage() { return engine::widget::PageId("clan-side"); }
        uint64_t SideRoot() { return engine::widget::Page("anim-group.side"); }
        bool SidePageUp() { return engine::widget::TopPage() == SidePage() && SideRoot() != 0; }

        uint64_t W(const char* rel) { return engine::widget::Page(rel); }

        void Vis(const char* rel, bool on)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::Visible(w, on);
        }
        void Alpha(const char* rel, float a)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::Alpha(w, ClampA(a));
        }
        // ch_a: a completely invisible widget is 0.003, never 0.
        void ChA(const char* rel, float a)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::Alpha(w, std::max(0.003f, ClampA(a)));
        }
        void Piv(const char* rel, float x, float y)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::Pivot(w, x, y);
        }
        void Scale(const char* rel, float s)
        {
            const uint64_t w = W(rel);
            if (w) engine::widget::Scale(w, s, s);
        }
        void TextRaw(const char* rel, const char* s)
        {
            const uint64_t w = W(rel);
            if (w && s) engine::widget::SetText(w, s);
        }
        void Text(const char* rel, const char* en)
        {
            TextRaw(rel, engine::locale::Tr(en));
        }
        void Image(const char* rel, const char* path)
        {
            const uint64_t w = W(rel);
            if (w && path) engine::widget::SetImage(w, path);
        }
        bool Over(const char* rel, bool arm)
        {
            return engine::widget::MouseOver(W(rel), arm);
        }
        void Sfx(const char* name) { AUDIO::PLAY_SOUND(SID(name)); }

        void PadHint(const char* key, const char* pad, int& dev)
        {
            const int mode = engine::menu_input::PadMode() ? 1 : 0;
            if (dev == mode) return;
            dev = mode;
            Vis(key, mode == 0);
            Vis(pad, mode == 1);
        }

        // ---- the 0xE4 side send: exact ext bytes, expected 0xE1 reply ---------

        bool SendSide(int side)
        {
            sockaddr_in addr{};
            if (!net::daemon::Addr(&addr) || !net::account::HaveToken())
                return false;
            const char* tok = net::account::Token();
            if (std::strlen(tok) != kTokenLen || side < 0 || side > 1)
                return false;
            // [u16 len=67][u8 0xE4][64B token][u8 side+1][u8 caps=0x01].
            // caps is CLAN_CAP_RECHOOSE either way: the daemon ignores it when
            // no re-choice is pending, exactly as the ext builds it.
            uint8_t frame[3 + kTokenLen + 2];
            constexpr uint8_t body = static_cast<uint8_t>(kTokenLen + 2);
            frame[0] = body;
            frame[1] = 0;
            frame[2] = static_cast<uint8_t>(NetDaemonOp::kClanChooseSide);
            std::memcpy(frame + 3, tok, kTokenLen);
            frame[3 + kTokenLen]     = static_cast<uint8_t>(side + 1);
            frame[4 + kTokenLen]     = 0x01;

            char rep[kReplyCap];
            int sent = 0;
            const int got = net::daemon::Rpc(addr, frame, static_cast<int>(sizeof frame),
                                             rep, sizeof rep, kConnMs, kIoMs, kReplyMs, &sent);
            if (got < 3 || !sent)
                return false;
            int total = 0;
            if (net::daemon::FrameState(rep, got, sizeof rep, &total) != 1 || total > got)
                return false;
            if (static_cast<uint8_t>(rep[2]) != static_cast<uint8_t>(NetDaemonOp::kClanStateReply))
                return false;
            net::daemon::MarkAlive();
            return true;
        }

        // ---- side page static fill ------------------------------------------

        void ChiArt(bool on);

        void SideImages()
        {
            for (int k = 0; k < 2; ++k) {
                char p[96], img[64];
                std::snprintf(img, sizeof img, "streaming/t2r/clan/fig-%s.png", N[k]);
                std::snprintf(p, sizeof p, "anim-group.side.fig%s.fig%sBody", K[k], K[k]);
                Image(p, img);
                std::snprintf(p, sizeof p, "anim-group.side.fig%s.fig%sDim", K[k], K[k]);
                Image(p, img);
                std::snprintf(p, sizeof p, "anim-group.intro.intro%s", K[k]);
                Image(p, img);
                std::snprintf(img, sizeof img, "streaming/t2r/clan/glow-%s.png", N[k]);
                std::snprintf(p, sizeof p, "anim-group.side.fig%s.fig%sGlow", K[k], K[k]);
                Image(p, img);
            }
        }

        void OldFocus(int f)
        {
            for (int k = 0; k < 2; ++k) {
                const bool on = k == f;
                char p[96];
                for (const char* part : { "Glow", "Body" }) {
                    std::snprintf(p, sizeof p, "anim-group.side.fig%s.fig%s%s", K[k], K[k], part);
                    Vis(p, on);
                }
                std::snprintf(p, sizeof p, "anim-group.side.fig%s.fig%sDim", K[k], K[k]);
                Vis(p, !on);
                std::snprintf(p, sizeof p, "anim-group.side.frame%s", K[k]);
                Vis(p, on);
                std::snprintf(p, sizeof p, "anim-group.side.frameBg%s", K[k]);
                Vis(p, on);
            }
            const int i = std::max(0, f + 1);
            Text("anim-group.side.sideName", SIDE_NAME[i]);
            Text("anim-group.side.sideDesc", SIDE_DESC[i]);
            Text("anim-group.side.sideWarn", g.rc ? kRcNote : kChWarn);
        }

        // ch_show: reset CH / CHI / CHP and switch between the key art and the
        // 2013 frames + figures.
        void ChShow(uint64_t now)
        {
            const bool art = !ArtDisabled();
            Show(g.art, art, now);
            g.payoff = Payoff{};
            g.intro = Intro{};
            Vis("anim-group.side.chIntro", false);
            Vis("anim-group.side.chIntroFade", false);
            Vis("anim-group.side.chArt.chLayers.chDeck", false);
            Vis("anim-group.side.chUi.chChalk", false);
            Vis("anim-group.side.chArt", art);
            Vis("anim-group.side.chUi", art);
            ChiArt(art);
            for (const char* rel : kOld) {
                char p[96];
                std::snprintf(p, sizeof p, "anim-group.side.%s", rel);
                const bool special = !std::strncmp(rel, "glow", 4)
                                  || !std::strncmp(rel, "fig", 3)
                                  || !std::strncmp(rel, "frame", 5)
                                  || !std::strcmp(rel, "sideHintPad");
                Vis(p, art ? false : !special);
            }
            PadHint("anim-group.side.chHint", "anim-group.side.chHintPad", g.hintArt);
            PadHint("anim-group.side.sideHint", "anim-group.side.sideHintPad", g.hintOld);
            if (!art) {
                OldFocus(-1);
                return;
            }
            const char* tm = ArtTest();
            for (int i = 0; i < 5; ++i) {
                char p[64], img[96];
                std::snprintf(p, sizeof p, "anim-group.side.chArt.chLayers.chL%d", i);
                if (!std::strcmp(tm, "ce"))
                    std::snprintf(img, sizeof img, "streaming/t2r/clanend/ce-lost-ser-L%d.png", i);
                else if (!std::strcmp(tm, "1080"))
                    std::snprintf(img, sizeof img, "streaming/t2r/choose/ce-choose-L%d_1080p.png", i);
                else
                    std::snprintf(img, sizeof img, "streaming/t2r/choose/ce-choose-L%d.png", i);
                Image(p, img);
            }
            Image("anim-group.side.chArt.chShade", "streaming/t2r/choose/ch-shade.png");
            Image("anim-group.side.chUi.chLogoW", "streaming/photo-mode/logos/wlf-wolf-logo.png");
            Image("anim-group.side.chUi.chLogoS", "streaming/photo-mode/logos/scar-logo.png");
            Image("anim-group.side.chUi.chTitleLogo", "streaming/t2r/load/tw-title.png");
            Text("anim-group.side.chUi.chWarn", kChWarn);
            char b[128];
            std::snprintf(b, sizeof b, "<font color=\"#9CC4E8\">%s</font>", engine::locale::Tr("WLF"));
            TextRaw("anim-group.side.chUi.chNameW", b);
            std::snprintf(b, sizeof b, "<font color=\"#E8905A\">%s</font>", engine::locale::Tr("SERAPHITES"));
            TextRaw("anim-group.side.chUi.chNameS", b);
            Alpha("anim-group.side.chUi.chTitleLogo", 1.0f);
            g.art.twa = 1.0f;
            g.art.twt = 0;
        }

        // ---- intro cinematic ---------------------------------------------------

        bool IntroSeen()
        {
            if (FILE* f = engine::exe_side::Open(kSeenFile, "r")) {
                std::fclose(f);
                return true;
            }
            return false;
        }
        void SaveIntroSeen()
        {
            if (FILE* f = engine::exe_side::Open(kSeenFile, "wb")) {
                std::fputs("1", f);
                std::fclose(f);
            }
        }
        int64_t IntroElapsed(const Intro& i, uint64_t now)
        {
            return static_cast<int64_t>(now) - static_cast<int64_t>(i.t0);
        }
        void ChiArt(bool on)
        {
            g.intro.artOn = on;
            for (int i = 0; i < 5; ++i) {
                char p[64];
                std::snprintf(p, sizeof p, "anim-group.side.chArt.chLayers.chL%d", i);
                Vis(p, on);
            }
            Vis("anim-group.side.chArt.chShade", on);
            Vis("anim-group.side.chUi", on);
        }
        void FillIntroLines()
        {
            char p[96], c[128];
            for (int k = 0; k < kIN; ++k) {
                std::snprintf(p, sizeof p, "anim-group.side.chIntro.chIntroL%d", k);
                Vis(p, true);
                Piv(p, 1920.0f, static_cast<float>(kLines[k].cy));
                ChA(p, 0.003f);

                std::snprintf(c, sizeof c, "%s.tx", p);
                Vis(c, !kLines[k].logo);
                if (!kLines[k].logo) Text(c, kLines[k].text);
                for (int j = 0; j < kBeatCount; ++j) {
                    std::snprintf(c, sizeof c, "%s.b%d", p, j);
                    Vis(c, kLines[k].logo);
                    if (kLines[k].logo) Text(c, kBeats[j]);
                }
                std::snprintf(c, sizeof c, "%s.lg", p);
                Vis(c, kLines[k].logo);
                if (kLines[k].logo) Image(c, "streaming/t2r/load/tw-title.png");
                std::snprintf(c, sizeof c, "%s.tl", p);
                Vis(c, kLines[k].logo);
                if (kLines[k].logo) Text(c, ".");
            }
        }
        void IntroBegin(uint64_t now, bool allowSkip)
        {
            Intro& i = g.intro;
            i = Intro{};
            i.on = g.art.on && !IntroDisabled();
            if (!i.on) return;
            i.seen = IntroSeen();
            i.skippable = allowSkip && i.seen;
            if (allowSkip && !i.seen)
                TO1_LOG("choose intro: SKIP held back - not watched through on this install (%s)\n", kSeenFile);
            i.t0 = now;
            const char* tm = ArtTest();
            Image("anim-group.side.chArt.chLayers.chDeck",
                  !std::strcmp(tm, "1080")
                      ? "streaming/t2r/choose/ce-choose-deck_1080p.png"
                      : !std::strcmp(tm, "ce")
                          ? "streaming/t2r/clanend/ce-lost-ser-L0.png"
                          : "streaming/t2r/choose/ce-choose-deck.png");
            Vis("anim-group.side.chArt.chLayers.chDeck", true);
            Vis("anim-group.side.chIntro", true);
            ChA("anim-group.side.chUi", 0.003f);
            ChA("anim-group.side.chIntro.chIntroSkip", 0.003f);
            Vis("anim-group.side.chIntro.chIntroSkip", i.skippable);
            Text("anim-group.side.chIntro.chIntroSkip.chSkipText", "SKIP");
            Vis("anim-group.side.chIntroFade", false);
            ChiArt(false);
            FillIntroLines();
            // chi_play(CHI_LORE) / chi_play(CHI_TAG) and the per-frame mixer
            // overrides are private-PCM / mixer seams: no native voice here.
        }
        void ChiSkip(uint64_t now)
        {
            Intro& i = g.intro;
            if (!i.on || i.skipping || i.jumped || !i.skippable) return;
            if (IntroElapsed(i, now) >= IntroDropMs()) return;
            i.skipping = true;
            i.skipAt = now;
            Vis("anim-group.side.chIntroFade", true);
            ChA("anim-group.side.chIntroFade", 0.003f);
        }
        void IntroTick(uint64_t now)
        {
            Intro& i = g.intro;
            if (!i.on) return;
            // chi_hold_wanted(): the entry hand-off / TWELVE WEEKS hand-off has
            // no native producer, so the clock starts at once (the ext's cap is
            // 9 s; this owner never holds the tarp).
            int64_t el = IntroElapsed(i, now);
            if (!i.artOn && (el >= kIntroArtMs || i.skipping || i.jumped)) ChiArt(true);

            if (i.skipping && !i.jumped) {
                const float d = static_cast<float>(now - i.skipAt) / kSkipDownMs;
                ChA("anim-group.side.chIntroFade",
                    SkipDownAlpha(static_cast<int64_t>(now - i.skipAt)));
                if (d >= 1.0f) {
                    i.jumped = true;
                    i.t0 = now - static_cast<uint64_t>(IntroUiMs());
                    i.skipAt = now;
                    el = IntroUiMs();
                    Vis("anim-group.side.chIntro", false);
                    Vis("anim-group.side.chArt.chLayers.chDeck", false);
                }
            } else if (i.jumped) {
                const float d = static_cast<float>(now - i.skipAt) / kSkipUpMs;
                ChA("anim-group.side.chIntroFade", SkipUpAlpha(static_cast<int64_t>(now - i.skipAt)));
                if (d >= 1.0f) Vis("anim-group.side.chIntroFade", false);
            }
            if (!i.jumped) {
                for (int k = 0; k < kIN && !i.linesOff; ++k) {
                    char p[96], c[128];
                    std::snprintf(p, sizeof p, "anim-group.side.chIntro.chIntroL%d", k);
                    ChA(p, IntroLineAlpha(el, k));
                    if (!kLines[k].logo) continue;
                    // The logo line's words, logo and tail have their own beats.
                    for (int j = 0; j < kBeatCount; ++j) {
                        std::snprintf(c, sizeof c, "%s.b%d", p, j);
                        ChA(c, IntroBeatAlpha(el, j));
                    }
                    std::snprintf(c, sizeof c, "%s.lg", p);
                    ChA(c, IntroLogoAlpha(el));
                    std::snprintf(c, sizeof c, "%s.tl", p);
                    ChA(c, IntroLogoAlpha(el));
                }
                if (el >= kTextOut + kOutMs2 && !i.linesOff) {
                    i.linesOff = true;
                    for (int k = 0; k < kIN; ++k) {
                        char p[64];
                        std::snprintf(p, sizeof p, "anim-group.side.chIntro.chIntroL%d", k);
                        Vis(p, false);
                    }
                }
                ChA("anim-group.side.chIntro.chIntroSkip",
                    IntroSkipAlpha(el, i.skippable));
            }
            if (!i.uiOn && el >= IntroUiMs()) {
                i.uiOn = true;
                if (!i.jumped && !i.seen) {
                    SaveIntroSeen();
                    i.seen = true;
                }
            }
            ChA("anim-group.side.chUi", IntroUiAlpha(el));
            // chi_audio(), ch_music_hold(), ce_menu_music_hold(): mixer / PCM
            // seams (choose_intro.pcm and choose_music.pcm are private assets).
            const bool done = el >= static_cast<int64_t>(IntroUiMs()) + kUiInMs + 100
                           && IntroCameraYAt(el) <= 0.0f
                           && (!i.jumped || now - i.skipAt >= static_cast<uint64_t>(kSkipUpMs));
            if (done) {
                i.on = false;
                Vis("anim-group.side.chIntro", false);
                Vis("anim-group.side.chArt.chLayers.chDeck", false);
                Vis("anim-group.side.chIntroFade", false);
                Alpha("anim-group.side.chUi", 1.0f);
            }
        }

        // ---- the key-art tick (choose.c ch_tick) -------------------------------

        void ChalkStart(int side)
        {
            const int other = side ^ 1;
            Vis("anim-group.side.chUi.chChalk", true);
            Piv("anim-group.side.chUi.chChalk", kLogoCX[other], kChalkCy);
            for (int k = 0; k < kChalkN; ++k) {
                char p[64], img[96];
                std::snprintf(p, sizeof p, "anim-group.side.chUi.chChalk.chg%d", k);
                std::snprintf(img, sizeof img, "streaming/t2r/choose/ch-chalkg-%02d.png", k);
                ChA(p, 0.003f);
                Image(p, img);
            }
        }

        void PayoffTick(uint64_t now)
        {
            if (!g.payoff.on) return;
            for (int k = 0; k < kChalkN; ++k) {
                char p[64];
                std::snprintf(p, sizeof p, "anim-group.side.chUi.chChalk.chg%d", k);
                ChA(p, ChalkAlpha(g.payoff, k, now));
            }
            if (PayoffSoundDue(g.payoff, now))
                // The NR wave is a private PCM in the ext; the engine's own SFX
                // approximation is the closest native event.
                Sfx("sfx-rl-hub-interact-map-select");
        }

        void ApplyRamps(uint64_t now)
        {
            const Page& p = g.page;
            const bool fading = p.stage == kFadeOut;
            if (!fading && now - p.enterAt >= kInMs + kStaggerMs + 60) return;
            const float eo = fading ? ExitAmount(p, now) : 1.0f;
            const float bg = fading ? BackgroundOut(p, now) : BackgroundIn(p, now);
            const float ct = ContentIn(p, now) * eo;
            ChA("anim-group.bgDark", bg);
            if (p.stage == kIntro)
                ChA("anim-group.intro", ct);
            else if (fading) {
                ChA("anim-group.side", ct);
                ChA("anim-group.intro", 0.003f);
            }
        }

        void DrawArt(uint64_t now, bool introActive)
        {
            if (!g.art.on || !g.art.shown) return;
            Art& a = g.art;
            const float zt = a.zoom.Val(now);
            const float pan = a.pan.Val(now);
            const float seconds = static_cast<float>(now - a.t0) / 1000.0f;
            const float camY = g.intro.on && !g.intro.jumped
                             ? IntroCameraY(now, g.intro.t0) : 0.0f;

            // The final render-camera track has no native installer. The same
            // authored camera is applied to the five parallax layers instead.
            for (int i = 0; i < 5; ++i) {
                const LayerPose q = LayerAt(i, zt, pan, seconds, camY);
                char p[64];
                std::snprintf(p, sizeof p, "anim-group.side.chArt.chLayers.chL%d", i);
                Scale(p, q.scale);
                Piv(p, q.x, q.y);
            }
            if (g.intro.on && !g.intro.jumped) {
                const LayerPose d = DeckAt(zt, pan, seconds, camY);
                Scale("anim-group.side.chArt.chLayers.chDeck", d.scale);
                Piv("anim-group.side.chArt.chLayers.chDeck", d.x, d.y);
            }
            if (now - g.grainAt >= static_cast<uint64_t>(kGrainMs)) {
                g.grainAt = now;
                g.grain = g.grain * 1664525u + 1013904223u;
                Piv("anim-group.side.chArt.chGrain",
                    -512.0f + static_cast<float>((g.grain >> 8) & 511),
                    -512.0f + static_cast<float>((g.grain >> 20) & 511));
            }
            const float pa = PayoffTextAlpha(g.payoff, now);
            const LogoPose logoPay = PayoffLogo(g.payoff, now);

            // Camp names: small under the logo -> the big title, dipping in the
            // middle of a switch so the two never touch.
            const float p0 = a.name[0].Val(now), p1 = a.name[1].Val(now);
            const bool both = p0 > 0.02f && p0 < 0.98f && p1 > 0.02f && p1 < 0.98f;
            for (int k = 0; k < 2; ++k) {
                const float t = a.name[k].Val(now);
                const float x = kLogoCX[k] + (kBigX - kLogoCX[k]) * t;
                const float y = kTabY + (kBigY - kTabY) * t;
                const float sc = kTabK + (1.0f - kTabK) * t;
                char p[64];
                std::snprintf(p, sizeof p, "anim-group.side.chUi.chName%d", k);
                Piv(p, x, y);
                Scale(p, sc);
                const float dip = both ? std::fabs(2.0f * t - 1.0f) : 1.0f;
                ChA(p, dip * ((g.payoff.on && k == g.payoff.side) ? pa : 1.0f));
            }
            // Description + warning: out fast, text swapped, in once the name
            // has arrived. The overview hover uses the same path.
            const int tg = a.focus >= 0 ? a.focus
                         : (!g.payoff.on && a.hov >= 0 ? a.hov : -1);
            if (tg != g.descTgt) {
                g.descTgt = tg;
                g.descTAt = now;
            }
            float u = static_cast<float>(now - g.descTAt);
            float da = 0.0f;
            if (tg >= 0) {
                if (a.descFor != tg) {
                    if (a.descFor >= 0 && u < kFadeOutMs)
                        da = 1.0f - u / kFadeOutMs;
                    else {
                        Text("anim-group.side.chUi.chDesc", SIDE_DESC[tg + 1]);
                        a.descFor = tg;
                    }
                }
                if (a.descFor == tg) {
                    // A hovered camp's text is already up when it becomes the
                    // focus: do not restart the name + fade-in deadline.
                    if (g.wasTg == tg && g.wasFocus < 0 && a.focus == tg) {
                        const uint64_t lead = static_cast<uint64_t>(kNameMs + kFadeInMs);
                        g.descTAt = now > lead ? now - lead : 0;
                        u = static_cast<float>(now - g.descTAt);
                    }
                    g.wasTg = tg;
                    g.wasFocus = a.focus;
                    da = u < kNameMs ? 0.0f : Ease((u - kNameMs) / kFadeInMs);
                }
            } else if (a.descFor >= 0) {
                da = u < kFadeOutMs ? 1.0f - u / kFadeOutMs : 0.0f;
                if (da <= 0.0f) a.descFor = -1;
            }
            ChA("anim-group.side.chUi.chDesc", da * pa);
            ChA("anim-group.side.chUi.chWarn", da * pa);
            if (g.payoff.on) {
                ChA("anim-group.side.chUi.chHint", pa);
                ChA("anim-group.side.chUi.chHintPad", pa);
                ChA("anim-group.side.chUi.chSelIcon", pa);
                ChA("anim-group.side.chUi.chSelText", pa);
            }
            const float twa = TitleAmount(a, now, a.descFor, introActive);
            Alpha("anim-group.side.chUi.chTitleLogo", Ease(twa));

            for (int k = 0; k < 2; ++k) {
                const float h = a.hl[k ^ 1].Val(now) * (a.focus < 0 && !g.payoff.on ? 1.0f : 0.0f);
                char p[64];
                std::snprintf(p, sizeof p, "anim-group.side.chArt.chDim%d", k);
                ChA(p, h * kHlDim);
            }
            if (!g.payoff.on) {
                const float sa = a.focus < 0 ? 0.35f : (a.selHov ? 1.0f : 0.85f);
                ChA("anim-group.side.chUi.chSelIcon", sa);
                ChA("anim-group.side.chUi.chSelText", sa);
                Text("anim-group.side.chUi.chHint", a.focus < 0 ? kHintOver : kHintFocus);
            }
            for (int k = 0; k < 2; ++k) {
                float l = a.logo[k].Val(now) * (1.0f + kHlLogo * a.hl[k].Val(now));
                char p[64];
                std::snprintf(p, sizeof p, "anim-group.side.chUi.chLogo%d", k);
                if (g.payoff.on && k == g.payoff.side) {
                    l *= logoPay.scale;
                    Piv(p, kLogoCX[k], kLogoY - logoPay.rise);
                }
                Scale(p, l);
                const float la = a.focus >= 0 && a.focus != k
                               ? 0.6f + 0.35f * a.hl[k].Val(now) : 1.0f;
                ChA(p, la);
            }
            const int64_t el = g.intro.on ? IntroElapsed(g.intro, now) : INT64_MAX;
            float brand = 0.0f;
            if (g.intro.on && !g.intro.jumped)
                brand = el < IntroDropMs() ? 1.0f
                      : 1.0f - static_cast<float>(el - IntroDropMs()) / kBrandOutMs;
            ChA("anim-group.side.sideBrand", brand);

            PadHint("anim-group.side.chHint", "anim-group.side.chHintPad", g.hintArt);
            // ch_vox_tick(), ch_music_hold(), ch_amb_tick(), ce_img_tick():
            // private PCM / mixer / deferred-image seams. No native producer;
            // sfx-cycle on a focus switch is the engine-SFX approximation.
        }

        void GlowTick(uint64_t now)
        {
            const float dt = g.glAt && now - g.glAt < 200
                           ? static_cast<float>(now - g.glAt) / 1000.0f : 0.016f;
            const float step = dt / 0.3f;
            g.glAt = now;
            for (int k = 0; k < 2; ++k) {
                const float target = (g.page.stage == kPick || g.page.stage == kCrossfade)
                                   && k == (g.art.on ? g.art.focus : g.page.focus) ? 1.0f : 0.0f;
                const float d = target - g.gl[k];
                g.gl[k] += d > step ? step : (d < -step ? -step : d);
                char p[64];
                std::snprintf(p, sizeof p, "anim-group.side.glow%d", k ? 'S' : 'W');
                ChA(p, SideEase(g.gl[k]));
            }
        }

        // ---- page entry / leave ----------------------------------------------

        void FillIntroCard(const net::clan::State& c)
        {
            const int st = (c.st == 2 || c.st == 3) ? c.st : 0;
            Text("anim-group.intro.introText", INTRO_TEXT[st]);
            if (st) {
                char b[200];
                const int days = std::min(c.day, 83);
                std::snprintf(b, sizeof b,
                    "DAYS SURVIVED: %d (WEEK %d)\nMOST SURVIVORS: %d\nCLANS SURVIVED: %d   CLANS LOST: %d",
                    days, st == 3 ? 12 : days / 7, c.mx, c.rw, c.rl);
                TextRaw("anim-group.intro.introTitle", st == 3
                    ? "<font color=\"#5dc0dc\">YOUR CLAN SURVIVED</font>"
                    : "<font color=\"#e04848\">YOUR CLAN IS GONE</font>");
                TextRaw("anim-group.intro.introStats", engine::locale::Tr(b));
                Text("anim-group.intro.introNextText", "START A NEW CLAN");
            } else {
                TextRaw("anim-group.intro.introTitle", "");
                TextRaw("anim-group.intro.introStats", "");
                Text("anim-group.intro.introNextText", "NEXT");
            }
        }

        void RcNote()
        {
            if (!g.rc) return;
            Text("anim-group.side.chUi.chWarn", kRcNote);
            Text("anim-group.side.sideWarn", kRcNote);
        }

        void EnterPage(const net::clan::State& c, uint64_t now)
        {
            g = Runtime{};
            g.entered = true;
            g.rc = c.rc != 0;
            g.page.ceTest = engine::flag::Present("t2r-ce-test.txt");
            g.page.living = c.ok && c.st == 1;
            const bool art = !ArtDisabled();
            const bool cinematic = art && !IntroDisabled();
            Enter(g.page, false, cinematic, g.rc, now);
            Show(g.art, art, now);
            SideImages();

            if (g.page.stage == kPick) {
                // The key art + its intro replace the old card: stage 1 starts
                // at once, with the intro running in parallel inside it.
                Vis("anim-group.intro", false);
                Alpha("anim-group.side", 1.0f);
                Vis("anim-group.side", true);
                ChShow(now);
                IntroBegin(now, c.st == 2 || c.st == 3 || g.page.ceTest);
            } else {
                Alpha("anim-group.intro", 0.003f);
                Alpha("anim-group.bgDark", 0.003f);
                Vis("anim-group.intro", true);
                Vis("anim-group.side", false);
                FillIntroCard(c);
                ChShow(now);
                OldFocus(0);
            }
            // ch_show writes the normal warning; the one-time re-choice note
            // has to be restored after it (side_tick's side_rc_note).
            if (g.rc) RcNote();
            TO1_LOG("choose: native owner entered (art=%d intro=%d rc=%d st=%d)\n",
                    art ? 1 : 0, cinematic ? 1 : 0, g.rc ? 1 : 0, c.st);
        }

        void ClosePage(const char* why)
        {
            (void)why;
            if (engine::widget::TopPage() == SidePage())
                DC::Call<"menu2-pop-page">(MenuStack::hudMenuStack, 0);
            TO1_LOG("choose: page closed (%s)\n", why);
            Reset();
        }

        void ConfirmSelect(uint64_t now)
        {
            const int side = g.art.on ? g.art.focus : g.page.focus;
            if (side < 0) return;
            if (g.page.ceTest && g.art.on && g.page.stage == kPick) {
                // A demo over a living clan plays the payoff but sends no side.
                Confirm(g.page, side, g.art.on, true, now);
                StartPayoff(g.payoff, side, g.art.on, now);
                ChalkStart(side);
                return;
            }
            if (!SendSide(side)) {
                TO1_LOG("choose: 0xE4 side=%d failed (no daemon reply)\n", side + 1);
                return;
            }
            Sfx("sfx-select");
            g.sideWaitUntil = now + 8000;   // no re-push while the reply is in flight
            Confirm(g.page, side, g.art.on, false, now);
            if (g.art.on) {
                StartPayoff(g.payoff, side, true, now);
                ChalkStart(side);
            }
            TO1_LOG("choose: side %s chosen%s\n", SIDE_NAME[side + 1],
                    g.rc ? " (one-time re-choice: the clan keeps its progress)" : "");
        }

    } // namespace

    void Tick(uint64_t now)
    {
        if (boot::ExtLoaded()) return;
        if (!SidePageUp()) {
            if (g.entered) Reset();
            return;
        }
        net::clan::State cs{};
        if (net::clan::HaveState()) cs = net::clan::Get();
        if (!g.entered) EnterPage(cs, now);

        const uint64_t frame = engine::frame::Count();
        bool sel  = engine::menu_input::CommandEdge(SID("menu-select"), g.hSel);
        bool back = engine::menu_input::CommandEdge(SID("menu-back"), g.hBack);
        bool left  = engine::menu_input::CommandEdge(SID("menu-left"), g.hLeft);
        bool right = engine::menu_input::CommandEdge(SID("menu-right"), g.hRight);
        left  |= engine::menu_input::CommandEdge(SID("menu-prev-tab"), g.hPrev);
        right |= engine::menu_input::CommandEdge(SID("menu-next-tab"), g.hNext);

        float mx = -1.0f, my = -1.0f;
        const bool live = (g.page.stage == kPick || g.page.stage == kCrossfade)
                       && engine::window::CursorCanvas(&mx, &my);
        const int zone = !live ? -2
                       : Over("anim-group.side.chHovW", true) ? 0
                       : Over("anim-group.side.chHovS", true) ? 1 : -1;
        // The logos / names are rect-only: arming them let the engine's hover
        // fight the per-frame pivot writes (the ext's exact fix).
        const int tab = !live ? -1
                      : Over("anim-group.side.chUi.chLogoW", false)
                     || Over("anim-group.side.chUi.chNameW", false) ? 0
                      : Over("anim-group.side.chUi.chLogoS", false)
                     || Over("anim-group.side.chUi.chNameS", false) ? 1 : -1;
        const bool onSel = live && (Over("anim-group.side.chUi.chSelText", true)
                                 || Over("anim-group.side.chUi.chSelIcon", true));
        bool click = sel && engine::menu_input::SelectIsMouse();
        if (click) sel = false;
        const bool rb = engine::window::KeyDown(VK_RBUTTON);
        const bool rclick = live && rb && !g.rbHeld;
        g.rbHeld = rb;

        // While the intro runs, SELECT is SKIP (only after a watched-through
        // run) and nothing else reaches the picker.
        const bool introBusy = g.intro.on && !IntroInputOk(g.intro.on, now, g.intro.t0);
        if (introBusy) {
            if (sel) ChiSkip(now);
            sel = click = false;
            left = right = false;
        }
        if (g.intro.on) IntroTick(now);
        ApplyRamps(now);

        if (g.rc && ShowRechoiceNote(g.page) && !(frame % 30)) RcNote();

        // Back: a focused camp returns to the overview; only a living clan or
        // the demo closes the page, after the ext's six-frame latch. The latch
        // is checked before the stage switch, exactly like side_tick.
        if (back && !g.page.backFrameAt && (g.page.ceTest || g.page.living))
            g.page.backFrameAt = frame;
        const bool artBack = (back || rclick) && Back(g.art, g.page.pickSide >= 0, now);
        if (artBack) {
            back = false;
            g.page.backFrameAt = 0;
        } else if ((back || g.page.backFrameAt)
                   && BackOrClose(g.page, false, frame, now)) {
            ClosePage("back");
            return;
        }

        if (g.page.stage == kIntro) {
            // The old intro card (no key art / no cinematic).
            if (sel) {
                Sfx("sfx-select");
                IntroSelect(g.page, now);
                Alpha("anim-group.side", 0.0f);
                Vis("anim-group.side", true);
                ChShow(now);
                RcNote();
            }
            return;
        }
        if (g.page.stage == kCrossfade) {
            const float e = CrossfadeAlpha(g.page, now);
            ChA("anim-group.intro", 1.0f - e);
            ChA("anim-group.side", e);
            if (g.page.fadeMs > 220)
                Alpha("anim-group.bgDark", 1.0f + (kBgA - 1.0f) * e);
            if (CrossfadeDone(g.page, now)) {
                Vis("anim-group.intro", false);
                Alpha("anim-group.intro", 1.0f);
                Alpha("anim-group.side", 1.0f);
            }
        }
        if (g.page.stage != kPick && g.page.stage != kCrossfade) {
            // Stage 6 (payoff) and stage 3 (exit fade) own the rest of the tick.
            DrawArt(now, introBusy);
            GlowTick(now);
            if (g.page.stage == kPayoff) {
                PayoffTick(now);
                if (PayoffDone(g.payoff, now)) PayoffEnded(g.page, now);
            }
            if (g.page.stage == kFadeOut && FadeOutDone(g.page, now)) {
                const bool pick = g.page.pickSide >= 0;
                Popped(g.page);
                // ms_pick_start() / ms_pick_covered() belong to the mission
                // screen; there is no native producer here, so the guarded
                // page pop is the whole hand-off.
                ClosePage(pick ? "pick" : "back");
            }
            return;
        }

        if (g.art.on) {
            if (left || right) {
                const int f = Input(g.art, left, right, now);
                if (f >= 0) Sfx("sfx-cycle");   // camp vox is a private PCM seam
            }
            if (Mouse(g.art, tab, zone, onSel, click && g.page.stage == kPick,
                      live ? mx : -1.0f, live ? my : -1.0f, g.payoff.on, now))
                sel = 1;
            if (sel && g.art.focus >= 0) ConfirmSelect(now);
        } else {
            if (left || right) {
                g.page.focus ^= 1;
                OldFocus(g.page.focus);
            }
            if (g.page.stage == kPick) {
                const int hv = Over("anim-group.side.figW", true) ? 0
                             : Over("anim-group.side.figS", true) ? 1 : -1;
                if (hv != g.hHov) {
                    g.hHov = hv;
                    if (hv >= 0 && hv != g.page.focus) {
                        g.page.focus = hv;
                        OldFocus(hv);
                    }
                }
            }
            if (sel) ConfirmSelect(now);
        }
        DrawArt(now, introBusy);
        GlowTick(now);
    }

    bool Active() { return !boot::ExtLoaded() && g.entered; }
    int Stage() { return g.page.stage; }

    void Reset()
    {
        g = Runtime{};
    }

} // namespace net::ui::choose
