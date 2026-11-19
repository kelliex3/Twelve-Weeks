    // ---- fists (Abby), wall, OPEN: 05:08 throat strike + head into the wall
    { "wall-throat-punch", 1, 1.6f, 2.20f, -1, 2,
     { CD_ABBY_PAIR("hwall-melee-throat-punch-intro", 1.0f, 0.0f),
      CD_ABBY_PAIR("hwall-melee-throat-punch", 1.0f, 0.0f)} },
    // ---- fists, wall, COMBO: 04:56 forehead grab, head PULLED BACK 0.7 m off the wall, then smashed into it (SP E=28's finisher; replaces
    //      the quick 1.5 s hwall-melee-head-smash shove - the slower one, fairer vs the other takedowns). fist-wall-right-combo-head-smash-finisher is
    //      combo-only (dmax 2.5), so it starts from the contextual wall opener fist-wall-attack-enter-right / -right-far (window 0.5 .. 2.0 m, cd_dmin):
    //      abby-fist-hwall-enter-npc-close-hit (victim authored 0.50 m out -> 0.18 m) / -npc-far-hit (1.72 m -> 0.17 m), each from frame 0, cut at the
    //      root match to the finisher intro's frame 0 (tools/ctxcut.py), then SP's intro (both halves, 0.23 s) and the finisher (hand grabs the head 0.23 s,
    //      pulled back to 0.73 s, head into the wall 1.07 s at 6.1 m/s, let go 1.6 s, pelvis on the floor 3.17 s)
    //      SP capture (re/sp-abby-fist-sounds-1026.log): the pinning hit plays 0.53 s before the finisher intro takes over (ours was 0.87 s) - the
    //      close push is cut at 0.55 s so the pin -> grab -> slam rhythm is SP's (slam 1.04 s into the finisher both ways)
    { "wall-combo-head-smash", 1, 2.4f, 3.22f, -1, 3,
     { CD_ABBY_PAIR("hwall-enter-npc-close-hit", 1.0f, 0.55f),
      CD_ABBY_PAIR("kill-hwall-combo-head-smash-intro", 1.0f, 0.0f),
      CD_ABBY_PAIR("kill-hwall-combo-head-smash", 1.0f, 0.0f)} },
    { "wall-combo-head-smash~far", 1, 2.4f, 3.22f, -1, 3,
     { CD_ABBY_PAIR("hwall-enter-npc-far-hit", 1.0f, 1.20f),
      CD_ABBY_PAIR("kill-hwall-combo-head-smash-intro", 1.0f, 0.0f),
      CD_ABBY_PAIR("kill-hwall-combo-head-smash", 1.0f, 0.0f)} },
    // ---- fists, low cover, OPEN: 05:15 grab + rotate + face onto the ledge (its intro is attacker-only: the main pair starts the run-in itself, 1.16 m out)
    { "cover-face-smash", 2, 1.7f, 2.80f, -1, 1,
     { CD_ABBY_PAIR("low-cover-kill-left-hand-face-smash", 1.0f, 0.0f)} },
    // ---- fists, low cover, COMBO: 05:35 pick up + head onto the ledge, 05:43 grab + head onto the ledge - push cut at the best match (0.16 / 0.33 m)
    //      (SP rules: a combo-only finisher needs the contextual low-cover swing first, whose opener window is the defender 0.5 .. 1.7 m from the
    //      cover frame (fist-low-cover-neck-attack-enter-close / -far: wall-min-range 0.5, wall-range 1.7, player-fisurgeonguild-contextual.bin) - cd_dmin)
    //      The old pushes low-cover-combo-2 / -3-hit move the victim 0.03 / 0.00 m (both bodies are
    //      already at the cover in them: they are the hits of a running combo). SP's push INTO the cover is the low-cover opener pair (contextual.bin
    //      fist-low-cover-neck-attack-enter-close / -shoulder-attack-enter-far): abby-fist-lc-enter-player-close-npc-close-neck-hit (victim authored
    //      0.59 m out -> 0.13 m, attacker 0.65 m) and -npc-far-shoulder-hit (2.02 m -> 0, attacker 2.18 m). Played from frame 0, cut where both bodies
    //      are nearest the finisher's start (root match: neck 0.93 s, 0.29 / 0.10 m; shoulder 1.50 s, 0.33 / 0.22 m). "~far" = the far variant of the
    //      same move: cd_pick plays, per move, the variant whose authored start is nearest the bodies (SP's "ideal wall distance" ranking).
    { "cover-lift-n-slam", 2, 1.9f, 2.05f, -1, 2,
     { CD_ABBY_PAIR
      ("lc-enter-player-close-npc-close-neck-hit", 1.0f, 0.93f),
      CD_ABBY_PAIR("kill-cover-combo-right-lift-n-slam", 1.0f, 0.0f)} },
    { "cover-lift-n-slam~far", 2, 2.4f, 2.05f, -1, 2,
     { CD_ABBY_PAIR
      ("lc-enter-player-close-npc-far-shoulder-hit", 1.0f, 1.50f),
      CD_ABBY_PAIR("kill-cover-combo-right-lift-n-slam", 1.0f, 0.0f)} },
    { "cover-combo-head-smash", 2, 1.9f, 2.10f, -1, 2,
     { CD_ABBY_PAIR
      ("lc-enter-player-close-npc-close-neck-hit", 1.0f, 0.93f),
      CD_ABBY_PAIR("kill-cover-combo-right-head-smash", 1.0f, 0.0f)} },
    { "cover-combo-head-smash~far", 2, 2.4f, 2.10f, -1, 2,
     { CD_ABBY_PAIR
      ("lc-enter-player-close-npc-far-shoulder-hit", 1.0f, 1.50f),
      CD_ABBY_PAIR("kill-cover-combo-right-head-smash", 1.0f, 0.0f)} },
    // ---- knife (Ellie), wall, COMBO: 02:25 / 02:20 gut stabs (push cut 0.24 / 0.31 m); OPEN: 02:30 kneed gut slash (intro = the run-in)
    //      (SP rules, player-switchblade-finishers.bin: EVERY knife wall / low-cover finisher of the pool is open-startable - the chest-stab / left-chest-stab /
    //      gut-ice-pick ones through their "-intro" move whose lambda only refuses a running contextual combo - with wall-dist-max 2.5 (neck-slice 1.5, not in the pool);
    //      the high probe wins: "(if against-wall-result (= low-cover? #f) (= low-cover? #t))")
    //      push variants by distance (contextual.bin switchblade-wall-attack-enter-right / -right-far / -left-enter): victim authored start
    //      stab-enter-short-hit 0.60 m (cut 0.37 s), enter-hit-left 1.33 m (0.55 s), enter-hit 1.77 m (0.75 / 0.87 s) - each played from frame 0
    //      The push was short and insta-cut to the wall death instead of a short fluid pause: SP's real combo
    //      path (re/sp-sandbox-runs/knife-1020b.named.log) is push -> combo-intro (attacker only; the victim keeps its slash-1 hit) 0.37-0.40 s -> combo-mid
    //      0.27-0.40 s (the pinned stab, both halves) -> end. The intro / mid / end joins are seamless (0.001-0.024 m); the pushes are cut where they match
    //      the intro's frame 0 (D 0.15-0.27 m; the old 0.37 s close cut was mid-lunge). Chain before the end: 1.67 s (long) / 1.50 s (left), was 0.37-0.87 s.
    { "knife-wall-chest-stab-long", 1, 2.5f, 1.20f, 3, 4,
     { CD_ELLIE_PAIR("wall-combo-enter-hit", 1.0f, 1.00f),
      CD_RAW_PAIR
      ("ellie-knife-kill-wall-chest-stab-long-combo-intro--ellie",
       "ellie-knife-wall-combo-slash-1-hit--npc-f", 1.0f, 0.0f),
      CD_ELLIE_PAIR("kill-wall-chest-stab-long-combo-mid", 1.0f, 0.0f),
      CD_ELLIE_PAIR("kill-wall-chest-stab-long-end", 1.0f, 0.0f)} },
    { "knife-wall-chest-stab-long~close", 1, 2.5f, 1.20f, 3, 4,
     { CD_ELLIE_PAIR("wall-combo-stab-enter-short-hit", 1.0f, 0.70f),
      CD_RAW_PAIR
      ("ellie-knife-kill-wall-chest-stab-long-combo-intro--ellie",
       "ellie-knife-wall-combo-slash-1-hit--npc-f", 1.0f, 0.0f),
      CD_ELLIE_PAIR("kill-wall-chest-stab-long-combo-mid", 1.0f, 0.0f),
      CD_ELLIE_PAIR("kill-wall-chest-stab-long-end", 1.0f, 0.0f)} },
    { "knife-wall-left-chest-stab", 1, 2.5f, 1.30f, 3, 4,
     { CD_ELLIE_PAIR("wall-combo-enter-hit-left", 1.0f, 0.73f),
      CD_RAW_PAIR
      ("ellie-knife-wall-kill-left-chest-stab-combo-intro--ellie",
       "ellie-knife-wall-combo-slash-1-hit--npc-f", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-combo-mid", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-end", 1.0f, 0.0f)} },
    { "knife-wall-left-chest-stab~close", 1, 2.5f, 1.30f, 3, 4,
     { CD_ELLIE_PAIR("wall-combo-stab-enter-short-hit", 1.0f, 0.60f),
      CD_RAW_PAIR
      ("ellie-knife-wall-kill-left-chest-stab-combo-intro--ellie",
       "ellie-knife-wall-combo-slash-1-hit--npc-f", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-combo-mid", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-end", 1.0f, 0.0f)} },
    { "knife-wall-left-chest-stab~far", 1, 2.5f, 1.30f, 3, 4,
     { CD_ELLIE_PAIR("wall-combo-enter-hit", 1.0f, 0.97f),
      CD_RAW_PAIR
      ("ellie-knife-wall-kill-left-chest-stab-combo-intro--ellie",
       "ellie-knife-wall-combo-slash-1-hit--npc-f", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-combo-mid", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-left-chest-stab-end", 1.0f, 0.0f)} },
    { "knife-wall-kneed-gut-slice", 1, 2.5f, 2.95f, 3, 2,
     { CD_ELLIE_PAIR("wall-kill-kneed-gut-slice-intro", 1.0f, 0.0f),
      CD_ELLIE_PAIR("wall-kill-kneed-gut-slice", 1.0f, 0.0f)} },
    // ---- knife, ledge / low cover: 02:37 COMBO (the close push matches best, 0.44 m), 02:49 OPEN (intro = the run-in)
    //      push variants (SP E=65 played enter-far + enter-close-hit): enter-close-hit 0.60 m (cut 0.50 s), enter-hit 1.17 m (0.87 s), enter-left-hit 1.56 m (0.93 s)
    { "knife-cover-ice-pick", 2, 2.5f, 3.65f, 3, 2,
     { CD_ELLIE_PAIR("low-cover-combo-enter-close-hit", 1.0f, 0.50f),
      CD_ELLIE_PAIR("low-cover-kill-gut-to-ice-pick-end", 1.0f, 0.0f)} },
    { "knife-cover-ice-pick~mid", 2, 2.5f, 3.65f, 3, 2,
     { CD_ELLIE_PAIR("low-cover-combo-enter-hit", 1.0f, 0.87f),
      CD_ELLIE_PAIR("low-cover-kill-gut-to-ice-pick-end", 1.0f, 0.0f)} },
    { "knife-cover-ice-pick~far", 2, 2.5f, 3.65f, 3, 2,
     { CD_ELLIE_PAIR("low-cover-combo-enter-left-hit", 1.0f, 0.93f),
      CD_ELLIE_PAIR("low-cover-kill-gut-to-ice-pick-end", 1.0f, 0.0f)} },
    { "knife-cover-chest-stab", 2, 2.5f, 3.70f, 3, 2,
     { CD_ELLIE_PAIR("low-cover-kill-chest-stab-intro", 1.0f, 0.0f),
      CD_ELLIE_PAIR("low-cover-kill-chest-stab", 1.0f, 0.0f)} },
    // ---- held MELEE WEAPONS. TLOU2's weapon wall / low-cover finishers
    //      (player-1h-blade / -1h-blunt / -2h-blunt-finishers.bin, macro melee-player-wall-finisher-with-intro, context wall / low-cover; re/contextual-downs.md "Melee weapons"):
    //      the start lambda's weapon mask picks who may play it (cd_weapon_mask): style 1 = every 1h blade (machete, tomahawk, hatchet), 4 = the machete's own two, 0 = every 1h
    //      blunt, 5 = the crowbar / hammer / club / wrench neck jab, 2 = the 2h blunt (bat, 2x4, lead pipe). OPEN moves (from-open? = 1: the enemy one hit from going down) =
    //      the finisher's own intro (the run-in, where both halves have it) + the main clip, exactly as authored; window = the lambda's wall-dist-max. The streamed clips
    //      (1h low-cover double bash, 2h knee break / baseball swing / left swing) never play through animate_ (re/melee-finishers.md) and are left out; the ledge kills are an
    //      edge probe, not a wall. Names "wpn-*": no native sound rows (their banks are not in the MP level: the PCM weapon cues play, blade_cd_act / cd_sfx_label).
    //      end_s = the victim's pelvis on the floor + 0.05 s (decoded halves, tools/ctxcam_poses.py), counted from the start of the last segment.
    { "wpn-blade-wall-gut-chop", 1, 2.5f, 2.15f, 1, 2,
     { CD_WPN_PAIR("1hblade-hwall-gut-chop-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblade-hwall-gut-chop", 1.0f, 0.0f)} },
    { "wpn-blade-wall-neck-chop", 1, 2.0f, 2.45f, 1, 2,
     { CD_WPN_PAIR("1hblade-hwall-kill-left-neck-chop-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblade-hwall-kill-left-neck-chop", 1.0f, 0.0f)} },
    { "wpn-blade-wall-skull-chop", 1, 2.0f, 0.82f, 1, 2,
     { CD_WPN_PAIR("1hblade-hwall-skull-chop-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblade-hwall-skull-chop", 1.0f, 0.0f)} },
    { "wpn-blade-wall-shoulder-chop", 1, 2.4f, 1.85f, 1, 2,
     { CD_WPN_PAIR("1hblade-hwall-kill-shoulder-chop-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblade-hwall-kill-shoulder-chop", 1.0f, 0.0f)} },
    { "wpn-machete-wall-gut-stab", 1, 2.5f, 2.68f, 4, 2,
     { CD_WPN_PAIR("machete-wall-kill-left-gut-stab-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("machete-wall-kill-left-gut-stab", 1.0f, 0.0f)} },
    { "wpn-machete-wall-neck-slice", 1, 2.5f, 3.15f, 4, 2,
     { CD_WPN_PAIR
      ("machete-wall-kill-slash-2-neck-slice-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("machete-wall-kill-slash-2-neck-slice", 1.0f, 0.0f)} },
    { "wpn-blunt-wall-double-hit", 1, 2.2f, 1.25f, 0, 1,
     { CD_WPN_PAIR("1hblunt-hwall-kill-double-hit", 1.0f, 0.0f)} },
    { "wpn-blunt-wall-head-bash", 1, 2.0f, 1.72f, 0, 2,
     { CD_WPN_PAIR("1hblunt-hwall-kill-head-bash-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblunt-hwall-kill-head-bash", 1.0f, 0.0f)} },
    { "wpn-blunt-wall-left-sweep", 1, 2.0f, 2.32f, 0, 2,
     { CD_WPN_PAIR("1hblunt-hwall-kill-left-sweep-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("1hblunt-hwall-kill-left-sweep", 1.0f, 0.0f)} },
    { "wpn-blunt-wall-uppercut", 1, 2.5f, 1.38f, 0, 2,
     { CD_WPNA_PAIR("1hblunt-kill-uppercut-intro", 1.0f, 0.0f),
      CD_WPNA_PAIR("1hblunt-kill-uppercut", 1.0f, 0.0f)} },
    { "wpn-hammer-wall-neck-jab", 1, 2.5f, 1.42f, 5, 2,
     { CD_WPN_PAIR("hammer-hwall-kill-left-neck-jab-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("hammer-hwall-kill-left-neck-jab", 1.0f, 0.0f)} },
    { "wpn-2h-wall-head-bonk", 1, 1.5f, 1.48f, 2, 2,
     { CD_WPN_PAIR("2hblunt-wall-kill-head-bonk-intro", 1.0f, 0.0f),
      CD_WPN_PAIR("2hblunt-wall-kill-head-bonk", 1.0f, 0.0f)} },
    //      low cover: the 1h blade hack finisher is combo-only (from-open? = 0): the push pair (the contextual low-cover opener's -hit pair, by distance: close / enter / left)
    //      cut at the best root match to the finisher, then the finisher (its intro is attacker-only: left out). The 1h blunt low-cover double bash is streamed: none.
    { "wpn-blade-cover-hack", 2, 2.5f, 2.15f, 1, 2,
     { CD_WPN_PAIR("1hblade-low-cover-combo-enter-close-hit", 1.0f, 1.13f),
      CD_WPN_PAIR("1hbade-cover-kill-combo-hack", 1.0f, 0.0f)} },
    { "wpn-blade-cover-hack~mid", 2, 2.5f, 2.15f, 1, 2,
     { CD_WPN_PAIR("1hblade-low-cover-combo-enter-hit", 1.0f, 0.73f),
      CD_WPN_PAIR("1hbade-cover-kill-combo-hack", 1.0f, 0.0f)} },
    { "wpn-blade-cover-hack~far", 2, 2.5f, 2.15f, 1, 2,
     { CD_WPN_PAIR("1hblade-low-cover-combo-enter-left-hit", 1.0f, 1.30f),
      CD_WPN_PAIR("1hbade-cover-kill-combo-hack", 1.0f, 0.0f)} },
    // ---- kind 0 = NO surface: the UPGRADED 1h melee one-shot (the 2013 pipe moves only make sense for the upgraded bat, 2x4 and pipe; the
    //      crowbar and tomahawk need their own: TLOU2's animations for both - the crowbar: Abby's head stab finisher; the tomahawk: the one that digs
    //      it into the side and kicks them away). Never picked by the scan (kind 0 matches no surface); cd_upg_start (from t2r-ext.c upg13_start, the
    //      connected one-shot swing) plays it on a STANDING victim, which is DOWNED at the hand-off, never killed (cd_victim_begin). One clip pair each,
    //      no front / back variant (the blade): the pair is anchored on the victim and turned so the authored attacker spot points at us. The attacker-only
    //      -intro clips are left out: our own connected swing IS the run-in, the main clips start with the weapon already in the body (decoded halves,
    //      30 fps: crowbar wrist 0.18 m from the victim's head at f0, sfx-crowbar-one-hand-finisher-stab-eye f1 / -stab-extract f22; machete blade in the
    //      gut f1 (bm-knife-wound-cloth, chest_holster_attach), Ellie's left foot 0.16 m from the victim's pelvis f25-30 = the kick (sfx-machete-impact-kick
    //      f27), extract f32-33, victim thrown 2.7 m). end_s = victim pelvis < 0.25 m (f56 = 1.87 s / f51 = 1.70 s) + 0.05 s.
    //      upg-crowbar-eye-jab = 1h-blunt-abby-left-crowbar-head-jab-finisher-sync (player-1h-blunt-finishers.bin; anim-melee-abby-t2, resident, 97 f each half)
    //      upg-blade-gut-chop-kick = machete-slash-3-finisher-sync / 1h-blade-ledge-left-finisher-sync (player-1h-blade-finishers.bin; anim-melee-player-shared-t2,
    //      resident, attacker 205 f / victim 260 f)
    //      The upgraded crowbar stab is replaced by TLOU2's hammer finishers (a one-hand blunt style, not a chop): the crowbar class (crowbar, hammers, 1h clubs, pipe wrench) plays Ellie's 1h blunt / hammer finishers below; the eye jab row stays
    //      only for t2r-upg-crowbar-stab.txt next to the exe (cd_upg_start never picks it otherwise).
    { "upg-crowbar-eye-jab", 0, 0.0f, 1.92f, 0, 1,
     { CD_RAW_PAIR
      ("abby-crowbar-kill-left-head-jab--abby",
       "abby-crowbar-kill-left-head-jab--npc-f", 1.0f, 0.0f)} },
    //      The 1h blunt / hammer finishers (player-1h-blunt-finishers.bin moves 1h-blunt-left-head-stop-finisher, -left-swing-through-finisher, -quick-left,
    //      -quick-right, -back-head-bonk-finisher; finisher_anims.h). All halves RESIDENT in anim-melee-player-shared-t2 (ClipData > 128 bytes, tools/mv3/gen_moves.py
    //      Clips.resident): head-stop 66 / 66 f, swing-through 71 / 71, upward-swing 59 / 59, quick-right 72 / 57, head-bonk 38 / 51. Every main clip starts
    //      AT the blow (victim head speed peak / hammer on the head f1-f4) like the crowbar's, so the connected swing is the run-in and the -intro clips stay out.
    //      Authored attacker spot (frame 0, decoded halves, tools/ctxcam_poses.py): the four front ones 0.90-1.20 m out, 3-15 deg off the victim's facing;
    //      the head bonk 0.89 m out at 148 deg = BEHIND (cd_upg_start picks by our side of the victim's facing, round robin among the side's rows; names
    //      with "-back-" = the back rows). end_s = victim pelvis < 0.25 m + 0.05 s: f25 0.88 / f29 1.02 / f31 1.08 / f27 0.95 s; the head bonk leaves the
    //      victim on its knees (pelvis 0.49 m from f45 to the clip's end): its hand-off is there, f45 + 0.05 = 1.55 s. Left out: the back knee double bash
    //      (its victim half is STREAMED, a 0-frame stub + chunks), the knee-face stun finisher (the victim starts kneeling, head 0.62 m), the E3 hammer
    //      face break (a claw STAB to the head, sfx-hammer-finisher-head-stab, and its victim half is not on the npc-f skeleton), wall / ledge / dog kills.
    { "upg-blunt-head-stop", 0, 0.0f, 0.88f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-kill-left-head-stop--ellie",
       "ellie-1hblunt-kill-left-head-stop--npc-f", 1.0f, 0.0f)} },
    { "upg-blunt-swing-through", 0, 0.0f, 1.02f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-kill-left-horizontal-swing-through--ellie",
       "ellie-1hblunt-kill-left-horizontal-swing-through--npc-f", 1.0f,
       0.0f)} },
    { "upg-blunt-upward-swing", 0, 0.0f, 1.08f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-kill-left-upward-swing--ellie",
       "ellie-1hblunt-kill-left-upward-swing--npc-f", 1.0f, 0.0f)} },
    { "upg-blunt-quick-right", 0, 0.0f, 0.95f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-kill-quick-right--ellie",
       "ellie-1hblunt-kill-quick-right--npc-f", 1.0f, 0.0f)} },
    { "upg-blunt-back-head-bonk", 0, 0.0f, 1.55f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-sneak-kill-head-bonk--ellie",
       "ellie-1hblunt-sneak-kill-head-bonk--npc-f", 1.0f, 0.0f)} },
    { "upg-blade-gut-chop-kick", 0, 0.0f, 1.75f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-machete-kill-slash-3-gut-chop--ellie",
       "ellie-machete-kill-slash-3-gut-chop--npc-f", 1.0f, 0.0f)} },
    // ---- kind 3 / 4 / 5 = the DODGE COUNTER (dcounter.c, re/scripted-melee-1026.md): the connected exchange after a successful dodge. dmax = the attacker half's hit moment (clip seconds, the
    //      weapon-trail event of the attack clip; the contextual rows: the -hit pair begins AT contact, so the hit is 0.30 s after the start to give the victim's escape window its telegraph), end_s = the hand-off (both halves blend out together; clip seconds from the clip START). Never picked by the surface scan (kind > 2).
    //      3 = open ground (SP's player dodge counter pairs: fist-dodge-back-attack-1 -> abby-fist-dodge-back-attack--abby + -hit--npc-f ...), 4 = high wall / 5 = low cover (TLOU2's contextual combo hit
    //      pairs of the same weapon sets: the engine's probe picks the variant, a wall never cancels the exchange).
    { "dc-fist-back", 3, 0.33f, 1.25f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-dodge-back-attack--abby",
       "abby-fist-dodge-back-attack-hit--npc-f", 1.0f, 0.0f)} },
    // The counter punch has a bigger wind-up so it feels stronger and punchier: TLOU2's haymaker - its attacker-only intro (0.53 s, the
    // held pull-back) then the main pair from the contact, cut into the stagger (the victim collapses by ~1.0 s of the main clip: hand-off 0.62 s into it).
    // The intro ends exactly on the main clip's first placement in the same apReference (offline check, ctxcam_lib), so the chain is seamless; the victim is
    // held on the main clip's first frame through the intro (paired.c, dc_seg0_hold). Preferred for fists over the plain dodge-back counter when loaded.
    { "dc-fist-haymaker", 3, 0.53f, 1.15f, -1, 2,
     { CD_RAW_PAIR
      ("abby-fist-kill-right-haymaker-intro--abby",
       "abby-fist-kill-right-haymaker--npc-f", 1.0f, 0.0f),
      CD_RAW_PAIR("abby-fist-kill-right-haymaker--abby",
                  "abby-fist-kill-right-haymaker--npc-f", 1.0f, 0.0f)} },
    { "dc-fist-fw", 3, 0.57f, 1.50f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-dodge-foward-attack--abby",
       "abby-fist-dodge-foward-attack-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblunt-back", 3, 0.43f, 1.30f, 0, 1,
     { CD_RAW_PAIR
      ("abby-1hblunt-dodge-back-attack--abby",
       "abby-1hblunt-dodge-back-attack-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblunt-fw", 3, 0.43f, 1.30f, 0, 1,
     { CD_RAW_PAIR
      ("abby-1hblunt-dodge-fw-attack--abby",
       "abby-1hblunt-dodge-fw-attack-hit--npc-f", 1.0f, 0.0f)} },
    // knife (Ellie hits the attacker in the face with the bottom of her palm while the knife's in the hand): TLOU2's knife stun handle bash 2.
    // Offline (ctxcam poses): right (knife) wrist 0.20 m from the victim's head at the contact 0.37 s (solo-to-pair pose match, RMS 0.015 m), 0.43 m pull-back
    // over 0.33 s, 15.9 m/s; the knife victim half staggers (head 1.04 m at +0.5 s) and stays up - the 1hblade twin collapses, so the knife clip is used.
    // Preferred for knives over the fist haymaker (dc_plan); no knife-pak clip -> the knife falls back to the fist rows as before.
    // The knife counter attack should be more of a backhand slash/hit than just a hit on the face: SP plays switchblade-stun-breaker-bash
    // (= handle-bash-2--ellie) for 368 ms only, then switchblade-stun-breaker-bash-hit (= handle-bash-2-hit--ellie) from the contact (SP trace MARK 17,
    // 3406426.7 -> 3406794.9); the plain clip played on past 0.37 s is its swing-through, which read as a backhand. Now the same chain: wind-up cut at
    // 0.37 s, the -hit pair from the contact, the victim held on its first frame through the wind-up (dc_seg0_hold). Contact +0.04 s into the -hit pair.
    { "dc-knife-palm", 3, 0.41f, 0.97f, 3, 2,
     { CD_RAW_PAIR
      ("ellie-knife-stun-handle-bash-2--ellie",
       "ellie-knife-stun-handle-bash-2-hit--npc-f", 1.0f, 0.37f),
      CD_RAW_PAIR("ellie-knife-stun-handle-bash-2-hit--ellie",
                  "ellie-knife-stun-handle-bash-2-hit--npc-f", 1.0f,
                  0.0f)} },
    { "dc-knife-back", 3, 0.47f, 1.35f, 3, 1, { CD_RAW_PAIR("ellie-knife-dodge-back-attack", "ellie-knife-dodge-back-attack-hit--npc-m", 1.0f, 0.0f)} },        // SP's knife dodge counter
    { "dc-1hblade-back", 3, 0.47f, 1.35f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-1hblade-dodge-back-attack--ellie",
       "ellie-1hblade-dodge-back-attack-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblade-heavy", 3, 0.50f, 1.35f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-1hblade-dodge-bk-heavy-atk",
       "ellie-1hblade-dodge-bk-heavy-atk-hit--npc-m", 1.0f, 0.0f)} },
    { "dc-2h-back", 3, 0.50f, 1.40f, 2, 1,
     { CD_RAW_PAIR
      ("ellie-2hblunt-dodge-back-attack--ellie",
       "ellie-2hblunt-dodge-back-attack-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-fist-hwall", 4, 0.42f, 1.55f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-hwall-combo-1-hit--abby",
       "abby-fist-hwall-combo-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-fist-lcover", 5, 0.42f, 1.55f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-low-cover-combo-1-hit--abby",
       "abby-fist-low-cover-combo-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblunt-hwall", 4, 0.42f, 1.55f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-hwall-combo-strike-1-hit--ellie",
       "ellie-1hblunt-hwall-combo-strike-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblunt-lcover", 5, 0.42f, 1.55f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-low-cover-combo-strike-1-hit--ellie",
       "ellie-1hblunt-low-cover-combo-strike-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblade-hwall", 4, 0.42f, 1.55f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-1hblade-hwall-combo-slash-1-hit--ellie",
       "ellie-1hblade-hwall-combo-slash-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-1hblade-lcover", 5, 0.42f, 1.55f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-1hblade-low-cover-combo-slash-1-hit--ellie",
       "ellie-1hblade-low-cover-combo-slash-1-hit--npc-f", 1.0f, 0.0f)} },
    { "dc-2h-hwall", 4, 0.42f, 1.55f, 2, 1,
     { CD_RAW_PAIR
      ("ellie-2hblunt-hwall-combo-strike-1-hit--ellie",
       "ellie-2hblunt-hwall-combo-strike-1-hit--npc-f", 1.0f, 0.0f)} },
    // ---- melee SEQUENCE rows (melee_seq.c, kind 6): the synced hit pair of blow N of a melee exchange; the clips BEGIN AT THE CONTACT (dmax = the victim vox / blood event at 0.03 s), end_s = the
    //      default hand-off (melee_seq.c msq_end_s overrides it: 0.65 s fists / 0.75 s weapons); style as the takedown rows (-1 fists, 0 1h blunt, 1 1h blade, 2 2h, 3 a female life's knife)
    { "ms-fist-1", 6, 0.03f, 0.75f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-combo-punch-1-hit--abby",
       "abby-fist-combo-punch-1-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-fist-2", 6, 0.03f, 0.75f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-combo-punch-2-hit--abby",
       "abby-fist-combo-punch-2-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-fist-3", 6, 0.03f, 0.75f, -1, 1,
     { CD_RAW_PAIR
      ("abby-fist-combo-punch-3-hit--abby",
       "abby-fist-combo-punch-3-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-1hblunt-1", 6, 0.03f, 0.75f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-combo-strike-1-hit--ellie",
       "ellie-1hblunt-combo-strike-1-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-1hblunt-2", 6, 0.03f, 0.75f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-combo-strike-2-hit--ellie",
       "ellie-1hblunt-combo-strike-2-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-1hblunt-3", 6, 0.03f, 0.75f, 0, 1,
     { CD_RAW_PAIR
      ("ellie-1hblunt-combo-strike-3-hit--ellie",
       "ellie-1hblunt-combo-strike-3-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-1hblade-1", 6, 0.03f, 0.75f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-machete-combo-slash-1-hit--ellie",
       "ellie-machete-combo-slash-1-hit--npc-m", 1.0f, 0.0f)} },
    { "ms-1hblade-2", 6, 0.03f, 0.75f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-machete-combo-slash-2-hit--ellie",
       "ellie-machete-combo-slash-2-hit--npc-m", 1.0f, 0.0f)} },
    { "ms-1hblade-3", 6, 0.03f, 0.75f, 1, 1,
     { CD_RAW_PAIR
      ("ellie-machete-combo-slash-3-hit--ellie",
       "ellie-machete-combo-slash-3-hit--npc-m", 1.0f, 0.0f)} },
    { "ms-2h-1", 6, 0.03f, 0.75f, 2, 1,
     { CD_RAW_PAIR
      ("ellie-2hblunt-open-combo-1-hit--ellie",
       "ellie-2hblunt-open-combo-1-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-2h-2", 6, 0.03f, 0.75f, 2, 1,
     { CD_RAW_PAIR
      ("ellie-2hblunt-open-combo-2-hit--ellie",
       "ellie-2hblunt-open-combo-2-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-2h-3", 6, 0.03f, 0.75f, 2, 1,
     { CD_RAW_PAIR
      ("ellie-2hblunt-open-combo-3-hit--ellie",
       "ellie-2hblunt-open-combo-3-hit--npc-f", 1.0f, 0.0f)} },
    { "ms-knife-1", 6, 0.03f, 0.75f, 3, 1,
     { CD_RAW_PAIR
      ("ellie-knife-combo-slash-1-hit--ellie",
       "ellie-knife-combo-slash-1-hit--npc-m", 1.0f, 0.0f)} },
    { "ms-knife-2", 6, 0.03f, 0.75f, 3, 1,
     { CD_RAW_PAIR
      ("ellie-knife-combo-slash-2-hit--ellie",
       "ellie-knife-combo-slash-2-hit--npc-m", 1.0f, 0.0f)} },
    { "ms-knife-3", 6, 0.03f, 0.75f, 3, 1,
     { CD_RAW_PAIR
      ("ellie-knife-combo-slash-3-hit--ellie",
       "ellie-knife-combo-slash-3-hit--npc-m", 1.0f, 0.0f)} },
    // melee duel (melee_duel.c, kind 7): one row per SP -hit pair of every melee set, generated (tools/mv3/gen_duel_moves.py)
    MDU_CD_ROWS
        // melee duel finishers (kind 0 "mf-<SP move>": the lethal blow of a duel swing plays an SP player finisher, cd_fin_pick / cd_upg_go), generated (tools/mv3/gen_duel_fin.py)
        MDF_CD_ROWS MPN_CD_ROWS MPN_ESC_CD_ROWS
        // melee duel parry / clash (melee_duel_parry.c, kind 7): the SP parry pairs, generated (tools/mv3/gen_duel_parry.py)
        MDP_CD_ROWS
        // prone pull-out (melee_pullout.c, kind 8): SP's NPC-pulls-you-out-of-cover drag + the counter's success / fail pairs
    PUL_CD_ROWS
