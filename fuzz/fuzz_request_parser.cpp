// libFuzzer entry point for RequestParser.
// Build: make fuzz_parser
// Run:   ./fuzz/fuzz_parser fuzz/corpus/ -max_total_time=300
//
// Any crash under AddressSanitizer is a real bug. The fuzzer exercises
// all parser states: request line, headers, body, keep-alive sequencing,
// and error paths (PayloadTooLarge, BadRequest, MethodNotAllowed).

#include "core/request_parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    RequestParser parser;
    parser.feed(reinterpret_cast<const char*>(data), size);

    // If the parser reached Done, drain it and run a second request on the
    // same instance — exercises keep-alive reset path.
    if (parser.has_request()) {
        parser.get_request();
        parser.reset();
        parser.feed(reinterpret_cast<const char*>(data), size);
    }

    return 0;
}
