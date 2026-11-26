"""Differential test of tools/clan_journal.py (+ clan_journal_entries.py): same engine and script format as clan_alife_harness.py, with the journal ops
(entries, perks, render, in-game entry regrouping) generated more often.  Rust side: `t2r-daemon selftest clan_journal`."""
import clan_alife_harness as H

if __name__ == "__main__":
    H.main("jn")
