/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TVBridgeIO.h"

#include "utils/log.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>

#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace TVPLAYER;

namespace
{
constexpr const char* ENV_BRIDGE_DIR = "FLEX_KODI_BRIDGE_DIR";
constexpr const char* ENV_BRIDGE_KEYFILE = "FLEX_KODI_BRIDGE_KEYFILE";

// uid of the TV launcher (hisenseUI, flex.js); it owns the bridge directory
constexpr uid_t LAUNCHER_UID = 10009;

// frame limits (section 3.2 of TVPLAYER-DESIGN.md)
constexpr size_t MAX_JSON = 1900;
constexpr size_t MAX_FILE = 2048;
constexpr size_t MAC_HEX = 64;

const char* HEX_DIGITS = "0123456789abcdef";

bool IsLowerHex(const std::string& text, size_t from, size_t count)
{
  if (text.size() < from + count)
    return false;
  for (size_t i = from; i < from + count; ++i)
  {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

int HexValue(char c)
{
  return (c >= '0' && c <= '9') ? c - '0' : c - 'a' + 10;
}

bool WriteAll(int fd, const std::string& data)
{
  size_t done = 0;
  while (done < data.size())
  {
    const ssize_t n = write(fd, data.data() + done, data.size() - done);
    if (n < 0)
    {
      if (errno == EINTR)
        continue;
      return false;
    }
    done += static_cast<size_t>(n);
  }
  return true;
}

bool ReadUrandom(unsigned char* out, size_t count)
{
  const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  size_t done = 0;
  while (done < count)
  {
    const ssize_t n = read(fd, out + done, count - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    done += static_cast<size_t>(n);
  }
  close(fd);
  return done == count;
}
} // namespace

std::string TVPLAYER::RandomHex(size_t bytes)
{
  std::string buffer(bytes, '\0');
  unsigned char* raw = reinterpret_cast<unsigned char*>(&buffer[0]);
  if (!ReadUrandom(raw, bytes))
  {
    // not expected on Linux; random_device reads the same kernel source through getrandom()
    std::random_device rd;
    for (size_t i = 0; i < bytes; ++i)
      raw[i] = static_cast<unsigned char>(rd() & 0xff);
  }
  std::string hex;
  hex.reserve(bytes * 2);
  for (size_t i = 0; i < bytes; ++i)
  {
    hex.push_back(HEX_DIGITS[raw[i] >> 4]);
    hex.push_back(HEX_DIGITS[raw[i] & 0x0f]);
  }
  return hex;
}

bool TVPLAYER::IsPrintableAscii(const std::string& text)
{
  for (const char c : text)
  {
    if (c < 0x20 || c > 0x7e)
      return false;
  }
  return true;
}

CTVBridgeIO& CTVBridgeIO::Get()
{
  // first call from the evdev thread (tvkeys) or the TVPlayer, both after the environment is set
  static CTVBridgeIO instance;
  return instance;
}

CTVBridgeIO::CTVBridgeIO()
{
  const char* dir = std::getenv(ENV_BRIDGE_DIR);
  const char* keyFile = std::getenv(ENV_BRIDGE_KEYFILE);

  if (!dir || dir[0] != '/' || !keyFile || keyFile[0] != '/')
  {
    CLog::Log(LOGINFO, "CTVBridgeIO: no bridge to flex.js ({} / {} not set), TVPlayer and TV volume off",
              ENV_BRIDGE_DIR, ENV_BRIDGE_KEYFILE);
    return;
  }

  m_dir = dir;
  while (m_dir.size() > 1 && m_dir.back() == '/')
    m_dir.pop_back();

  m_keyOk = LoadKey(keyFile);
  CLog::Log(LOGINFO, "CTVBridgeIO: bridge directory {}, run key {}", m_dir,
            m_keyOk ? "loaded" : "missing or invalid (bridge off)");

  // the watcher's marker lives next to the key file ($R=/tmp/flexkodi.d in kodi-watch.sh)
  const std::string key(keyFile);
  const size_t slash = key.find_last_of('/');
  m_offMarker = (slash == 0 ? std::string() : key.substr(0, slash)) + "/bridge-off";
}

bool CTVBridgeIO::IsSwitchedOff() const
{
  if (m_offMarker.empty())
    return false; // no bridge configured at all: IsUsable() is false anyway
  struct stat sb{};
  if (lstat(m_offMarker.c_str(), &sb) != 0 && errno == ENOENT)
  {
    m_offLogged = false;
    return false;
  }
  // exists (any type), or cannot be checked: off, in doubt less (D20)
  if (!m_offLogged.exchange(true))
    CLog::Log(LOGWARNING, "CTVBridgeIO: {} exists, the watcher switched the bridge off: noBridge",
              m_offMarker);
  return true;
}

bool CTVBridgeIO::LoadKey(const char* path)
{
  // root-only file from kodi.sh start: regular file of uid 0, not writable for group/others,
  // exactly 32 lowercase hex characters (optionally followed by a newline)
  const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
  {
    CLog::Log(LOGWARNING, "CTVBridgeIO: run key file {} not readable ({})", path,
              std::strerror(errno));
    return false;
  }

  struct stat sb{};
  char buffer[65] = {};
  ssize_t n = -1;
  if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_uid == 0 &&
      (sb.st_mode & (S_IWGRP | S_IWOTH)) == 0 && sb.st_size <= 64)
    n = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);

  std::string text(buffer, n > 0 ? static_cast<size_t>(n) : 0);
  OPENSSL_cleanse(buffer, sizeof(buffer));
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();

  bool ok = text.size() == 2 * m_key.size() && IsLowerHex(text, 0, text.size());
  if (ok)
  {
    for (size_t i = 0; i < m_key.size(); ++i)
      m_key[i] = static_cast<unsigned char>(HexValue(text[2 * i]) << 4 | HexValue(text[2 * i + 1]));
  }
  else
  {
    CLog::Log(LOGWARNING, "CTVBridgeIO: run key file {} has the wrong owner, mode or form", path);
  }
  OPENSSL_cleanse(&text[0], text.size());
  return ok;
}

std::string CTVBridgeIO::Mac(const char* name, const std::string& json) const
{
  std::string data(name);
  data += '\n';
  data += json;

  unsigned char mac[EVP_MAX_MD_SIZE];
  unsigned int macLength = 0;
  if (!HMAC(EVP_sha256(), m_key.data(), static_cast<int>(m_key.size()),
            reinterpret_cast<const unsigned char*>(data.data()), data.size(), mac, &macLength) ||
      macLength * 2 != MAC_HEX)
    return std::string();

  std::string hex;
  hex.reserve(MAC_HEX);
  for (unsigned int i = 0; i < macLength; ++i)
  {
    hex.push_back(HEX_DIGITS[mac[i] >> 4]);
    hex.push_back(HEX_DIGITS[mac[i] & 0x0f]);
  }
  return hex;
}

int CTVBridgeIO::OpenDir() const
{
  const int dfd = open(m_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (dfd < 0)
  {
    if (!m_dirErrorLogged.exchange(true))
      CLog::Log(LOGWARNING, "CTVBridgeIO: bridge directory {} not usable ({})", m_dir,
                std::strerror(errno));
    return -1;
  }

  // the watcher (kodi-watch.sh) keeps it 10009:10009 0700 (RAM) or 10009 0755 (eMMC fallback)
  struct stat sb{};
  if (fstat(dfd, &sb) != 0 || !S_ISDIR(sb.st_mode) ||
      (sb.st_uid != 0 && sb.st_uid != LAUNCHER_UID) || (sb.st_mode & S_IWOTH) != 0)
  {
    if (!m_dirErrorLogged.exchange(true))
      CLog::Log(LOGWARNING,
                "CTVBridgeIO: bridge directory {} has the wrong owner or mode, bridge ignored",
                m_dir);
    close(dfd);
    return -1;
  }
  m_dirErrorLogged = false;
  return dfd;
}

bool CTVBridgeIO::WriteSigned(const char* name, const std::string& json)
{
  if (!IsUsable() || json.size() > MAX_JSON || !IsPrintableAscii(json))
    return false;

  const std::string mac = Mac(name, json);
  if (mac.size() != MAC_HEX)
    return false;
  const std::string frame = mac + " " + json + "\n";

  std::unique_lock<CCriticalSection> lock(m_lock);
  const int dfd = OpenDir();
  if (dfd < 0)
    return false;

  const std::string tmp = ".kp." + RandomHex(8);
  const int fd =
      openat(dfd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  // 0644: the launcher (uid 10009) reads it, the umask must not take that away
  bool ok = fd >= 0 && WriteAll(fd, frame) && fchmod(fd, 0644) == 0;
  if (fd >= 0)
    close(fd);
  // replaces a planted symlink instead of following it
  ok = ok && renameat(dfd, tmp.c_str(), dfd, name) == 0;
  if (!ok && fd >= 0)
    unlinkat(dfd, tmp.c_str(), 0);
  close(dfd);
  return ok;
}

bool CTVBridgeIO::ReadSigned(const char* name, std::string& json)
{
  if (!IsUsable())
    return false;

  const int dfd = OpenDir();
  if (dfd < 0)
    return false;
  // O_NONBLOCK: a FIFO planted under this name must not block the reader
  const int fd = openat(dfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  close(dfd);
  if (fd < 0)
    return false;

  struct stat sb{};
  char buffer[MAX_FILE + 1];
  ssize_t n = -1;
  if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size <= static_cast<off_t>(MAX_FILE))
    n = read(fd, buffer, sizeof(buffer));
  close(fd);
  if (n < static_cast<ssize_t>(MAC_HEX + 3) || n > static_cast<ssize_t>(MAX_FILE))
    return false;

  std::string raw(buffer, static_cast<size_t>(n));
  while (!raw.empty() && (raw.back() == '\n' || raw.back() == ' ' || raw.back() == '\r'))
    raw.pop_back();
  if (raw.size() < MAC_HEX + 3 || raw[MAC_HEX] != ' ' || !IsLowerHex(raw, 0, MAC_HEX) ||
      raw[MAC_HEX + 1] != '{' || raw.back() != '}' || !IsPrintableAscii(raw))
    return false;

  std::string body = raw.substr(MAC_HEX + 1);
  const std::string mac = Mac(name, body);
  if (mac.size() != MAC_HEX || CRYPTO_memcmp(mac.data(), raw.data(), MAC_HEX) != 0)
    return false;

  json = std::move(body);
  return true;
}

void CTVBridgeIO::Remove(const char* name)
{
  if (m_dir.empty())
    return;

  std::unique_lock<CCriticalSection> lock(m_lock);
  const int dfd = OpenDir();
  if (dfd < 0)
    return;
  unlinkat(dfd, name, 0);
  close(dfd);
}
