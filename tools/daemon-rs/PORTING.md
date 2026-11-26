# Porting rules for daemon.py and its modules

The reference is the deployed Python (host186, daemon.py md5 07ecb2ed, hostmig.py 560cb381). A port is a faithful 1:1
translation, not a redesign: same behaviour, same wire bytes, same log lines, same DB rows, same JSON.

## Layout

- `src/<module>.rs` = one Python module (`hostmig.py` -> `src/hostmig.rs`). daemon.py itself is split under `src/d/`.
- Private modules (`anticheat.py`, `attest.py`, `ident.py`, `ident_lookup.py` and the daemon glue that calls them) go to
  `private/` (git-ignored, never pushed) and are compiled with `--features private`. `src/privstub.rs` holds neutral
  stubs with the same signatures for the public build.
- Keep a `// py: file.py:LINE name` comment above every ported function and class, so a reviewer can diff them.

## Translation

| Python | Rust |
|---|---|
| `time.time()` (daemon.py's steady clock and the modules' wall clock) | `crate::clock::now()` |
| `time.monotonic()` | `crate::clock::mono()` |
| `time.strftime('%H:%M:%S')` | `crate::clock::hms_now()` |
| `secrets.*`, `random.*` (global) | `crate::rnd::*` (pinned by `T2R_TEST_SEED` in tests) |
| seeded `random.Random(x)` | `t2r_clanstory::pyrand::PyRandom` |
| `json.dumps` | `crate::py::dumps*` (exact separators, `ensure_ascii`, float repr) on `serde_json::Value` (preserve_order) |
| `str(float)` / `repr(float)` in text | `crate::py::f(x)` |
| `round(x, n)` / `round(x)` | `crate::py::round_n` / `crate::py::round0` |
| `"%.1f" % x` | `crate::py::fixed(x, 1)` |
| `str(True)` | `crate::py::b(v)` |
| `repr(s)` | `crate::py::repr_str(s)` |
| `clean_str(s, n)` | `crate::py::clean_str` |
| `log(a, b, c)` | `dlog!(a, b, c)` (each argument formatted like Python `str()`: use the helpers above for floats, bools, lists) |
| `evt(kind, ...)` | `evt!(kind, ...)` |
| `dict` (iteration order visible anywhere: output, log, first match, `next(iter(..))`) | `indexmap::IndexMap` |
| `dict` (lookups only) | `HashMap` |
| `set` iterated into output | sort exactly as Python would (`sorted(...)`), or IndexSet if Python relied on insertion order; Python's set order for str is hash-random, so the Python code never relies on it (if it does: note it) |
| `int` | `i64` (u64 for Steam ids) |
| `bytes` | `Vec<u8>` / `&[u8]` |
| a class with `self.lock = threading.Lock()` | `pub struct X { inner: parking_lot::Mutex<XInner> }`, methods take `&self` and lock like the Python `with self.lock:` |
| `threading.RLock` | `parking_lot::ReentrantMutex<RefCell<..>>` or split into `fn x(&self)` (locks) + `fn x_locked(inner: &mut Inner)` (called with the lock held) |
| module globals | fields of a struct behind a `parking_lot::Mutex` in a `static` (`std::sync::LazyLock` / `OnceLock`) |
| `threading.Thread` / `Timer` | `std::thread::Builder::new().name(..).stack_size(256 * 1024).spawn(..)` |
| `os.environ.get("T2R_X", "5")` | `crate::env_parse("T2R_X", 5.0)` / `crate::env_str` / `crate::env_on("T2R_X", true)` (read once into a `LazyLock` config) |
| `except Exception: pass` | ignore the error the same way; never `unwrap()` / `expect()` / index without a bounds check on input: a panic aborts the process and drops every player |
| `None` | `Option` |

Watch for:
- Python truthiness (`if x:` on 0, "", [], {}, None).
- Python slicing never raises (`b[3:67]` on a short buffer is just shorter): use `crate::util::sl(b, a, z)`.
- `int.from_bytes(b, "little")` on a short slice = the value of the bytes present.
- `struct.unpack` raises on a short buffer: the Python code usually catches it; keep the same fallback.
- `str.split()` (no argument) splits on runs of whitespace and drops empty parts; `split(" ")` keeps them.
- `sorted` is stable; `key=` ties keep input order. `max`/`min` return the FIRST maximal element.
- Integer division `//` and `%` floor toward minus infinity (`crate::py::floordiv`, `pymod`, `fmod`).
- Float formatting in f-strings: `f"{x:.0f}"` rounds half to even.
- `dict.get(k, d)` default evaluation, `setdefault`, `pop(k, None)`.

## Verification

Every module ships a differential test: `verify/py/<module>_harness.py` drives the live Python module with a scripted
input (JSON lines on stdin, one call per line, with the frozen time) and prints one JSON line per result;
`t2r-daemon selftest <module>` reads the same script and must print the same bytes. Scripts are generated
(random but seeded) to hit every branch. Run both on sfo3 (`~/rsverify/daemon/`) and `cmp` the outputs.
A module is done when the outputs are identical on at least 10 seeds x 2,000 calls (or every branch for small modules).

## Float math (libm)
musl libm is not glibc libm: `log10` differed by 1 ulp in the Elo seed (K1 ported glibc's as `glibc_log10`). Python on the server uses
glibc. Any transcendental (`math.log`, `log10`, `exp`, `pow`, trig) whose result is stored, printed or compared must use a glibc-exact port
(or be proven identical over the inputs by the differential test). `sqrt`, `+ - * /`, `floor`, `round` are exact in both.

## Randomness: every draw names its Python call site
`crate::rnd::*` takes a key first: `rnd::randbelow("daemon:vote_options", n)`, `rnd::token_hex("lobbybook:_new", 4)`.
key = "<module>:<function>" of the Python function that calls `secrets.*` / `random.*` ("daemon" = daemon.py, "<module>" = module-level code).
With `T2R_TEST_SEED_STREAMS=1` (the wire-level bot test, verify/bot/env.sh + verify/py/seedhook.py) each key has its own stream
`random.Random("<seed>/<key>")`. An unseeded Python `random.Random()` instance = `rnd::unseeded()` (= Random(seed + 1) when pinned).
`T2R_TEST_EPOCH=<unix s>`: `clock::now()` / `wall_time()` start at that value.
