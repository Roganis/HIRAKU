// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzzing (tools/fuzz.sh): an e-AMUSEMENT call as text, the HTTP message around it and its
// headers (runtime/eamuse/protocol.h): what hiraku-eamuse reads off the network before it knows
// who is asking.
#include <cstdint>
#include <map>
#include <string>

#include "eamuse/protocol.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    using namespace hk::ea;
    const std::string in((const char *)data, size);
    Call call;
    if (parse(in, call))
        for (const Request &r : call.requests) elements(r.inner);
    elements(in);
    std::map<std::string, std::string> attrs;
    bool self_closing = false;
    read_attrs(in, 0, attrs, &self_closing);
    header(in, "X-Eamuse-Info");
    header(in, "Content-Length");
    key(in.substr(0, 40));
    clear(in);
    std::string request = in;
    set_srcid(request, "0123456789ABCDEF0123");
    return 0;
}
