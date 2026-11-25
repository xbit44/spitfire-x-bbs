#pragma once
#include "sfx/application/display.hpp"
#include <filesystem>
#include <memory>
#ifdef _WIN32
#include "sfx/platform/win32/trusted_display_asset_reader.hpp"
#else
#include "sfx/platform/linux/trusted_display_asset_reader.hpp"
#endif

namespace sfx::apps::runtime {
inline std::unique_ptr<application::ITrustedDisplayAssetReader> open_native_display_reader(const std::filesystem::path& root) {
#ifdef _WIN32
    return platform::win32::Win32TrustedDisplayAssetReader::open(root.wstring());
#else
    return platform::linux_host::LinuxTrustedDisplayAssetReader::open(root);
#endif
}
// The same welcome sequence, acknowledgement and include/output limits are
// used by native Windows and Linux callers. Only the file boundary differs.
class AlphaTrustedDisplays final : public application::ITrustedDisplaySelectionPort {
public:
    explicit AlphaTrustedDisplays(std::filesystem::path root):root_(std::move(root)) {}
    application::TrustedDisplaySelectionResult select(
        const application::TrustedDisplaySelectionRequest& request) override {
        if (request.identity == application::TrustedDisplayIdentity::welcome1 &&
            request.graphics == application::GraphicsMode::ansi) {
            ansi_selected_ = true;
        }
        using namespace application;
        try {
#ifdef _WIN32
            auto reader=open_native_display_reader(root_/"Display");
#else
            auto reader=open_native_display_reader(root_/"display");
#endif
            if(!reader) return {TrustedDisplaySelectionCode::unavailable,{}};
            TrustedDisplaySelector selector(*reader);
            if(request.identity!=TrustedDisplayIdentity::welcome1) {
                auto selected=selector.select(request);
                if(selected.available() && (request.identity==TrustedDisplayIdentity::all ||
                    request.identity==TrustedDisplayIdentity::newuser) &&
                    !prepare_acknowledged_display(selected.program->terminal_program,request.graphics))
                    return {TrustedDisplaySelectionCode::invalid,{}};
                return selected;
            }
            // Numeric order, gaps allowed, with one display's aggregate limits
            // across the entire sequence. No authentication/security suffixes.
            std::optional<RenderedTrustedDisplayProgram> combined;
            for(unsigned number=1;number<=99;++number) {
                auto part=request;part.welcome_number=static_cast<std::uint8_t>(number);
                auto selected=selector.select(part);
                if(!selected.available()) continue;
                auto& next=*selected.program;
                // Every welcome is a separate, acknowledged screen. Plain mode
                // scroll-clears without ANSI; authored ANSI remains untouched.
                auto& program=next.terminal_program;
                if(!prepare_acknowledged_display(program,request.graphics)) continue;
                if(!valid_rendered_trusted_display_program(next)) continue;
                if(!combined) {combined=std::move(next);combined->selected_logical_asset=request.graphics==GraphicsMode::ansi?"WELCOME1.ANS":"WELCOME1.ASC";continue;}
                if(next.aggregate_source_bytes>kMaximumTrustedDisplayAggregateSourceBytes-combined->aggregate_source_bytes ||
                    next.resolved_include_count>kMaximumTrustedDisplayIncludeInstances-combined->resolved_include_count ||
                    next.terminal_program.output_byte_count>kTerminalMaximumDisplayOutputBytes-combined->terminal_program.output_byte_count ||
                    combined->terminal_program.operations.size()>=kTerminalMaximumDisplayOperations ||
                    next.terminal_program.operations.size()>kTerminalMaximumDisplayOperations-combined->terminal_program.operations.size()-1U) break;
                combined->aggregate_source_bytes+=next.aggregate_source_bytes;
                combined->resolved_include_count+=next.resolved_include_count;
                combined->terminal_program.output_byte_count+=next.terminal_program.output_byte_count;
                // Explicit NOABORT is local
                // to one asset and must not suppress a later welcome's pause.
                combined->terminal_program.operations.push_back({TerminalDisplayOperationKind::abort_enabled,{}});
                combined->terminal_program.operations.insert(combined->terminal_program.operations.end(),
                    next.terminal_program.operations.begin(),next.terminal_program.operations.end());
            }
            return combined ? TrustedDisplaySelectionResult{TrustedDisplaySelectionCode::available,std::move(combined)} :
                TrustedDisplaySelectionResult{TrustedDisplaySelectionCode::not_found,{}};
        } catch(...) {return {TrustedDisplaySelectionCode::unavailable,{}};}
    }

    bool ansi_selected() const noexcept { return ansi_selected_; }

private:
    std::filesystem::path root_;
    bool ansi_selected_{false};
};


} // namespace sfx::apps::runtime
