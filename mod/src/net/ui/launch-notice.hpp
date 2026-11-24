#pragma once

#include <cstdint>

// Port of tools/ext/launchnote.c: the once-per-run unfinished-state notice under
// the TWELVE WEEKS entry title. The page is pushed on the MP menu while the title
// still covers it, loadseq holds its hand-off fade until the notice is up, and the
// acknowledge button is locked for 10 s after the title goes away.
//
// It yields to the legacy t2r-ext.dll while that is actually loaded, so the ext
// cannot push the same page or consume the same clicks twice.
namespace net::ui::launch_notice {

    void Tick(uint64_t now);
    bool Busy();                     // ext LnBusy (due / pushed / up)
    bool HoldFade(uint64_t now);     // ext LnHoldFade, for loadseq's hand-off fade
    bool ClickHit();                 // ext LnClickHit click-gate seam

    void ResetRuntime();             // session reset: keep the once-per-run latch
    void ResetAll();                 // test reset: re-read t2r-notice.txt

}
