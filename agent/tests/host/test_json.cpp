// Host-compiled unit tests for the agent's JSON codec and path policy.
//
// These run on the developer machine, not the console: the parser and the path
// normaliser are pure logic with no libnx dependency, and they are exactly the
// code an attacker reaches first (every request goes through both). Waiting for
// a devkitPro build and a reboot to find out a path check regressed is far too
// slow a loop for something this important.
//
// Build and run with `make -C agent/tests/host`.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../source/json.hpp"
#include "../../source/path.hpp"

// path.cpp is compiled straight into this binary: the tests exercise the exact
// code the console runs, not a copy of it.

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void TestJsonRoundTrip() {
    std::printf("json round-trip\n");
    json::Value v = json::Value::object();
    v.set("cmd", "fs.read");
    v.set("offset", (int64_t)4096);
    v.set("eof", true);
    std::string text = v.dump();

    json::Value parsed;
    CHECK(json::Value::parse(text, parsed));
    CHECK(parsed["cmd"].as_string() == "fs.read");
    CHECK(parsed["offset"].as_int(0) == 4096);
    CHECK(parsed["eof"].as_bool(false) == true);
}

static void TestJsonMissingKeysUseDefaults() {
    std::printf("missing keys fall back to defaults\n");
    json::Value v;
    CHECK(json::Value::parse("{\"a\":1}", v));
    CHECK(v["nope"].as_int(42) == 42);
    CHECK(v["nope"].as_string("d") == "d");
    CHECK(v["nope"].as_bool(true) == true);
    CHECK(!v.has("nope"));
    CHECK(v.has("a"));
}

static void TestJsonRejectsMalformed() {
    std::printf("malformed input is rejected, not guessed at\n");
    const char* bad[] = {
        "",  "{",  "}",  "{\"a\"}",  "{\"a\":}",  "[1,2",
        "{\"a\":1,}",  "nonsense",  "{\"a\":\"unterminated}",
    };
    for (const char* b : bad) {
        json::Value v;
        bool ok = json::Value::parse(b, v);
        if (ok) std::printf("    accepted bad input: %s\n", b);
        CHECK(!ok);
    }
}

static void TestJsonEscaping() {
    std::printf("strings survive escaping\n");
    json::Value v = json::Value::object();
    // Paths and log lines really do contain these.
    v.set("path", "/a \"quoted\" \\ back\\slash\nnewline\ttab");
    json::Value parsed;
    CHECK(json::Value::parse(v.dump(), parsed));
    CHECK(parsed["path"].as_string() ==
          "/a \"quoted\" \\ back\\slash\nnewline\ttab");
}

static void TestJsonNesting() {
    std::printf("nested objects and arrays\n");
    json::Value inner = json::Value::object();
    inner.set("size", (int64_t)7);
    json::Value arr = json::Value::array();
    arr.push(std::move(inner));
    arr.push(json::Value((int64_t)3));
    json::Value root = json::Value::object();
    root.set("entries", std::move(arr));

    json::Value p;
    CHECK(json::Value::parse(root.dump(), p));
    CHECK(p["entries"].is_array());
    const json::Array& entries = p["entries"].as_array();
    CHECK(entries.size() == 2);
    CHECK(entries[0]["size"].as_int(0) == 7);
    CHECK(entries[1].as_int(0) == 3);
}

// as_array() on a non-array must be safe, not UB: handlers reach for it on
// replies that may legitimately be missing the field.
static void TestArrayAccessorsAreSafeOnWrongTypes() {
    std::printf("array/object accessors are safe on the wrong type\n");
    json::Value v;
    CHECK(json::Value::parse("{\"a\":1}", v));
    CHECK(v["a"].as_array().empty());
    CHECK(v["missing"].as_array().empty());
    CHECK(v["missing"].as_object().empty());
    CHECK(!v["a"].is_array());
}

static void TestJsonNegativeAndLargeInts() {
    std::printf("negative and 64-bit integers\n");
    json::Value v = json::Value::object();
    v.set("neg", (int64_t)-1);
    v.set("big", (int64_t)0x7FFFFFFFFFLL);   // a plausible Switch address
    json::Value p;
    CHECK(json::Value::parse(v.dump(), p));
    CHECK(p["neg"].as_int(0) == -1);
    CHECK(p["big"].as_int(0) == 0x7FFFFFFFFFLL);
}

// --- path policy -------------------------------------------------------------
//
// This is the security boundary for every filesystem command. A regression here
// is a directory traversal, so the cases are spelled out explicitly.

static void TestPathAcceptsNormalPaths() {
    std::printf("ordinary paths are accepted and normalised\n");
    std::string out;
    CHECK(agent::NormalizePath("/", out) && out == "/");
    CHECK(agent::NormalizePath("/hbmenu.nro", out) && out == "/hbmenu.nro");
    CHECK(agent::NormalizePath("/switch/app.nro", out) && out == "/switch/app.nro");
    // Redundant separators and "." collapse away.
    CHECK(agent::NormalizePath("//switch///app.nro", out) && out == "/switch/app.nro");
    CHECK(agent::NormalizePath("/./switch/./app.nro", out) && out == "/switch/app.nro");
    CHECK(agent::NormalizePath("/switch/", out) && out == "/switch");
}

static void TestPathRejectsTraversal() {
    std::printf("traversal is rejected in every form\n");
    const char* evil[] = {
        "/..",
        "/../secret",
        "/switch/../../etc",
        "/switch/..",
        "/a/b/../../../..",
        "/..//x",
        "/./../x",
    };
    std::string out;
    for (const char* e : evil) {
        bool ok = agent::NormalizePath(e, out);
        if (ok) std::printf("    accepted traversal: %s -> %s\n", e, out.c_str());
        CHECK(!ok);
    }
}

static void TestPathRejectsRelative() {
    std::printf("relative and empty paths are rejected\n");
    std::string out;
    CHECK(!agent::NormalizePath("", out));
    CHECK(!agent::NormalizePath("relative", out));
    CHECK(!agent::NormalizePath("switch/app.nro", out));
    CHECK(!agent::NormalizePath("./x", out));
}

static void TestPathKeepsDotfilesAndDoubleDotNames() {
    std::printf("names that merely start with dots are still legal\n");
    std::string out;
    // "..foo" is a filename, not traversal - rejecting it would be wrong.
    CHECK(agent::NormalizePath("/..foo", out) && out == "/..foo");
    CHECK(agent::NormalizePath("/.hidden", out) && out == "/.hidden");
    CHECK(agent::NormalizePath("/a/..b/c", out) && out == "/a/..b/c");
}

static void TestResolveSdPathPrefixes() {
    std::printf("resolved paths land under sdmc: and nowhere else\n");
    std::string dev;
    CHECK(agent::ResolveSdPath("/switch/app.nro", dev));
    CHECK(dev == "sdmc:/switch/app.nro");
    CHECK(agent::ResolveSdPath("/", dev) && dev == "sdmc:/");
    CHECK(!agent::ResolveSdPath("/../escape", dev));
    CHECK(!agent::ResolveSdPath("nodevice", dev));
}

int main() {
    TestJsonRoundTrip();
    TestJsonMissingKeysUseDefaults();
    TestJsonRejectsMalformed();
    TestJsonEscaping();
    TestJsonNesting();
    TestJsonNegativeAndLargeInts();
    TestArrayAccessorsAreSafeOnWrongTypes();
    TestPathAcceptsNormalPaths();
    TestPathRejectsTraversal();
    TestPathRejectsRelative();
    TestPathKeepsDotfilesAndDoubleDotNames();
    TestResolveSdPathPrefixes();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
