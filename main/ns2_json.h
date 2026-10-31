#pragma once
#include <cstddef>
#include <cstdint>

namespace ns2 {
// Escape a bounded device-supplied byte string. Non-ASCII bytes are escaped too,
// so malformed advertisement UTF-8 cannot invalidate a Manager response.
inline bool json_escape_bytes(const char *input, size_t input_capacity, char *out, size_t capacity) {
    if (out == nullptr || capacity == 0) return false;
    size_t used=0;
    constexpr char hex[]="0123456789abcdef";
    for (size_t i=0; input != nullptr && i<input_capacity && input[i]!=0; ++i) {
        const auto c=static_cast<uint8_t>(input[i]);
        const size_t needed=(c<0x20 || c>=0x7f)?6:((c=='"' || c=='\\')?2:1);
        if (used+needed>=capacity) { out[used]=0; return false; }
        if (needed==6) {
            out[used++]='\\';out[used++]='u';out[used++]='0';out[used++]='0';
            out[used++]=hex[c>>4];out[used++]=hex[c&15];
        } else {
            if(needed==2)out[used++]='\\';
            out[used++]=static_cast<char>(c);
        }
    }
    out[used]=0;
    return true;
}
}
