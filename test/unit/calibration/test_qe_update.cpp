#include "catch_amalgamated.hpp"
#include "nukex/calibration/qe_update.hpp"
#include "nukex/calibration/ed25519_verify.hpp"

#include <map>
#include <string>
#include <vector>

using namespace nukex;

// Signed fixtures. The test binary cannot sign for itself -- tweetnacl.h is
// PRIVATE to nukex4_calibration, and exposing a signing entry point purely
// for tests would put test-only code into the shipped library. Regenerate
// with: python3 tools/gen_qe_update_fixtures.py
static const char* kDbBody                = "{\"schema_version\":1,\"cameras\":{},\"filters\":{}}";
static const char* kDbSig                 = "iV/OQn86cv0rnkiKHNIeekW24qZuB7Ts4saskRBzwx5WMJU2ZZjEdVerPyWi/54UryYQjAFCm2KDlPTotjaOBQ==";
static const char* kDbSha512              = "5711f1f56c9bc9b78d5b6464bed0270788c33574e14def011804a0ead23ee2a92e611b8fc3fea3a29e6eb848b2d517446b4df72b580a24894cb1eb033a653062";
static const char* kManifestV14           = "{\"schema_version\":1,\"db_version\":14,\"db_sha512\":\"5711f1f56c9bc9b78d5b6464bed0270788c33574e14def011804a0ead23ee2a92e611b8fc3fea3a29e6eb848b2d517446b4df72b580a24894cb1eb033a653062\",\"db_bytes\":46,\"n_cameras\":61,\"n_sensors\":26,\"released_utc\":\"2026-09-10T00:00:00Z\",\"summary\":\"Adds 6 cameras.\"}";
static const char* kManifestV14Sig        = "aqpWmqervrTNw7WjOVqbs+F0SsZcAljNN9PwBChxGgypKKtQyVSRXyl6lvmx4SdjrYvfQGnkbpdYopkA/WBMBg==";
static const char* kManifestV14Bad        = "{\"schema_version\":1,\"db_version\":14,\"db_sha512\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"db_bytes\":46,\"n_cameras\":61,\"n_sensors\":26,\"released_utc\":\"2026-09-10T00:00:00Z\",\"summary\":\"Adds 6 cameras.\"}";
static const char* kManifestV14BadSig     = "PlXGdK0I8XALIoW+TAHcCwFlej+Zd9nYi6Qjvy1vsnpijha4Hdd/Aww7RhOw819yo0MIIj2QXjoV0EwZlciTBQ==";
static const char* kManifestV12           = "{\"schema_version\":1,\"db_version\":12,\"db_sha512\":\"5711f1f56c9bc9b78d5b6464bed0270788c33574e14def011804a0ead23ee2a92e611b8fc3fea3a29e6eb848b2d517446b4df72b580a24894cb1eb033a653062\",\"db_bytes\":46,\"n_cameras\":61,\"n_sensors\":26,\"released_utc\":\"2026-09-10T00:00:00Z\",\"summary\":\"Adds 6 cameras.\"}";
static const char* kManifestV12Sig        = "U2uqFl9bmnhv6JFAkgU4ORbcddeLfkyfBjp8YyhvVEAaaxPFL5BovzMJSUPDZoc8L2lb+Xw8ydY7mZf0j+r5Cg==";
static const char* kManifestV99Schema     = "{\"schema_version\":99,\"db_version\":14,\"db_sha512\":\"5711f1f56c9bc9b78d5b6464bed0270788c33574e14def011804a0ead23ee2a92e611b8fc3fea3a29e6eb848b2d517446b4df72b580a24894cb1eb033a653062\",\"db_bytes\":46,\"n_cameras\":61,\"n_sensors\":26,\"released_utc\":\"2026-09-10T00:00:00Z\",\"summary\":\"Adds 6 cameras.\"}";
static const char* kManifestV99SchemaSig  = "wFwWdZF3SkGoTPH9feSR2URK40dp7Fmr0vaCPH0Jywm/medzWCKeWt36m/OOaJsXZ957VPHDE1JpwXdjrxRtBg==";
static const char* kManifestForged        = "{\"schema_version\":1,\"db_version\":99,\"db_sha512\":\"5711f1f56c9bc9b78d5b6464bed0270788c33574e14def011804a0ead23ee2a92e611b8fc3fea3a29e6eb848b2d517446b4df72b580a24894cb1eb033a653062\",\"db_bytes\":46,\"n_cameras\":61,\"n_sensors\":26,\"released_utc\":\"2026-09-10T00:00:00Z\",\"summary\":\"Adds 6 cameras.\"}";
static const char* kJunk                  = "{not json";
static const char* kJunkSig               = "a9xDEz+ujzrSAsf1pAqE7yjMEMkhE+rOHt6uqc2kd6QcTnaAALacrMQdeZitrdnVcWGtj6TAQ44g4lAir55nAQ==";
static const char* kTestPubHex            = "143211773272d53bdcefa5a1eae7d32e99833af0ac5da2729e9063b41e6d217d";

