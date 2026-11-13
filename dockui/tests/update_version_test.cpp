// Version ordering used by the update check (update_client.cpp).
// Manual network check (not part of the automatic run): DockyUpdateVersionTest --check [--download <zip>]
#include <cstdio>
#include <cstring>
#include <string>
#include "update_client.h"

static int Network(int argc, char** argv) {
    namespace up = docky::update;
    const auto r = up::Check();
    std::printf("repo=%ls current=%s status=%d version=%s asset=%s size=%llu sha256=%s\n", up::Repository().c_str(), up::CurrentVersion(),
                static_cast<int>(r.status), r.release.version.c_str(), r.release.assetName.c_str(),
                static_cast<unsigned long long>(r.release.size), r.release.sha256.c_str());
    if (!r.error.empty()) std::printf("error=%ls\n", r.error.c_str());
    if (argc > 3 && std::strcmp(argv[2], "--download") == 0 && r.status == up::CheckStatus::Available) {
        std::wstring err;
        const std::string path = argv[3];
        const bool ok = up::Download(r.release, std::wstring(path.begin(), path.end()), nullptr, nullptr, err);
        std::printf("download=%d %ls\n", ok, err.c_str());
        return ok ? 0 : 1;
    }
    return r.status == up::CheckStatus::Failed ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--check") == 0) return Network(argc, argv);
    using docky::update::CompareVersions;
    int failures = 0;
    auto expect = [&](const char* a, const char* b, int sign) {
        const int got = CompareVersions(a, b);
        const int s = got < 0 ? -1 : got > 0 ? 1 : 0;
        if (s != sign) { std::printf("FAIL %s vs %s: %d, expected %d\n", a, b, s, sign); ++failures; }
    };
    expect("0.1.0", "0.1.0", 0);
    expect("v0.1.1", "0.1.0", 1);
    expect("0.1.0", "0.2", -1);
    expect("0.10.0", "0.9.9", 1);
    expect("1.0", "1.0.0", 0);
    expect("1.0.0-beta", "1.0.0", -1);
    expect("1.0.0-beta.2", "1.0.0-beta.1", 1);
    expect("1.0.0+build5", "1.0.0", 0);
    expect("2", "1.99.99", 1);
    std::printf(failures ? "update version test: %d failures\n" : "update version test: ok\n", failures);
    return failures ? 1 : 0;
}
