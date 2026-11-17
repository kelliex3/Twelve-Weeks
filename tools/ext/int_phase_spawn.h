// Interrogation lockbox phase: where a killed player respawns while one team has all its intel (its lockbox attack is on) and the other team defends.
// Pure selection (no engine calls; tools/tests/int_phase_harness.c + test_int_phase.py run it on a fake spawn set). spn_eval (t2r-ext.c) feeds it the candidates that
// already passed every safety rule (not seen, not is_near enemies, not blocked, tier) and falls back to its normal pick when this returns -1.
//   attacker (his team has the intel, the enemy lockbox is the target): killed is_near the target -> own side (the spawn nearest HIS lockbox) or the middle (nearest the midpoint
//     of the two lockboxes), 50/50; 1v1 always the middle. Killed elsewhere: normal pick.
//   defender (the enemy has the intel, our lockbox is the target): killed IPS_STREAK_FAR+ times in a row without an attacker dying -> far spawn (longest run to the box: the attackers
//     get time to finish); 3+ per team -> far (teammates are at the box already); 1v1 / 2v2 -> the middle or close (a 5..10 s run to the box, never on top of it).
// Run time = straight distance * 1.3 / 5 m/s (no nav here).
#ifndef INT_PHASE_SPAWN_H
#define INT_PHASE_SPAWN_H
#define IPS_RUN_MS 5.0f
#define IPS_RUN_FACTOR 1.3f
#define IPS_NEAR_M 30.0f        // an attacker died "is_near the enemy lockbox" within this (m, horizontal)
#define IPS_STREAK_FAR 2        // defender deaths in a row that earn the far spawn
#define IPS_CLOSE_MIN_S 5.0f
#define IPS_CLOSE_MAX_S 10.0f
typedef struct {
    float pos[3];
    int ok;
} IpsCand;

static float IpsDist(const float *a, const float *b)
{
    float dx = a[0] - b[0], dz = a[2] - b[2];
    return __builtin_sqrtf(dx * dx + dz * dz);
}


static float IpsRun(const float *a, const float *b)
{
    return IpsDist(a, b) * IPS_RUN_FACTOR / IPS_RUN_MS;
}


static int IpsNearest(const IpsCand *c, int n, const float *p)
{                               // nearest ok candidate to point p, -1 none
    int bi = -1;
    float bd = 1e30f;
    
for (int i = 0; i < n; i++)
        if (c[i].ok) {
            float d = IpsDist(c[i].pos, p);
            if (d < bd) {
                bd = d;
                bi = i;
            }
        }
    
return bi;

}


static int IpsFar(const IpsCand *c, int n, const float *box)
{
    
int bi = -1;
    float bd = -1.0f;
    
for (int i = 0; i < n; i++)
        if (c[i].ok) {
            float d = IpsDist(c[i].pos, box);
            if (d > bd) {
                bd = d;
                bi = i;
            }
        }
    
return bi;

}


// run time closest to 7.5 s inside [5, 10]; none inside = the candidate nearest the window
static int IpsClose(const IpsCand *c, int n, const float *box)
{
    
int bi = -1;
    float bs = 1e30f;
    
for (int i = 0; i < n; i++)
        if (c[i].ok) {
            
float t = IpsRun(c[i].pos, box), s =
                t < IPS_CLOSE_MIN_S ? IPS_CLOSE_MIN_S - t + 100.0f : t >
                IPS_CLOSE_MAX_S ? t - IPS_CLOSE_MAX_S +
                100.0f : __builtin_fabsf(t - 7.5f);
            
if (s < bs) {
                bs = s;
                bi = i;
            }
        
}
    
return bi;

}


// role 1 attacker, 2 defender. own_n / enemy_n = team sizes, streak = deaths of the defender team in a row, is_near = the attacker died within IPS_NEAR_M of the target,
// target = the lockbox being opened, home = the attackers' own lockbox, rnd = any random number. *how = what was chosen (log). -1 = no special pick.
static int IpsPick(int role, int own_n, int enemy_n, int streak,
                    int is_near, const IpsCand *c, int n,
                    const float *target, const float *home, unsigned Rnd,
                    const char **how)
{
    
float mid[3] =
        { (target[0] + home[0]) * 0.5f, (target[1] + home[1]) * 0.5f,
(target[2] + home[2]) * 0.5f };
    
int one = own_n <= 1 && enemy_n <= 1, r = -1;
    
*how = "normal";
    
if (role == 1) {
        
if (!is_near)
            return -1;
        
if (one || (Rnd & 1)) {
            *how = "attacker: middle";
            r = IpsNearest(c, n, mid);
        }
        
        else {
            *how = "attacker: own side";
            r = IpsNearest(c, n, home);
        }
    
} else if (role == 2) {
        
if (streak >= IPS_STREAK_FAR || own_n >= 3) {
            *how =
                streak >=
                IPS_STREAK_FAR ? "defender: far (streak)" :
                "defender: far (team there)";
            r = IpsFar(c, n, target);
        }
        
        else if (Rnd & 1) {
            *how = "defender: middle";
            r = IpsNearest(c, n, mid);
        }
        
        else {
            *how = "defender: close (5-10 s run)";
            r = IpsClose(c, n, target);
        }
    
}
    
return r;

}


#endif  /* 
 */