static std::vector<unsigned char> unhex(const std::string& h) {
    std::vector<unsigned char> out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(static_cast<unsigned char>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

static const char* kBase = "https://example.invalid/repo";

class FakeFetcher : public Fetcher {
public:
    std::map<std::string, FetchResult> responses;
    std::vector<std::string>           requested;

    FetchResult get(const std::string& url) override {
        requested.push_back(url);
        auto it = responses.find(url);
        if (it == responses.end()) return { false, "", "not reachable" };
        return it->second;
    }
};

static void serve(FakeFetcher& f, const char* name,
                  const std::string& body, const std::string& sig) {
    f.responses[std::string(kBase) + "/" + name]          = { true, body, "" };
    f.responses[std::string(kBase) + "/" + name + ".sig"] = { true, sig,  "" };
}

TEST_CASE("QEUpdater: a newer signed manifest reports AVAILABLE", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPubHex);
    serve(f, "qe_manifest.json", kManifestV14, kManifestV14Sig);

    QEUpdater up(f, kBase, pub.data());
    auto r = up.check(13);

    REQUIRE(r.outcome == UpdateOutcome::AVAILABLE);
    REQUIRE(r.manifest.db_version == 14);
    REQUIRE(r.manifest.n_cameras  == 61);
    REQUIRE(r.manifest.n_sensors  == 26);
    REQUIRE(r.manifest.db_sha512  == kDbSha512);
}

TEST_CASE("QEUpdater: the installed version reports UP_TO_DATE", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPubHex);
    serve(f, "qe_manifest.json", kManifestV14, kManifestV14Sig);

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(14).outcome == UpdateOutcome::UP_TO_DATE);
}

TEST_CASE("QEUpdater: an older published version is refused as a rollback", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPubHex);
    serve(f, "qe_manifest.json", kManifestV12, kManifestV12Sig);

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(14).outcome == UpdateOutcome::REFUSED_ROLLBACK);
}

TEST_CASE("QEUpdater: a forged manifest body fails verification", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPubHex);
    // Body claims db_version 99; signature is the genuine one over v14.
    serve(f, "qe_manifest.json", kManifestForged, kManifestV14Sig);

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(13).outcome == UpdateOutcome::BAD_SIGNATURE);
}

TEST_CASE("QEUpdater: a schema newer than this build is refused", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPubHex);
    serve(f, "qe_manifest.json", kManifestV99Schema, kManifestV99SchemaSig);

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(13).outcome == UpdateOutcome::SCHEMA_TOO_NEW);
}

TEST_CASE("QEUpdater: an unreachable host is OFFLINE, not an error", "[qe_update]") {
    FakeFetcher f;                       // serves nothing
    auto pub = unhex(kTestPubHex);

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(13).outcome == UpdateOutcome::OFFLINE);
}

TEST_CASE("QEUpdater: a malformed but correctly signed manifest is reported", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPubHex);
    serve(f, "qe_manifest.json", kJunk, kJunkSig);

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(13).outcome == UpdateOutcome::MALFORMED_MANIFEST);
}

TEST_CASE("QEUpdater: check() never requests the database", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPubHex);
    serve(f, "qe_manifest.json", kManifestV14, kManifestV14Sig);

    QEUpdater up(f, kBase, pub.data());
    up.check(13);

    for (const auto& url : f.requested)
        REQUIRE(url.find("qe_database.json") == std::string::npos);
}

TEST_CASE("QEUpdater: a garbage signature is rejected without parsing the body", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPubHex);
    serve(f, "qe_manifest.json", kManifestV14, "!!!not base64!!!");

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(13).outcome == UpdateOutcome::BAD_SIGNATURE);
}
