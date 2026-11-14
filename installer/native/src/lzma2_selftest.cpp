// lzma2_selftest.cpp -- standalone driver for fc::Lzma2Decode (not linked into the installer).
//
//   lzma2_selftest <in.raw> <dictProp> <expectedSize|0> <out.bin>
//
// Exit code 0 on success, 1 on failure (reason on stderr).

#include "lzma2.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool ReadAll(const char* path, std::vector<uint8_t>& buf, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = std::string("cannot open input file: ") + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        err = std::string("cannot size input file: ") + path;
        return false;
    }
    in.seekg(0, std::ios::beg);
    buf.resize(static_cast<size_t>(size));
    if (size > 0)
        in.read(reinterpret_cast<char*>(buf.data()), size);
    if (!in) {
        err = std::string("cannot read input file: ") + path;
        return false;
    }
    return true;
}

bool WriteAll(const char* path, const std::vector<uint8_t>& buf, std::string& err)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = std::string("cannot open output file: ") + path;
        return false;
    }
    if (!buf.empty())
        out.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
    if (!out) {
        err = std::string("cannot write output file: ") + path;
        return false;
    }
    return true;
}

bool ParseU64(const char* text, uint64_t& value)
{
    if (text == nullptr || *text == '\0')
        return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0')
        return false;
    value = static_cast<uint64_t>(v);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: lzma2_selftest <in.raw> <dictProp> <expectedSize|0> <out.bin>\n");
        return 1;
    }

    uint64_t dictPropValue = 0;
    uint64_t expectedSize  = 0;
    if (!ParseU64(argv[2], dictPropValue) || dictPropValue > 255) {
        std::fprintf(stderr, "error: bad dictProp '%s'\n", argv[2]);
        return 1;
    }
    if (!ParseU64(argv[3], expectedSize)) {
        std::fprintf(stderr, "error: bad expectedSize '%s'\n", argv[3]);
        return 1;
    }

    std::vector<uint8_t> compressed;
    std::string err;
    if (!ReadAll(argv[1], compressed, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    std::vector<uint8_t> out;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = fc::Lzma2Decode(compressed.data(), compressed.size(),
                                    static_cast<uint8_t>(dictPropValue), out, expectedSize, &err);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!ok) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    if (!WriteAll(argv[4], out, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    const double mbps = (ms > 0.0) ? (static_cast<double>(out.size()) / 1e6) / (ms / 1000.0) : 0.0;
    std::printf("ok out=%llu ms=%.3f mbps=%.1f\n",
                static_cast<unsigned long long>(out.size()), ms, mbps);
    return 0;
}
