#include "sfx/application/display.hpp"
#include "sfx/core/security.hpp"

#include <sodium.h>

#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
namespace app = sfx::application;
namespace core = sfx::core;
namespace fs = std::filesystem;

constexpr std::size_t maximum_fixture_root_bytes = 4096U;

struct FixtureManifestEntry {
    std::string_view relative_path;
    std::uintmax_t bytes;
    std::string_view sha256;
};

constexpr std::array<FixtureManifestEntry, 4> fixture_manifest{{
    {"ogsf/local-login.observed.txt", 438U,
     "967FFE65D2C4F450E865DAF7293C891DBC881B011640199BF45D634EA5FBFF28"},
    {"ogsf/startup-ready.observed.txt", 132U,
     "6295BDFF6B93AAB2AD6526B8D10EF6B1FFF361A32BFBA4C348ACC6C9300C1F88"},
    {"sfx/caller-info.ANS", 135U,
     "21622778B32DA69BA2134E4BE7287969A1A84483D9E601B3A0B17745D939E8D0"},
    {"sfx/caller-info.ASC", 194U,
     "DAC93902C3CBC267E9B48F97E462F34D865A5E9FDA4AD1D7DF3B2DF6A920A1D4"},
}};

constexpr std::string_view repository_fixture_documentation = "README.md";

void require(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string fixture(const fs::path& root, const std::string_view name) {
    const fs::path path = root / fs::path(name);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "unable to read conformance fixture: " + path.generic_string());
    }
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

std::string sha256(const std::string_view bytes) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    require(sodium_init() >= 0, "fixture SHA-256 initialization failed");
    require(
        crypto_hash_sha256(
            digest.data(),
            reinterpret_cast<const unsigned char*>(bytes.data()),
            static_cast<unsigned long long>(bytes.size())) == 0,
        "fixture SHA-256 calculation failed");

    constexpr char hexadecimal[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const unsigned char byte : digest) {
        result.push_back(hexadecimal[(byte >> 4U) & 0x0FU]);
        result.push_back(hexadecimal[byte & 0x0FU]);
    }
    return result;
}

fs::path validated_fixture_root(const char* const supplied) {
    const std::string value = supplied == nullptr ? std::string{} : std::string(supplied);
    require(!value.empty(), "fixture-root argument is empty");
    require(value.size() <= maximum_fixture_root_bytes, "fixture-root argument is overlength");

    std::error_code error;
    const fs::path root = fs::canonical(fs::path(value), error);
    require(!error && !root.empty(), "fixture root is unavailable");
    require(fs::is_directory(root, error) && !error, "fixture root is not a directory");
    return root;
}

const FixtureManifestEntry* expected_fixture(const std::string_view relative_path) {
    for (const auto& entry : fixture_manifest) {
        if (entry.relative_path == relative_path) return &entry;
    }
    return nullptr;
}

void verify_fixture_manifest(const fs::path& root) {
    const std::set<std::string> expected_directories{"ogsf", "sfx"};
    std::set<std::string> observed_files;
    std::error_code error;
    fs::recursive_directory_iterator current(root, fs::directory_options::none, error);
    require(!error, "unable to enumerate fixture root");
    const fs::recursive_directory_iterator end;

    while (current != end) {
        const fs::file_status status = current->symlink_status(error);
        require(!error, "unable to inspect fixture path");
        require(!fs::is_symlink(status), "fixture path must not be a symlink");

        const std::string relative = fs::relative(current->path(), root, error).generic_string();
        require(!error && !relative.empty(), "unable to resolve fixture path");
        if (fs::is_directory(status)) {
            require(
                expected_directories.count(relative) == 1U,
                "unexpected fixture directory: " + relative);
        } else {
            require(fs::is_regular_file(status), "unsupported fixture path: " + relative);
            if (relative == repository_fixture_documentation) {
                current.increment(error);
                require(!error, "unable to continue fixture enumeration");
                continue;
            }
            const FixtureManifestEntry* const expected = expected_fixture(relative);
            require(expected != nullptr, "unexpected fixture path: " + relative);
            require(observed_files.insert(relative).second, "duplicate fixture path: " + relative);

            const std::uintmax_t bytes = fs::file_size(current->path(), error);
            require(!error, "unable to measure fixture: " + relative);
            require(bytes == expected->bytes, "fixture byte-size mismatch: " + relative);
            const std::string content = fixture(root, relative);
            require(content.size() == expected->bytes, "fixture read was incomplete: " + relative);
            require(sha256(content) == expected->sha256, "fixture SHA-256 mismatch: " + relative);
        }

        current.increment(error);
        require(!error, "unable to continue fixture enumeration");
    }

    for (const auto& expected : fixture_manifest) {
        require(
            observed_files.count(std::string(expected.relative_path)) == 1U,
            "required fixture missing: " + std::string(expected.relative_path));
    }
}

void test_evidence_fixtures(const fs::path& root) {
    const auto ready = fixture(root, "ogsf/startup-ready.observed.txt");
    require(ready.find("Spitfire ready for use") != std::string::npos, "documented ready-state fixture");

    const auto login = fixture(root, "ogsf/local-login.observed.txt");
    require(login.find("Enter your first name:") != std::string::npos, "observed first-name prompt");
    require(login.find("Enter your last name:") != std::string::npos, "observed last-name prompt");
    require(login.find("Enter your password:") != std::string::npos, "observed password prompt");
    require(login.find("@PASSWORD@") == std::string::npos, "fixture has no password substitution");
}

void test_locked_sfx_display_fixture(const fs::path& root) {
    app::DisplayTokenRegistry registry;
    const auto source = fixture(root, "sfx/caller-info.ASC");
    const auto parsed = app::parse_display(source, app::DisplayTrust::trusted_sysop_asset, registry);
    require(parsed.valid(), "locked ASCII display fixture parses");
    require(parsed.segments.size() > 3, "fixture contains values and presentation controls");

    const auto ansi = fixture(root, "sfx/caller-info.ANS");
    const auto ansi_parsed = app::parse_display(ansi, app::DisplayTrust::trusted_sysop_asset, registry);
    require(ansi_parsed.valid(), "locked ANSI display fixture parses");

    const auto level10 = core::SecurityLevel::create(10).value();
    const auto ascii_plan = app::make_display_lookup_plan("MAIN", app::GraphicsMode::ascii, level10).value();
    require(ascii_plan.exact_security_candidate == "MAIN10.ASC", "exact-security ASCII candidate first");
    require(ascii_plan.fallback_candidate == "MAIN.ASC", "unsuffixed ASCII fallback");
    const auto ansi_plan = app::make_display_lookup_plan("main", app::GraphicsMode::ansi, level10).value();
    require(ansi_plan.exact_security_candidate == "MAIN10.ANS", "ANSI name normalized");
    require(!app::make_display_lookup_plan("../MAIN", app::GraphicsMode::ansi, level10), "display traversal rejected");
}
}  // namespace

int main(const int argc, char* argv[]) {
    try {
        require(argc == 2, "exactly one fixture-root argument is required");
        const fs::path root = validated_fixture_root(argv[1]);
        verify_fixture_manifest(root);
        test_evidence_fixtures(root);
        test_locked_sfx_display_fixture(root);
        std::cout << "all SFX foundation conformance tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX foundation conformance test failure: " << error.what() << '\n';
        return 1;
    }
}
