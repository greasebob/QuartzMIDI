#pragma once
// Add-on loader. Each add-on is a folder under addons\ beside the exe holding a
// DLL, addon.json and addon.sig. addon.json names the DLL and its SHA-256;
// addon.sig is an ECDSA P-256 signature over addon.json's bytes. Unsigned
// add-ons are never loaded.
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "json.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace addon {

// BCRYPT_ECCPUBLIC_BLOB for P-256: the header, then X and Y, 32 bytes each.
using PublicKey = std::array<std::uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 64>;

struct Manifest {
    std::string id;        // must equal the folder name, e.g. "sheets"
    std::string name;      // display name
    std::string dll;       // file name inside the folder, never a path
    std::string sha256;    // of the DLL, lowercase hex
    std::string owner;     // licensee; empty for everyone
    std::string ends;      // YYYY-MM-DD, last day it loads; empty for no expiry
    int abi = 0;
};

struct Verified {
    Manifest manifest;
    std::filesystem::path dll;
};

inline constexpr int kAbi = 1;

namespace detail {
inline std::optional<std::vector<std::uint8_t>> ReadAll(const std::filesystem::path& path, std::uintmax_t limit) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > limit) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) && !bytes.empty()) return std::nullopt;
    return bytes;
}

struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    explicit Algorithm(const wchar_t* id) { if (BCryptOpenAlgorithmProvider(&handle, id, nullptr, 0) < 0) handle = nullptr; }
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    Algorithm(const Algorithm&) = delete; Algorithm& operator=(const Algorithm&) = delete;
};

inline std::optional<std::array<std::uint8_t, 32>> Sha256(std::span<const std::uint8_t> bytes) {
    Algorithm sha(BCRYPT_SHA256_ALGORITHM);
    std::array<std::uint8_t, 32> digest{};
    if (!sha.handle || BCryptHash(sha.handle, nullptr, 0, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
                                  digest.data(), static_cast<ULONG>(digest.size())) < 0) return std::nullopt;
    return digest;
}

inline std::string Hex(std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) { text += digits[byte >> 4]; text += digits[byte & 15]; }
    return text;
}

// UTC date as YYYY-MM-DD, so dates compare correctly as strings.
inline std::string Today() {
    SYSTEMTIME now; GetSystemTime(&now);
    char text[11];
    std::snprintf(text, sizeof text, "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    return text;
}
}

