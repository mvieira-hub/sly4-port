// Raw DEFLATE (RFC 1951) decoder, no zlib header. Used where the game decompresses on
// the SPU (EDGE zlib), whose task the runtime replaces with native code.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sly4 {

enum class InflateResult {
    Ok,
    InputTruncated,   // the stream ends before its last block
    OutputTooSmall,   // more data than `out_cap`
    BadData,          // invalid block type, code lengths or distance
};

// Decompresses `src` into `out` (at most `out_cap` bytes). `out_len` receives the bytes
// produced and `in_used` the bytes of `src` consumed (rounded up to a whole byte).
InflateResult inflate_raw(const uint8_t* src, size_t src_len, uint8_t* out, size_t out_cap, size_t& out_len,
                          size_t& in_used);

const char* inflate_result_name(InflateResult r);

}  // namespace sly4
