/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2013 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/util/file-cache.h"

#include <sys/mman.h>

#include <set>
#include <string>

#include "folly/String.h"
#include "hphp/util/cache/cache-manager.h"
#include "hphp/util/cache/cache-type.h"
#include "hphp/util/exception.h"
#include "hphp/util/compression.h"
#include "hphp/util/logger.h"
#include "hphp/util/util.h"

namespace HPHP {

using std::set;
using std::string;

static const short kFileCacheVersion_1 = 1;
static const short kCurrentFileCacheVersion = kFileCacheVersion_1;

///////////////////////////////////////////////////////////////////////////////

string FileCache::SourceRoot;
bool FileCache::UseNewCache;

///////////////////////////////////////////////////////////////////////////////
// helper

static bool read_bytes(FILE *f, char *buf, int len) {
  size_t nread = 0;
  while (len && (nread = fread(buf, 1, len, f)) != 0) {
    buf += nread;
    len -= nread;
  }
  return nread;
}

static bool read_bytes(char *&ptr, char *end, char *buf, int len) {
  if (ptr + len <= end) {
    memcpy(buf, ptr, len);
    ptr += len;
    return len;
  }
  return false;
}

///////////////////////////////////////////////////////////////////////////////

FileCache::FileCache()
    : m_fd(-1),
      m_size(0),
      m_addr(nullptr),
      cache_manager_(new CacheManager) {
}

FileCache::~FileCache() {
  if (UseNewCache) {
    return;
  }

  for (FileMap::iterator iter = m_files.begin(); iter != m_files.end();
       ++iter) {
    Buffer &buffer = iter->second;
    if (m_fd == -1) {
      if (buffer.data) {
        free(buffer.data);
      }
      if (buffer.cdata) {
        free(buffer.cdata);
      }
    } else {
      always_assert(buffer.data == nullptr || buffer.cdata == nullptr);
    }
  }
  if (m_fd != -1) {
    always_assert(m_addr != nullptr);
    always_assert(m_size > 0);
    munmap(m_addr, m_size);
    close(m_fd);
  }
}

string FileCache::GetRelativePath(const char *path) {
  assert(path);

  string relative = path;
  unsigned int len = SourceRoot.size();
  if (len > 0 && relative.size() > len &&
      strncmp(relative.data(), SourceRoot.c_str(), len) == 0) {
    relative = relative.substr(len);
  }
  if (!relative.empty() && relative[relative.length() - 1] == '/') {
    relative = relative.substr(0, relative.length() - 1);
  }
  return relative;
}

void FileCache::write(const char *name, bool addDirectories /* = true */) {
  assert(name && *name);
  assert(!exists(name));

  if (UseNewCache) {
    if (!cache_manager_->addEmptyEntry(name)) {
      throw Exception("Unable to add entry for %s", name);
    }

    return;
  }

  Buffer &buffer = m_files[name];
  buffer.len = -1; // PHP file
  buffer.data = nullptr;
  buffer.clen = -1;
  buffer.cdata = nullptr;

  if (addDirectories) {
    writeDirectories(name);
  }
}

void FileCache::write(const char *name, const char *fullpath) {
  assert(name && *name);
  assert(fullpath && *fullpath);
  assert(!exists(name));

  if (UseNewCache) {
    if (!cache_manager_->addFileContents(name, fullpath)) {
      throw Exception("Unable to add entry for %s (%s)", name, fullpath);
    }

    return;
  }

  struct stat sb;
  if (stat(fullpath, &sb) != 0) {
    throw Exception("Unable to stat %s: %s", fullpath,
                    folly::errnoStr(errno).c_str());
  }
  int len = sb.st_size;
  Buffer &buffer = m_files[name];
  buffer.len = len; // static file
  buffer.data = nullptr;
  buffer.clen = -1;
  buffer.cdata = nullptr;

  if (len) {
    FILE *f = fopen(fullpath, "r");
    if (f == nullptr) {
      throw Exception("Unable to open %s: %s", fullpath,
                      folly::errnoStr(errno).c_str());
    }

    char *buf = buffer.data = (char *)malloc(len);
    if (!read_bytes(f, buf, len)) {
      throw Exception("Unable to read all bytes from %s", fullpath);
    }
    fclose(f);

    if (is_compressible_file(name)) {
      int new_len = buffer.len;
      char *compressed = gzencode(buffer.data, new_len, 9, CODING_GZIP);
      if (compressed && new_len < ((buffer.len * 3) / 4)) {
        buffer.clen = new_len;
        buffer.cdata = compressed;
      } else {
        free(compressed);
      }
    }
  }

  writeDirectories(name);
}

void FileCache::save(const char *filename) {
  assert(filename && *filename);

  if (UseNewCache) {
    if (!cache_manager_->saveCache(filename)) {
      throw Exception("Unable to save cache to %s", filename);
    }

    return;
  }

  FILE *f = fopen(filename, "w");
  if (f == nullptr) {
    throw Exception("Unable to open %s: %s", filename,
                    folly::errnoStr(errno).c_str());
  }

  // write an invalid length followed by a version number
  short minus_one = -1;
  short version = kCurrentFileCacheVersion;

  fwrite(&minus_one, sizeof(minus_one), 1, f);
  fwrite(&version, sizeof(version), 1, f);

  for (auto& file : m_files) {
    short name_len = file.first.size();
    const char *name = file.first.data();
    assert(name_len);

    fwrite(&name_len, sizeof(short), 1, f);
    fwrite(name, name_len, 1, f);

    const Buffer &buffer = file.second;

    char c = buffer.cdata ? 1 : 0;
    fwrite(&c, 1, 1, f);

    if (c) {
      assert(buffer.clen > 0);
      fwrite(&buffer.clen, sizeof(int), 1, f);
      assert(buffer.cdata);
      fwrite(buffer.cdata, buffer.clen, 1, f);
      fwrite("\0", 1, 1, f);
    } else {
      fwrite(&buffer.len, sizeof(int), 1, f);
      if (buffer.len > 0) {
        assert(buffer.data);
        fwrite(buffer.data, buffer.len, 1, f);
        fwrite("\0", 1, 1, f);
      }
    }
  }

  fclose(f);
}

short FileCache::getVersion(const char *filename) {
  assert(filename && *filename);

  // Provided during the migration from the old cache to the new.
  CacheType ct;
  if (ct.isNewCache(filename)) {
    Logger::Info("Autodetected new cache format: %s", filename);
    UseNewCache = true;
    return 2;
  }

  FILE *f = fopen(filename, "r");
  if (f == nullptr) {
    throw Exception("Unable to open %s: %s", filename,
                    folly::errnoStr(errno).c_str());
  }

  short tag = -1;
  short version = -1;
  if (!read_bytes(f, (char*)&tag, sizeof(tag)) || tag > 0) return -1;
  read_bytes(f, (char*)&version, sizeof(version));
  fclose(f);
  return version;
}

void FileCache::load(const char *filename, bool onDemandUncompress,
                     short version) {
  assert(filename && *filename);

  if (UseNewCache) {
    throw Exception("Non-mmap load not supported with UseNewCache enabled");
  }

  FILE *f = fopen(filename, "r");
  if (f == nullptr) {
    throw Exception("Unable to open %s: %s", filename,
                    folly::errnoStr(errno).c_str());
  }

  if (version > 0) {
    // skip the leading -1 and the version id
    short tmp = -1;
    read_bytes(f, (char*)&tmp, sizeof(tmp));
    read_bytes(f, (char*)&tmp, sizeof(tmp));
  }

  while (true) {
    short name_len;
    if (!read_bytes(f, (char*)&name_len, sizeof(short)) || name_len <= 0) {
      if (feof(f)) break;
      throw Exception("Bad file name length in archive %s", filename);
    }
    char *name = (char *)malloc(name_len + 1);
    if (!read_bytes(f, name, name_len)) {
      free(name);
      throw Exception("Bad file name in archive %s", filename);
    }
    name[name_len] = '\0';
    string file(name, name_len);
    free(name);
    if (exists(file.c_str())) {
      throw Exception("Same file %s appeared twice in %s", file.c_str(),
                      filename);
    }

    char c; int len;
    if (!read_bytes(f, (char*)&c, 1) ||
        !read_bytes(f, (char*)&len, sizeof(int))) {
      throw Exception("Bad data length in archive %s", filename);
    }

    Buffer &buffer = m_files[file];
    buffer.len = len;
    buffer.data = nullptr;
    buffer.clen = -1;
    buffer.cdata = nullptr;

    if (len > 0) {
      buffer.data = (char *)malloc(len + 1);
      if (version > 0) {
        if (!read_bytes(f, buffer.data, len + 1)) {
          throw Exception("Bad data in archive %s", filename);
        }
        always_assert(buffer.data[len] == '\0');
      } else {
        if (!read_bytes(f, buffer.data, len)) {
          throw Exception("Bad data in archive %s", filename);
        }
        buffer.data[len] = '\0';
      }
      if (c) {
        if (onDemandUncompress) {
          buffer.clen = buffer.len;
          buffer.cdata = buffer.data;
          buffer.len = -1;
          buffer.data = nullptr;
        } else {
          int new_len = buffer.len;
          char *uncompressed = gzdecode(buffer.data, new_len);
          if (uncompressed == nullptr) {
            throw Exception("Bad compressed data in archive %s", filename);
          }

          buffer.clen = buffer.len;
          buffer.cdata = buffer.data;
          buffer.len = new_len;
          buffer.data = uncompressed;
        }
      }
    }
  }
  fclose(f);
}

void FileCache::adviseOutMemory() {
  // Not supported with the new cache.
  if (UseNewCache) {
    return;
  }

  if (posix_madvise(m_addr, m_size, POSIX_MADV_DONTNEED)) {
    Logger::Error("posix_madvise failed: %s",
                  folly::errnoStr(errno).c_str());
  }
}

void FileCache::loadMmap(const char *filename, short version) {
  assert(filename && *filename);

  // Provided during the migration from the old cache to the new.
  CacheType ct;
  if (ct.isNewCache(filename)) {
    Logger::Info("Autodetected new cache format: %s", filename);
    UseNewCache = true;
  }

  if (UseNewCache) {
    if (!cache_manager_->loadCache(filename)) {
      throw Exception("Unable to load cache from %s", filename);
    }

    return;
  }

  always_assert(version > 0);

  struct stat sbuf;
  if (stat(filename, &sbuf) == -1) {
    throw Exception("Unable to stat %s: %s", filename,
                    folly::errnoStr(errno).c_str());
  }
  m_fd = open(filename, O_RDONLY);
  if (m_fd == -1) {
    throw Exception("Unable to open %s: %s", filename,
                    folly::errnoStr(errno).c_str());
  }

  m_addr = mmap(nullptr, sbuf.st_size, PROT_READ, MAP_PRIVATE, m_fd, 0);
  if (m_addr == (void *)-1) {
    close(m_fd);
    throw Exception("Unable to mmap %s: %s", filename,
                    folly::errnoStr(errno).c_str());
  }
  m_size = sbuf.st_size;
  char *p = (char *)m_addr;
  char *e = p + m_size;

  // skip the leading -1 and the version id
  p += sizeof(short) + sizeof(short);
  while (p < e) {
    short name_len = -1;

    if (!read_bytes(p, e, (char *)(&name_len), (int)sizeof(short)) ||
        name_len <= 0) {
      throw Exception("Bad file name length in archive %s", filename);
    }
    char *name = (char *)malloc(name_len + 1);
    if (!read_bytes(p, e, name, name_len)) {
      free(name);
      throw Exception("Bad file name in archive %s", filename);
    }
    name[name_len] = '\0';
    string file(name, name_len);
    free(name);
    if (exists(file.c_str())) {
      throw Exception("Same file %s appeared twice in %s", file.c_str(),
                      filename);
    }

    char c; int len;
    if (!read_bytes(p, e, (char*)&c, 1) ||
        !read_bytes(p, e, (char*)&len, sizeof(int))) {
      throw Exception("Bad data length in archive %s", filename);
    }

    Buffer &buffer = m_files[file];
    buffer.len = len;
    buffer.data = nullptr;
    buffer.clen = -1;
    buffer.cdata = nullptr;

    if (len > 0) {
      if (p + len >= e) {
        throw Exception("Bad data in archive %s", filename);
      }
      buffer.data = p;
      p += len;
      always_assert(*p == '\0');
      p++;
      if (c) {
        buffer.clen = buffer.len;
        buffer.cdata = buffer.data;
        buffer.len = -1;
        buffer.data = nullptr;
      }
    }
  }
  adviseOutMemory();
}

bool FileCache::fileExists(const char *name,
                           bool isRelative /* = true */) const {
  if (isRelative) {
    if (UseNewCache) {
      // Original cache behavior: an empty entry is also a "file".
      return cache_manager_->fileExists(name) ||
             cache_manager_->emptyEntryExists(name);
    }

    if (name && *name) {
      FileMap::const_iterator iter = m_files.find(name);
      if (iter != m_files.end() && iter->second.len >= -1) {
        return true;
      }
    }
    return false;
  }
  return fileExists(GetRelativePath(name).c_str());
}

bool FileCache::dirExists(const char *name,
                          bool isRelative /* = true */) const {
  if (isRelative) {
    if (UseNewCache) {
      return cache_manager_->dirExists(name);
    }

    if (name && *name) {
      FileMap::const_iterator iter = m_files.find(name);
      if (iter != m_files.end() && iter->second.len == -2) {
        return true;
      }
    }
    return false;
  }
  return dirExists(GetRelativePath(name).c_str());
}

bool FileCache::exists(const char *name,
                       bool isRelative /* = true */) const {
  if (isRelative) {
    if (UseNewCache) {
      return cache_manager_->entryExists(name);
    }

    if (name && *name) {
      return m_files.find(name) != m_files.end();
    }
    return false;
  }
  return exists(GetRelativePath(name).c_str());
}

char *FileCache::read(const char *name, int &len, bool &compressed) const {
  if (name && *name) {
    if (UseNewCache) {
      const char* data;
      uint64_t data_len;
      bool data_compressed;

      if (!cache_manager_->getFileContents(name, &data, &data_len,
                                           &data_compressed)) {
        return nullptr;
      }

      compressed = data_compressed;
      len = data_len;

      // Yep, throwing away const here (for now) for API compatibility.
      return (char*) data;
    }

    FileMap::const_iterator iter = m_files.find(name);
    if (iter != m_files.end()) {
      const Buffer &buf = iter->second;
      if (compressed && buf.cdata) {
        len = buf.clen;
        assert(len > 0);
        return buf.cdata;
      }
      if (!compressed && !buf.data && buf.cdata) {
        // only compressed data available, the client has to uncompress it
        compressed = true;
        len = buf.clen;
        assert(len > 0);
        return buf.cdata;
      }
      compressed = false;
      len = buf.len;
      if (len == 0) {
        assert(buf.data == nullptr);
        return "";
      }
      return buf.data;
    }
  }
  return nullptr;
}

int64_t FileCache::fileSize(const char *name, bool isRelative) const {
  if (!name || !*name) {
    return -1;
  }

  if (isRelative) {
    if (UseNewCache) {
      uint64_t size;
      if (!cache_manager_->getUncompressedFileSize(name, &size)) {
        return -1;
      }

      return size;
    }

    FileMap::const_iterator iter = m_files.find(name);
    if (iter != m_files.end()) {
      const Buffer &buf = iter->second;
      if (buf.len >= 0) return buf.len;
      if (buf.cdata) {
        int new_len = buf.clen;
        char *uncompressed = gzdecode(buf.cdata, new_len);
        if (uncompressed == nullptr) {
          throw Exception("Bad compressed data in archive %s", name);
        } else {
          free(uncompressed);
        }
        return new_len;
      }
    }
    return -1;
  }
  return fileSize(GetRelativePath(name).c_str(), true);
}

void FileCache::dump() const {
  set<string> files;

  if (UseNewCache) {
    cache_manager_->getEntryNames(&files);
  } else {
    // For sorting purposes.
    for (auto& file: m_files) {
      files.insert(file.first);
    }
  }

  for (auto& name: files) {
    printf("%s\n", name.c_str());
  }
}

// --- Private functions.

void FileCache::writeDirectories(const char* name) {
  string sname = name;
  for (int i = 1; i < (int)sname.size(); i++) {
    if (sname[i] == '/') {
      string dir = sname.substr(0, i);
      if (!exists(dir.c_str())) {
        Buffer &buffer = m_files[dir];
        buffer.len = -2; // directory
        buffer.data = nullptr;
        buffer.clen = -2;
        buffer.cdata = nullptr;
      }
    }
  }
}

}   // namespace HPHP
