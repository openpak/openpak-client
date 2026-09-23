// SPDX-License-Identifier: GPL-2.0-or-later
// The built-in zip code Android's cloud saves use: a round trip, a deflated archive from another
// writer when there is zlib (argv[1]), and an entry that climbs out refused as a whole.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "openpak/platform.h"
#include "openpak/zip_store.h"

namespace fs = std::filesystem;

static int failures = 0;
static void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(int argc, char** argv) {
    const fs::path root = fs::temp_directory_path() / "openpak_zip_store_test";
    fs::remove_all(root);
    openpak::Platform::WriteFile(root / "in" / "a.bin", std::string("hello"));
    openpak::Platform::WriteFile(root / "in" / "dir" / "b.bin", std::string(1000, 'x'));

    const auto zip = openpak::ZipStore::ZipDirectory(root / "in");
    Check(!zip.empty(), "zips a directory");
    Check(openpak::ZipStore::UnzipToDirectory(zip, root / "out"), "unzips its own zip");
    Check(openpak::Platform::ReadFile(root / "out" / "a.bin") == "hello", "a.bin round trip");
    Check(openpak::Platform::ReadFile(root / "out" / "dir" / "b.bin") == std::string(1000, 'x'),
          "dir/b.bin round trip");

    // An entry named "../evil" (same length as "dir/b.bin" minus one: patch the names in place).
    auto evil = zip;
    const std::string from = "dir/b.bin", to = "../evil_";
    for (std::size_t i = 0; i + from.size() <= evil.size(); i++) {
        if (std::string(evil.begin() + i, evil.begin() + i + from.size()) == from) {
            std::copy(to.begin(), to.end(), evil.begin() + i);
            evil[i + to.size()] = 'x';
        }
    }
    Check(!openpak::ZipStore::UnzipToDirectory(evil, root / "evil"), "refuses a climbing entry");
    Check(!fs::exists(root / "evil_x"), "wrote nothing outside");

    if (argc > 1) {
        std::ifstream f(argv[1], std::ios::binary);
        const std::vector<std::uint8_t> deflated{std::istreambuf_iterator<char>(f), {}};
        Check(openpak::ZipStore::UnzipToDirectory(deflated, root / "deflated"),
              "unzips a deflated zip");
        Check(openpak::Platform::ReadFile(root / "deflated" / "save" / "data.bin") ==
                  std::string(4096, 'z'),
              "deflated content");
    }

    fs::remove_all(root);
    return failures == 0 ? 0 : 1;
}
