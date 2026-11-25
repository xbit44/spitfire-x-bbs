#include "common.hpp"
#include "../product_version.h"

#include <vector>

namespace fs = std::filesystem;
namespace tools = sfx::apps::archive_tools;

namespace {
void help() {
    std::cout <<
        "SFX ZIP compatibility tool " SFX_PRODUCT_VERSION_SHORT_STRING "\n"
        "Usage: zip [-q] [-r] [-0..-9] [-i pattern] [-x pattern] archive input...\n"
        "Creates a new Zip64 archive from paths relative to the current directory.\n"
        "Existing archives are never replaced. Directories require -r.\n"
        "Patterns use * and ?. Repeat -i/-x as needed.\n"
        "Options: -h, -help, -h2, --help, -v, --version, --, -q, -r, -0..-9.\n"
        "Exit codes: 0 success; 2 usage; 3 input; 4 unsafe path; 5 invalid ZIP;\n"
        "6 output/collision; 7 limit; 8 cancelled; 9 internal failure.\n"
        "Passwords/encryption, move-after-archive, repair/salvage, overwrite,\n"
        "privilege and volume-label options are intentionally unsupported.\n";
}

int run(const int argc, wchar_t** argv) {
    tools::prepare_console();
    if (argc == 2) {
        const std::wstring option = argv[1];
        if (option == L"-h" || option == L"-help" || option == L"-h2" ||
            option == L"--help") {
            help();
            return 0;
        }
        if (option == L"-v" || option == L"--version") {
            std::cout << "SFX ZIP compatibility tool " SFX_PRODUCT_VERSION_SHORT_STRING "; libarchive 3.8.9; "
                         "zlib 1.3.2; Zip64; UTF-8 paths; no encryption\n";
            return 0;
        }
    }

    bool quiet = false;
    bool recursive = false;
    bool options = true;
    int level = 6;
    std::vector<std::string> includes;
    std::vector<std::string> excludes;
    fs::path archive_path;
    std::vector<fs::path> inputs;
    for (int index = 1; index < argc; ++index) {
        const std::wstring value = argv[index];
        if (options && value == L"--") {
            options = false;
            continue;
        }
        if (options && (value == L"-i" || value == L"-x")) {
            if (++index == argc) {
                std::cerr << "SFX ZIP usage error: " << tools::pattern(value)
                          << " requires one pattern\n";
                return 2;
            }
            auto converted = tools::pattern(fs::path(argv[index]));
            (value == L"-i" ? includes : excludes).push_back(std::move(converted));
            continue;
        }
        if (options && value == L"-q") { quiet = true; continue; }
        if (options && value == L"-r") { recursive = true; continue; }
        if (options && value.size() == 2 && value[0] == L'-' &&
            value[1] >= L'0' && value[1] <= L'9') {
            level = value[1] - L'0';
            continue;
        }
        if (options && !value.empty() && value[0] == L'-') {
            std::cerr << (tools::dangerous_switch(value)
                              ? "SFX ZIP safety refusal: dangerous option is not supported: "
                              : "SFX ZIP usage error: unsupported option: ")
                      << tools::pattern(value) << '\n';
            return tools::dangerous_switch(value) ? 4 : 2;
        }
        if (archive_path.empty()) archive_path = value;
        else inputs.emplace_back(value);
    }
    if (archive_path.empty() || inputs.empty()) {
        help();
        return 2;
    }
    if (archive_path.extension().empty()) archive_path += L".zip";
    std::error_code error;
    const auto destination = fs::absolute(archive_path, error).lexically_normal();
    if (error || destination == destination.root_path()) {
        std::cerr << "SFX ZIP usage error: invalid output path\n";
        return 2;
    }
    sfx::archive::CreateZipRequest request;
    request.source_root = fs::current_path();
    request.destination_path = destination;
    request.partial_path = destination.native() + std::wstring(L".partial");
    request.input_paths = std::move(inputs);
    request.included_patterns = std::move(includes);
    request.excluded_patterns = std::move(excludes);
    request.recursive = recursive;
    request.compression_level = level;
    if (!quiet) {
        request.progress = [](const std::uint64_t complete, const std::uint64_t total,
                              const std::string& path) {
            std::cout << "adding: " << path << " (" << complete << '/' << total << ")\n";
        };
    }
    const auto result = sfx::archive::create_zip(request);
    const int code = tools::report(result, quiet);
    if (code == 0 && !quiet) {
        std::cout << "SFX ZIP created and verified: " << destination.u8string() << '\n';
    }
    return code;
}
} // namespace

int wmain(const int argc, wchar_t** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << "SFX ZIP internal failure: " << error.what() << '\n';
        return 9;
    }
}
