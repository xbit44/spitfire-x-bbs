#pragma once

#include "sfx/application/display.hpp"
#include <string>
#include <string_view>

namespace sfx::application {

inline std::string offline_notice(std::string_view bbs_name,
    ITrustedDisplayAssetReader* reader, GraphicsMode graphics = GraphicsMode::ascii) {
    const std::string fallback = "SF-X (" + std::string(bbs_name) +
        ") is currently offline, please call back soon.\r\n";
    if (!reader) return fallback;
    try {
        if (graphics == GraphicsMode::ascii) {
            // NONODES.ASC is literal Sysop text, not the display-code language.
            // Keep the protected path reader and bounded terminal-safe content;
            // email addresses, @@ and @TOKEN@ are all displayed unchanged.
            const auto asset = reader->read_asset("NONODES.ASC", 4096);
            if (asset.code != TrustedDisplayAssetReadCode::available ||
                asset.logical_identity != "NONODES.ASC" || asset.bytes.size() > 4096)
                return fallback;
            for (const unsigned char byte : asset.bytes) {
                if (byte < 0x20 && byte != '\r' && byte != '\n' && byte != '\t')
                    return fallback;
            }
            return asset.bytes.find_first_not_of(" \t\r\n") == std::string::npos ? fallback : asset.bytes;
        }
        TrustedDisplaySelector displays(*reader);
        TrustedDisplaySelectionRequest request;
        request.identity = TrustedDisplayIdentity::nonodes;
        request.graphics = graphics;
        request.context.bbs_name = std::string(bbs_name);
        const auto selected = displays.select(request);
        if (!selected.available()) return fallback;
        std::string body;
        for (const auto& operation : selected.program->terminal_program.operations) {
            switch (operation.kind) {
            case TerminalDisplayOperationKind::cp437_text:
            case TerminalDisplayOperationKind::ansi_sequence: body += operation.bytes; break;
            case TerminalDisplayOperationKind::logical_newline:
            case TerminalDisplayOperationKind::paging_newline: body += "\r\n"; break;
            case TerminalDisplayOperationKind::clear_screen:
                body += graphics == GraphicsMode::ansi ? "\x1b[2J\x1b[H" : "\r\n"; break;
            case TerminalDisplayOperationKind::paging_disabled:
            case TerminalDisplayOperationKind::paging_enabled:
            case TerminalDisplayOperationKind::abort_disabled:
            case TerminalDisplayOperationKind::abort_enabled: break;
            default: return fallback; // No bell, interactive pause or caller input on refusal.
            }
            if (body.size() > 4096) return fallback;
        }
        return body.find_first_not_of(" \t\r\n") == std::string::npos ? fallback : body;
    } catch (...) { return fallback; }
}

} // namespace sfx::application
