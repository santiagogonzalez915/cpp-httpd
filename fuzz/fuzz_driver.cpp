// Standalone fuzzer driver — reads one test case from stdin and runs the parser.
// Works with any compiler; compatible with AFL++ (afl-clang-fast++ fuzz_driver.cpp).
//
// Usage (manual):
//   ./build/fuzz_driver < fuzz/corpus/get_basic.txt
//
// Usage (AFL++):
//   afl-clang-fast++ ... -o build/fuzz_driver_afl fuzz/fuzz_driver.cpp ...
//   afl-fuzz -i fuzz/corpus -o fuzz/findings -- ./build/fuzz_driver_afl

#include "core/request_parser.hpp"
#include <cstdio>
#include <vector>

int main() {
    std::vector<char> buf;
    buf.reserve(65536);
    int c;
    while ((c = fgetc(stdin)) != EOF) {
        buf.push_back(static_cast<char>(c));
    }

    RequestParser parser;
    parser.feed(buf.data(), buf.size());

    if (parser.has_request()) {
        parser.get_request();
        parser.reset();
        parser.feed(buf.data(), buf.size());
    }

    return 0;
}
