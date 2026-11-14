// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzzing (tools/fuzz.sh): Konami's binary XML and its LZ77, which hiraku-eamuse reads off the
// network from whoever connects (runtime/python/kbin.h). Any bytes must be refused or read, with
// no read or write outside a buffer; and what the packer wrote, the reader reads.
#include <cstdint>
#include <cstdlib>
#include <string>

#include "python/kbin.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    using namespace hk::py::kbin;
    const std::string in((const char *)data, size);
    std::string xml, bin, again;
    is_binary(in);
    if (to_text(in, xml)) from_text(xml, bin);   // a cabinet's call, read; and its text packed again
    if (from_text(in, bin) && !to_text(bin, again)) abort();   // text packed: what was written must read
    lz77_unpack(in);
    return 0;
}
