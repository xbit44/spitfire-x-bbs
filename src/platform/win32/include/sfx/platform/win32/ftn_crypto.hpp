#pragma once
#include "sfx/application/rlogin.hpp"
namespace sfx::platform::win32 {
// Fresh per-answer nonce, no deterministic time/node fallback.
std::optional<std::string> ftn_cram_challenge();
// FTS1027: decode hex challenge first, HMAC-MD5 with the separate session key.
// Sensitive response is move-only/erased; never log a handshake transcript.
std::optional<application::RloginSecret> ftn_cram_response(std::string_view secret,std::string_view challenge_hex);
bool verify_ftn_cram(std::string_view secret,std::string_view challenge_hex,std::string_view response);
// Diagnostic classification only; acceptance remains exactly the same. No secret,
// digest or challenge bytes are retained in the result.
enum class FtnCramCheck { accepted, length, trailing_nul, encoding, digest_mismatch, unavailable };
FtnCramCheck check_ftn_cram(std::string_view secret,std::string_view challenge_hex,std::string_view response);
}
