# Environment of the differential tests = the live daemon's environment (build hostlag-1125, host186) plus the test pins.
# Sourced by run_one.sh for BOTH daemons (Python and Rust). Paths come from the caller: T2R_DB, T2R_LOG_PATH, T2R_PORT, T2R_ADMIN_PORT.
export T2R_HOST=127.0.0.1
export T2R_MATCH_SIZE=8
export T2R_MIN_PLAYERS=2
export T2R_FILL_WAIT=15
export T2R_MAP=rotate
export T2R_MAP_POOL=to1-forest,to1-shipyard,to1-resort,to1-seraphite-camp,to1-jesse-houses,to1-custom-map-1,to1-train-station,to1-wilds-houses,to1-capitol-shops,to1-into-the-city,to1-overpass,to1-jackson-market,to1-p1-bookstore,to1-p1-plaza
export T2R_INT_KILLS_PER_INFO=0
export T2R_MAP_VOTE=1
export T2R_HOST_MIGRATION=1
export T2R_HOST_REPICK=2
export T2R_REQUIRE_VERSION=1
export T2R_PING_KICK_MS=190
export T2R_NAME_CACHE_S=120
export T2R_PRIVATE=0
export T2R_MAX_CONNS=900
export T2R_DB_BACKUP_S=10800
# test pins (not in the live environment). A Rust daemon run through run_one.sh must honour all of them:
#   T2R_TEST_SEED   pinned random stream (rnd.rs); T2R_TEST_SEED_STREAMS=1  one stream per calling function "<module>:<function>" ("daemon" for daemon.py); T2R_TEST_EPOCH  wall clock at start = this value, then monotonic (clock.rs: now() and
#   wall_time() = EPOCH + seconds since start; the Python side is verify/py/seedhook.py); T2R_VERSION  server version string (no version.txt in
#   the test folder); T2R_UDP=0  no UDP relay; T2R_LB_CACHE_S / T2R_SEARCH_CACHE_S = 0  the reply caches (5 s / 1 s live) off, so a reply never
#   depends on how fast the bots are; T2R_PRE_COUNTDOWN / T2R_VOTE_SECS / T2R_COUNTDOWN  shorter match-start stages (T2R_FAST=0 = live length).
#   T2R_DB / T2R_LOG_PATH / T2R_PORT / T2R_ADMIN_PORT come from run_one.sh; every file next to the database (admin-secret.txt, client_versions.json,
#   linux_clients.json, bans.txt, t2r-status.json, live-matches.json) is read from / written to that folder.
export T2R_UDP=0                      # the UDP relay is not part of this comparison
export T2R_VERSION=cz1117v            # live reads version.txt next to daemon.py; the test folder has none, so the same string comes from the environment
export T2R_TEST_SEED=${T2R_TEST_SEED:-1125}
export T2R_TEST_SEED_STREAMS=1       # one random stream per calling function (see py/seedhook.py): timer-driven draws (lobby ids) must not shift the others
export PYTHONHASHSEED=0
export T2R_TEST_EPOCH=${T2R_TEST_EPOCH:-1794999962}   # wall clock of the daemon process at start (mod 60 = 2: the 60 s clan-feed slot turns at ~58 s)
export T2R_LB_CACHE_S=0              # leaderboard reply cache (5 s live): off, so a reply never depends on how fast the bots are
export T2R_SEARCH_CACHE_S=0          # searching-list cache (1 s live): same
# FAST=1 (set by run_all.sh unless T2R_FAST=0): shorter match-start stages so a match takes ~25 s instead of ~60 s. The stage lengths are
# plain daemon settings (T2R_PRE_COUNTDOWN / T2R_VOTE_SECS / T2R_COUNTDOWN); the frame sequence has the same shape, only fewer 0x0c steps.
if [ "${T2R_FAST:-1}" = "1" ]; then
  export T2R_PRE_COUNTDOWN=2
  export T2R_VOTE_SECS=10
  export T2R_COUNTDOWN=4
fi
