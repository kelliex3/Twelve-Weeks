// ---------------------------------------------------------------- Esc / Start on the VICTORY / DEFEAT card shows the scoreboard
// Pressing Esc while the victory / defeat screen is on shows the scoreboard. winmm pushes to1/scoreboard at the match end and the
// end banner (feature_hud_overlays) keeps that page's anim-group hidden until the fade to black; one press of Esc (keyboard) or Start / Options
// (any pad) now shows it in place of the card, a second press brings the card back. Only the visibility of the two widgets changes: the end
// clock (fade to black, the leave to the menu) is untouched, so a scoreboard left open is simply covered by the fade like the card is.
// Pad: the engine's own Joypad mask (padinject.c) is read raw for the command that opens the pause menu - the name is not known, so a few
// candidates are probed every 2 s until one yields an input bit (logged once); XInput Start / Back is the fallback for a plain XInput pad.
// Log 'endpeek: ...'. Off: t2r-no-end-peek.txt.
static int EscPeekOff(void)
{
    
static int off = -1;
    static uint64_t chk;
    
return OvOff("t2r-no-end-peek.txt", &off, &chk);

}


static int EscPeekEdge(void)
{                               // one press edge of Esc / the pad's pause button, polled once per frame
    static int was;
    static struct padinj_w *pw;
    static uint64_t probe_at;
    static int ni;
    
int on = 0;
    
if (!GameFocused()) {
        was = 1;
        return 0;
    }                           // (a button held while focus comes back is no press)
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
        on = 1;
    
WORD pb = 0;
    BYTE l, r;
    SHORT x, y;
    
if (!on && PadState(&pb, &l, &r, &x, &y) && (pb & (0x0010 | 0x0020)))
        on = 1;                 // XInput Start / Back
    if (!on && PadinjReady()) {
        
static const char *const N[] =
            { "pause", "start", "pause-menu", "menu-pause", "options" };
        
uint64_t now = GetTickCount64();
        
if (!pw && now >= probe_at) {
            
probe_at = now + 2000;
            
const char *nm = N[ni++ % (int) (sizeof N / sizeof *N)];
            
struct padinj_w *w = PadinjWatch(nm);
            
if (w) {
                PadinjProbeRead(nm);
                if (w->mask && w->jp) {
                    pw = w;
                    Xlog("endpeek: pad button learned from command '%s' (input bit %016llx on joypad %p)", nm, (unsigned long long) w->mask, w->jp);
                }
            }
        
}
        
if (pw && !IsBadReadPtr((const uint8_t *) pw->jp + 0x138, 8)
             &&
             (*(const volatile uint64_t *)
              ((const uint8_t *) pw->jp + 0x138) & pw->mask))
            on = 1;
    
}
    
int edge = on && !was;
    
was = on;
    
return edge;

}