// Returns nullopt if the add-on fails any check; *why receives the reason for the log.
inline std::optional<Verified> Verify(const std::filesystem::path& folder, const PublicKey& key, std::string* why = nullptr,
                                      const std::string& today = detail::Today()) {
    const auto refuse = [&](const char* reason) { if (why) *why = reason; return std::nullopt; };
    const auto manifestBytes = detail::ReadAll(folder / L"addon.json", 64 * 1024);
    const auto signature = detail::ReadAll(folder / L"addon.sig", 64);
    if (!manifestBytes) return refuse("addon.json is missing");
    if (!signature || signature->size() != 64) return refuse("addon.sig is missing");

    const auto digest = detail::Sha256(*manifestBytes);
    detail::Algorithm ecdsa(BCRYPT_ECDSA_P256_ALGORITHM);
    if (!digest || !ecdsa.handle) return refuse("the signature could not be checked");
    BCRYPT_KEY_HANDLE imported = nullptr;
    if (BCryptImportKeyPair(ecdsa.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, &imported, const_cast<PUCHAR>(key.data()),
                            static_cast<ULONG>(key.size()), 0) < 0) return refuse("the app's key is not a key");
    const auto status = BCryptVerifySignature(imported, nullptr, const_cast<PUCHAR>(digest->data()), static_cast<ULONG>(digest->size()),
                                              const_cast<PUCHAR>(signature->data()), static_cast<ULONG>(signature->size()), 0);
    BCryptDestroyKey(imported);
    if (status < 0) return refuse("the signature is not the owner's");

    // Parse the manifest only after its signature checks out.
    Verified verified;
    try {
        const auto json = nlohmann::json::parse(manifestBytes->begin(), manifestBytes->end());
        auto& m = verified.manifest;
        m.id = json.value("id", ""); m.name = json.value("name", ""); m.dll = json.value("dll", "");
        m.sha256 = json.value("sha256", ""); m.owner = json.value("owner", ""); m.ends = json.value("ends", "");
        m.abi = json.value("abi", 0);
    } catch (const std::exception&) { return refuse("addon.json is not JSON"); }
    const auto& m = verified.manifest;
    // Stops a validly signed manifest from being copied into another add-on's folder.
    if (m.id.empty() || std::filesystem::path(m.id) != folder.filename()) return refuse("addon.json is for another add-on");
    if (m.abi != kAbi) return refuse("the add-on is for another version of the app");
    if (!m.ends.empty() && (m.ends.size() != 10 || today > m.ends)) return refuse("the add-on has ended");
    const std::filesystem::path name(std::u8string(m.dll.begin(), m.dll.end()));
    if (m.dll.empty() || name.has_parent_path() || name.is_absolute()) return refuse("addon.json names a path");
    verified.dll = folder / name;
    const auto dllBytes = detail::ReadAll(verified.dll, 256ull * 1024 * 1024);
    if (!dllBytes) return refuse("the add-on's DLL is missing");
    const auto dllDigest = detail::Sha256(*dllBytes);
    if (!dllDigest || detail::Hex(*dllDigest) != m.sha256) return refuse("the DLL is not the one that was signed");
    return verified;
}

// A loaded add-on. The DLL is opened without FILE_SHARE_WRITE from before it is
// hashed until it is mapped, so it cannot be swapped between check and load.
class Loaded {
public:
    Loaded() = default;
    Loaded(Loaded&& other) noexcept : manifest(std::move(other.manifest)), module_(other.module_) { other.module_ = nullptr; }
    Loaded& operator=(Loaded&& other) noexcept {
        if (this != &other) { Free(); manifest = std::move(other.manifest); module_ = other.module_; other.module_ = nullptr; }
        return *this;
    }
    ~Loaded() { Free(); }
    explicit operator bool() const { return module_ != nullptr; }
    template <class Function> Function* Find(const char* name) const {
        return module_ ? reinterpret_cast<Function*>(GetProcAddress(module_, name)) : nullptr;
    }
    Manifest manifest;

    static Loaded Load(const std::filesystem::path& folder, const PublicKey& key, std::string* why = nullptr) {
        Loaded loaded;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(folder / L"addon.json", ec)) { if (why) *why = "addon.json is missing"; return loaded; }
        // Read the DLL name unverified only to lock the file; Verify re-reads it.
        std::string dll;
        try {
            std::ifstream input(folder / L"addon.json", std::ios::binary);
            dll = nlohmann::json::parse(input).value("dll", "");
        } catch (const std::exception&) {}
        const std::filesystem::path name(std::u8string(dll.begin(), dll.end()));
        HANDLE held = INVALID_HANDLE_VALUE;
        if (!dll.empty() && !name.has_parent_path())
            held = CreateFileW((folder / name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (const auto verified = Verify(folder, key, why)) {
            // The verified DLL must be the locked one. Imports resolve from
            // System32 only, since nothing else in the folder is signed.
            if (held == INVALID_HANDLE_VALUE || verified->dll != folder / name) {
                if (why) *why = "the DLL could not be held";
            } else {
                loaded.module_ = LoadLibraryExW(verified->dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (loaded.module_) loaded.manifest = verified->manifest;
                else if (why) *why = "the DLL did not load";
            }
        }
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        return loaded;
    }

private:
    void Free() { if (module_) FreeLibrary(module_); module_ = nullptr; }
    HMODULE module_ = nullptr;
};

}
