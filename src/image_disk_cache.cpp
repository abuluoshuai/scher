#include "image_disk_cache.h"
#include "settings.h"

#include <windows.h>
#include <wincrypt.h>

#include <filesystem>
#include <fstream>
#include <mutex>

namespace {

std::mutex g_diskMutex;  // stores/loads from worker threads and the UI thread

std::filesystem::path cacheDir() {
    static std::filesystem::path dir;
    static bool resolved = false;
    std::lock_guard<std::mutex> lock(g_diskMutex);
    if (!resolved) {
        resolved = true;
        dir = std::filesystem::path(tintaConfigDir()) / L"ImageCache";
        std::filesystem::create_directories(dir);
    }
    return dir;
}

// SHA-1 via CryptoAPI, hex encoded
std::wstring sha1Hex(const std::string& data) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::wstring hex;
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &hash)) {
            if (CryptHashData(hash, (const BYTE*)data.data(),
                              (DWORD)data.size(), 0)) {
                DWORD len = 0;
                CryptGetHashParam(hash, HP_HASHSIZE, (BYTE*)&len, &len, 0);
                BYTE digest[20] = {};
                DWORD digestLen = sizeof(digest);
                if (len <= digestLen &&
                    CryptGetHashParam(hash, HP_HASHVAL, digest, &digestLen, 0)) {
                    static const wchar_t* digits = L"0123456789abcdef";
                    for (BYTE b : digest) {
                        hex += digits[b >> 4];
                        hex += digits[b & 0xf];
                    }
                }
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(prov, 0);
    }
    return hex;
}

std::wstring sidecarPath(const std::wstring& cacheFile) {
    return cacheFile + L".url";
}

}  // namespace

std::wstring imageDiskCacheDir() { return cacheDir().wstring(); }

std::wstring imageDiskCachePath(const std::string& key) {
    return cacheDir() / (sha1Hex(key) + L".bin");
}

std::string imageDiskCacheLoad(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_diskMutex);
    std::ifstream file(std::filesystem::path(imageDiskCachePath(key)), std::ios::binary);
    if (!file) return {};
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

std::string imageDiskCacheUrl(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_diskMutex);
    std::ifstream file(std::filesystem::path(sidecarPath(imageDiskCachePath(key))), std::ios::binary);
    if (!file) return {};
    std::string line;
    std::getline(file, line);
    return line;
}

void imageDiskCacheStore(const std::string& key, const std::string& url,
                         const std::string& bytes) {
    std::lock_guard<std::mutex> lock(g_diskMutex);
    const std::wstring cacheFile = imageDiskCachePath(key);
    // Atomic write: temp file in the same directory, then rename over
    const std::wstring tmp = cacheFile + L".tmp";
    {
        std::ofstream out(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(bytes.data(), (std::streamsize)bytes.size());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, cacheFile, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        return;
    }
    std::ofstream side(std::filesystem::path(sidecarPath(cacheFile)), std::ios::binary | std::ios::trunc);
    side << url << "\n";
}
