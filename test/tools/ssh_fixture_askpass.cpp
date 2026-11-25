// Test-only OpenSSH askpass helper. Contains only a disposable fixture secret.
#include <cstdio>
#include <windows.h>
int main() {
    return std::fputs(GetEnvironmentVariableA("SFX_SSH_QA_BAD_PASSWORD", nullptr, 0) != 0
        ? "Wrong-disposable-fixture-password\n" : "Sfx-test-password-123\n", stdout) < 0 ? 1 : 0;
}
