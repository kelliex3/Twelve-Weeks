import threading
from cachetools import TTLCache

class SafeCache:
    def __init__(self, ttl: int, stale_ttl: int = 86400, maxsize: int = 1024):
        self.ttl = ttl
        self.maxsize = maxsize

        self.fresh = TTLCache(maxsize=maxsize, ttl=ttl)
        self.fresh_by_ttl = {}

        self.stale = TTLCache(maxsize=maxsize, ttl=stale_ttl)
        self.global_lock = threading.Lock()
        self.key_locks = {}
    def _get_fresh_cache(self, ttl):
        if ttl is None or ttl == self.ttl:
            return self.fresh
        if ttl not in self.fresh_by_ttl:
            self.fresh_by_ttl[ttl] = TTLCache(
                maxsize=self.maxsize,
                ttl=ttl
            )
        return self.fresh_by_ttl[ttl]
    def _get_key_lock(self, key):
        with self.global_lock:
            if key not in self.key_locks:
                self.key_locks[key] = threading.Lock()
            return self.key_locks[key]
    def get(self, key, fn, *, ttl=None):
        with self.global_lock:
            fresh = self._get_fresh_cache(ttl)
            if key in fresh:
                return fresh[key]
        key_lock = self._get_key_lock(key)
        with key_lock:
            with self.global_lock:
                fresh = self._get_fresh_cache(ttl)
                if key in fresh:
                    return fresh[key]
            try:
                val = fn()
                with self.global_lock:
                    fresh[key] = val
                    self.stale[key] = val
                return val
            except Exception as e:
                with self.global_lock:
                    if key in self.stale:
                        val = self.stale[key]
                        return (
                            {**val, "stale": str(e)}
                            if isinstance(val, dict)
                            else val
                        )
                raise