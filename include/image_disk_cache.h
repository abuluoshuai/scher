#ifndef TINTA_IMAGE_DISK_CACHE_H
#define TINTA_IMAGE_DISK_CACHE_H

#include <string>

// Persistent HTTP(S) image cache: downloaded images land in
// <configDir>\ImageCache\<sha1(url)>.bin together with a tiny sidecar
// recording the source URL and the original remote size. On the next
// access the cached pixels show immediately while a fresh download runs
// in the background; the two are compared byte-for-byte and the cache is
// refreshed only when they differ.
std::wstring imageDiskCacheDir();

// SHA-1 of the UTF-8 key, hex encoded (40 chars). Deterministic and
// collision-safe for cache filenames.
std::wstring imageDiskCachePath(const std::string& key);

// The bytes cached on disk for this key, or empty when missing.
std::string imageDiskCacheLoad(const std::string& key);

// Store bytes + a sidecar (url + content hash). Atomic via temp file.
void imageDiskCacheStore(const std::string& key, const std::string& url,
                         const std::string& bytes);

// The URL recorded for this cache entry (empty when absent).
std::string imageDiskCacheUrl(const std::string& key);

#endif
