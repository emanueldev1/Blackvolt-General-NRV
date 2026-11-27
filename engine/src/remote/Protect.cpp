#include "Protect.h"

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#endif

namespace bv::remote {

void secureWipe(void* data, std::size_t size) {
#ifdef _WIN32
    SecureZeroMemory(data, size);
#else
    volatile auto* p = static_cast<volatile uint8_t*>(data);
    while (size--) *p++ = 0;
#endif
}

#ifdef _WIN32
namespace {
std::optional<std::vector<uint8_t>> run(std::span<const uint8_t> in, bool protect) {
    DATA_BLOB src{static_cast<DWORD>(in.size()), const_cast<BYTE*>(in.data())};
    DATA_BLOB dst{};
    const BOOL ok = protect ? CryptProtectData(&src, L"Blackvolt", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst)
                            : CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst);
    if (!ok) return std::nullopt;
    std::vector<uint8_t> out(dst.pbData, dst.pbData + dst.cbData);
    SecureZeroMemory(dst.pbData, dst.cbData);
    LocalFree(dst.pbData);
    return out;
}
} // namespace
std::optional<std::vector<uint8_t>> protectForUser(std::span<const uint8_t> plain) { return run(plain, true); }
std::optional<std::vector<uint8_t>> unprotectForUser(std::span<const uint8_t> sealed) { return run(sealed, false); }
#else
// Non-Windows builds exist only for tests and tooling: identity transform.
std::optional<std::vector<uint8_t>> protectForUser(std::span<const uint8_t> plain) {
    return std::vector<uint8_t>(plain.begin(), plain.end());
}
std::optional<std::vector<uint8_t>> unprotectForUser(std::span<const uint8_t> sealed) {
    return std::vector<uint8_t>(sealed.begin(), sealed.end());
}
#endif

} // namespace bv::remote
