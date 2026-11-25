#include "common.hpp"
#include "../product_version.h"

#include <iomanip>
#include <vector>

namespace fs = std::filesystem;
namespace tools = sfx::apps::archive_tools;

namespace {
enum class Operation { extract, list, test };

void help() {
    std::cout <<
        "SFX UnZIP compatibility tool " SFX_PRODUCT_VERSION_SHORT_STRING "\n"
        "Usage: unzip [-q] [-n] archive [-d destination] [member-pattern ...]\n"
        "       unzip -l archive\n"
        "       unzip -t archive\n"
        "Extract requires an explicit -d directory and never replaces files.\n"
        "Member patterns and repeated -x patterns use * and ?.\n"
        "Options: -h, -help, -hh, --help, -v, --version, --, -q, -n, -l, -t.\n"
        "Exit codes: 0 success; 2 usage; 3 input; 4 unsafe path; 5 invalid ZIP;\n"
        "6 output/collision; 7 limit; 8 cancelled; 9 internal failure.\n"
        "Passwords/decryption, repair/salvage, overwrite, privilege and extraction\n"
        "outside the selected destination are intentionally unsupported.\n";
}

int run(const int argc, wchar_t** argv) {
    tools::prepare_console();
    if (argc == 2) {
        const std::wstring option = argv[1];
        if (option == L"-h" || option == L"-help" || option == L"-hh" ||
            option == L"--help") {
            help();
            return 0;
        }
        if (option == L"-v" || option == L"--version") {
            std::cout << "SFX UnZIP compatibility tool " SFX_PRODUCT_VERSION_SHORT_STRING "; libarchive 3.8.9; "
                         "zlib 1.3.2; Zip64; UTF-8 paths; no encryption\n";
            return 0;
        }
    }
    bool quiet = false;
    bool options = true;
    Operation operation = Operation::extract;
    fs::path archive_path;
    fs::path destination;
    std::vector<std::string> includes;
    std::vector<std::string> excludes;
    for (int index = 1; index < argc; ++index) {
        const std::wstring value = argv[index];
        if (options && value == L"--") { options = false; continue; }
        if (options && value == L"-d") {
            if (++index == argc || !destination.empty()) {
                std::cerr << "SFX UnZIP usage error: -d requires one destination\n";
                return 2;
            }
            destination = argv[index];
            continue;
        }
        if (options && value == L"-x") {
            if (++index == argc) {
                std::cerr << "SFX UnZIP usage error: -x requires one pattern\n";
                return 2;
            }
            excludes.push_back(tools::pattern(fs::path(argv[index])));
            continue;
        }
        if (options && value == L"-q") { quiet = true; continue; }
        if (options && value == L"-n") continue;
        if (options && (value == L"-l" || value == L"-t")) {
            const auto selected = value == L"-l" ? Operation::list : Operation::test;
            if (operation != Operation::extract && operation != selected) {
                std::cerr << "SFX UnZIP usage error: select only one operation\n";
                return 2;
            }
            operation = selected;
            continue;
        }
        if (options && !value.empty() && value[0] == L'-') {
            std::cerr << (tools::dangerous_switch(value)
                              ? "SFX UnZIP safety refusal: dangerous option is not supported: "
                              : "SFX UnZIP usage error: unsupported option: ")
                      << tools::pattern(value) << '\n';
            return tools::dangerous_switch(value) ? 4 : 2;
        }
        if (archive_path.empty()) archive_path = value;
        else includes.push_back(tools::pattern(fs::path(value)));
    }
    if (archive_path.empty() ||
        (operation == Operation::extract && destination.empty()) ||
        (operation != Operation::extract && !destination.empty())) {
        help();
        return 2;
    }
    const auto inspection = sfx::archive::inspect_zip(archive_path);
    int code = tools::report(inspection.result, quiet);
    if (code != 0) return code;
    std::vector<sfx::archive::FileRecord> selected;
    for (const auto& file : inspection.files) {
        if (tools::selected(file.path, includes, excludes)) selected.push_back(file);
    }
    if (operation == Operation::list) {
        if (selected.empty()) return 3;
        if (!quiet) {
            for (const auto& file : selected) {
                std::cout << std::setw(12) << file.size << "  " << file.path << '\n';
            }
            std::cout << selected.size() << " files; ZIP structure and entry data verified\n";
        }
        return 0;
    }
    if (operation == Operation::test) {
        if (selected.empty()) return 3;
        if (!quiet) std::cout << "SFX UnZIP test PASS: archive structure and entry data verified\n";
        return 0;
    }

    sfx::archive::ExtractRequest request;
    request.archive_path = archive_path;
    request.destination_root = fs::absolute(destination).lexically_normal();
    request.included_patterns = std::move(includes);
    request.excluded_patterns = std::move(excludes);
    if (!quiet) {
        request.progress = [](const std::uint64_t complete, const std::uint64_t total,
                              const std::string& path) {
            std::cout << "extracting: " << path << " (" << complete << '/' << total << ")\n";
        };
    }
    code = tools::report(sfx::archive::extract_zip(request), quiet);
    if (code == 0 && !quiet) {
        std::cout << "SFX UnZIP extraction complete; existing files were not replaced\n";
    }
    return code;
}
} // namespace

int wmain(const int argc, wchar_t** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << "SFX UnZIP internal failure: " << error.what() << '\n';
        return 9;
    }
}
