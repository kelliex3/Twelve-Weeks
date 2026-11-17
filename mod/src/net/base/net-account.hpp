#pragma once

namespace net::account {

    const char* Token();
    bool HaveToken();
    const char* Name();
    void        SetName(const char* name);

}