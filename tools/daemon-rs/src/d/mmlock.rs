//! K3: the MatchMaker lock. `MM.lock.lock()` is the Python `with MM.lock:`; this wrapper also remembers, per thread, that the lock is held, so
//! the daemon functions that Python runs WITHOUT taking the lock (they read `MM.matches` / `MM.ended` / `MM.departed` as plain dicts, and are
//! called both from inside `with MM.lock:` and from outside: map_pool, best_host_order, team_in) can read the matchmaker state without
//! deadlocking on their own thread: `MmMutex::peek`.
use super::*;
use parking_lot::{Mutex, MutexGuard};
use std::cell::Cell;
use std::time::Duration;

thread_local! {
    static HELD: Cell<u32> = const { Cell::new(0) };
}

pub struct MmMutex(Mutex<MMState>);

pub struct MmGuard<'a>(MutexGuard<'a, MMState>);

impl<'a> std::ops::Deref for MmGuard<'a> {
    type Target = MMState;
    fn deref(&self) -> &MMState {
        &self.0
    }
}
impl<'a> std::ops::DerefMut for MmGuard<'a> {
    fn deref_mut(&mut self) -> &mut MMState {
        &mut self.0
    }
}
impl<'a> Drop for MmGuard<'a> {
    fn drop(&mut self) {
        HELD.with(|h| h.set(h.get().saturating_sub(1)));
    }
}

impl MmMutex {
    pub fn new(s: MMState) -> MmMutex {
        MmMutex(Mutex::new(s))
    }
    pub fn lock(&self) -> MmGuard<'_> {
        let g = self.0.lock();
        HELD.with(|h| h.set(h.get() + 1));
        MmGuard(g)
    }
    pub fn try_lock_for(&self, d: Duration) -> Option<MmGuard<'_>> {
        let g = self.0.try_lock_for(d)?;
        HELD.with(|h| h.set(h.get() + 1));
        Some(MmGuard(g))
    }
    /// Read the state the way Python reads `MM.matches` etc. without the lock: through the lock held by THIS thread when it holds it
    /// (the caller is inside `with MM.lock:`), else under a short lock of its own.
    pub fn peek<R>(&self, f: impl FnOnce(&MMState) -> R) -> R {
        if HELD.with(|h| h.get()) > 0 {
            // SAFETY: this thread holds the guard (HELD is per thread and the only lock is this one), so no other thread mutates the state;
            // the caller's own guard is not used for the duration of this read.
            let s: &MMState = unsafe { &*self.0.data_ptr() };
            f(s)
        } else {
            let g = self.0.lock();
            f(&g)
        }
    }
}
