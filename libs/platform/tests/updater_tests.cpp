#include <doctest/doctest.h>

#include <algorithm>

#include "tbcad/platform/updater.h"

using namespace tbcad::platform;

TEST_CASE("versions parse and order like SemVer") {
    CHECK(parseVersion("v0.1.0-test.7").valid);
    CHECK(parseVersion("0.1.0-test.7").pre == "test.7");
    CHECK_FALSE(parseVersion("banana").valid);
    CHECK_FALSE(parseVersion("").valid);
    CHECK(compareVersions(parseVersion("0.1.0-test.7"), parseVersion("0.1.0-test.8")) < 0);
    CHECK(compareVersions(parseVersion("0.1.0-test.9"), parseVersion("0.1.0-test.10")) < 0);
    CHECK(compareVersions(parseVersion("0.1.0-test.8"), parseVersion("0.1.0")) < 0);
    CHECK(compareVersions(parseVersion("0.2.0"), parseVersion("0.1.9")) > 0);
    CHECK(compareVersions(parseVersion("1.0"), parseVersion("1.0.0")) == 0);
}

namespace {
const char* kFeed = R"([
 {"tag_name":"v0.1.0-test.8","draft":false,"prerelease":true,"html_url":"https://github.com/snabs1993/TBCAD/releases/tag/v0.1.0-test.8","body":"News",
  "assets":[{"name":"TBCAD-0.1.0-test.8-portable.zip","browser_download_url":"https://github.com/snabs1993/TBCAD/releases/download/v0.1.0-test.8/TBCAD-0.1.0-test.8-portable.zip","size":5},
            {"name":"TBCAD-0.1.0-test.8-setup.exe","size":1234,"digest":"sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
             "browser_download_url":"https://github.com/snabs1993/TBCAD/releases/download/v0.1.0-test.8/TBCAD-0.1.0-test.8-setup.exe"}]},
 {"tag_name":"v0.0.9","draft":false,"prerelease":false,"assets":[{"name":"TBCAD-0.0.9-setup.exe","size":1,
             "browser_download_url":"https://github.com/snabs1993/TBCAD/releases/download/v0.0.9/TBCAD-0.0.9-setup.exe"}]},
 {"tag_name":"v9.9.9","draft":true,"assets":[{"name":"TBCAD-9.9.9-setup.exe","size":1,
             "browser_download_url":"https://github.com/snabs1993/TBCAD/releases/download/v9.9.9/TBCAD-9.9.9-setup.exe"}]},
 {"tag_name":"v8.0.0","draft":false,"assets":[{"name":"TBCAD-8.0.0-setup.exe","size":1,
             "browser_download_url":"https://evil.example/TBCAD-8.0.0-setup.exe"}]}
])";
}  // namespace

TEST_CASE("release feed: drafts, foreign hosts and releases without an installer are ignored") {
    const auto r = parseReleases(kFeed);
    REQUIRE(r.size() == 2);
    CHECK(r[0].version == "0.1.0-test.8");
    CHECK(r[0].assetName == "TBCAD-0.1.0-test.8-setup.exe");
    CHECK(r[0].assetSize == 1234);
    CHECK(r[0].sha256.size() == 64);
    CHECK(r[0].prerelease);
    CHECK(parseReleases("not json").empty());
    CHECK(parseReleases("{}").empty());
}

TEST_CASE("update channel decides which releases count") {
    const auto r = parseReleases(kFeed);
    // A test build follows test builds by default.
    auto a = pickUpdate(r, "0.1.0-test.7", "auto");
    REQUIRE(a);
    CHECK(a->version == "0.1.0-test.8");
    // The stable channel never offers a test build.
    CHECK_FALSE(pickUpdate(r, "0.0.1", "stable").value_or(ReleaseInfo{}).prerelease);
    CHECK(pickUpdate(r, "0.0.1", "stable")->version == "0.0.9");
    CHECK(pickUpdate(r, "0.0.1", "test")->version == "0.1.0-test.8");
    // Nothing newer.
    CHECK_FALSE(pickUpdate(r, "0.1.0-test.8", "auto"));
    CHECK_FALSE(pickUpdate(r, "0.1.0", "test"));
    CHECK_FALSE(pickUpdate(r, "garbage", "test"));
}

TEST_CASE("sha256") {
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex(std::string(1000, 'a')) ==
          "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("installer arguments choose the install kind of the running copy") {
    const auto user = installerArguments(false);
    const auto all = installerArguments(true);
    CHECK(std::find(user.begin(), user.end(), "/CURRENTUSER") != user.end());
    CHECK(std::find(all.begin(), all.end(), "/ALLUSERS") != all.end());
    CHECK(std::find(user.begin(), user.end(), "/TBCADUPDATE=1") != user.end());
}
