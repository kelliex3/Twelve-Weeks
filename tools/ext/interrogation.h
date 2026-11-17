// interrogation.h - the Interrogation state shared between interrogation.c (gameplay, server link) and interrogation_hud.c (the widget HUD).
// The HUD only READS these; gameplay owns and writes them. Included from interrogation.c.
//
//   int_t g_int            last server reply (op 0xDD "on=1 mode=interrogation ..."): on / map / team (our team) / need (interrogations to reveal, 2013: 5) /
//                          p[team] interrogations done (the big left bar) / rev[team] 1 = team has located the ENEMY lockbox / box[team] candidate index /
//                          st[team] lockbox stages that team got through (0..3, the padlock bar: 3 stages, checkpoints kept per stage) /
//                          h[team] = percent (0..100) of the stage the team is unlocking RIGHT NOW (server: highest fresh report of a client of that team; the marker rings),
//                          hold = seconds one unlock stage takes now / win = winning team or -1 / ended
//   int_live()             an Interrogation match is running (substate 6, server said on=1)
//   int_lockbox(team,pos)  world position of a team's lockbox (the safe prop stands there); our own is always known, the enemy's once rev[our team]
//   g_int_target           slot of the victim of the interrogation we run RIGHT NOW (-1 = none): the 2013 interrogate-enter move plays
//   g_int_hold_frames      1/60 s ticks the running interrogation has played (0 when none); the victim dies at INT_SECS
//   INT_SECS               seconds until the running interrogation's kill frame (fists 4.6, shiv ~2.5) - a variable, not a constant
//   g_lb_hold_frames       1/60 s ticks of the unlock hold in progress at the enemy lockbox; one stage takes g_int.hold seconds
//   g_int_ended_sent       the match end was triggered (hide the HUD)
//
// Unlocking: hold E (pad: the interact button) within LB_RANGE of the enemy lockbox; 3 stages, each its own hold, a finished stage stays done (2 checkpoints
// between the 3 stages: letting go inside a stage loses only that stage's progress).
typedef struct {
    int ok, on, map, team, need, kpi, p[2], rev[2], box[2], st[2], h[2],
        win, ended, sp;
    float hold;
} IntT;                        // sp: unlock speed steps so far (server 'sp')
