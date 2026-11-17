// melee v2 (a recode of the melee system after a root problem in the layered design).
// Step 1 of the recode: ONE switch that retires the layers that fought over the body and the button (live logs: ~30 % of blows showed no
// reaction, 1-11 presses per player eaten, Combo / Strike bars refilling, exchanges ending on their own):
//   - no turn-based exchange (mex / mseq / mqte): every swing is a free swing, the engine's own combo timing
//   - the windup keeps SP speed (no tell speed-up / x0.65 slow-down; the swish + grunt cue stays)
//   - a blow always shows on the victim: no 1.0 s graze, no immune window, a blow mid swing cancels our swing and plays the reaction (SP)
//   - a melee press during the cadence lock is never dropped while we are not hit (fires when the lock ends)
// t2r-melee-v1.txt = the old layers back.
static int Mv2On(void)
{
    static int on = -1;
    static uint64_t at;
    uint64_t now = GetTickCount64();
    if (on < 0 || now - at >= 2000) {
        at = now ? now : 1;
        FILE *f = ExeSideOpen("t2r-melee-v1.txt", "r");
        int o = f == NULL;
        if (f)
            fclose(f);
        if (o != on)
            Xlog("melee v2: %s",
                 o ?
                 "ON (free swings, SP windup, every blow reacts, no eaten presses; t2r-melee-v1.txt = old layers)"
                 : "OFF (t2r-melee-v1.txt)");
        on = o;
    }
    return on;
}
