/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/CriticalSection.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <string>

namespace TVPLAYER
{

/*!
 * \brief Signed small files shared with the TV launcher mod flex.js (TVPlayer bridge, tvkeys).
 *
 * Frame: "<64 hex HMAC-SHA256(key, name + "\n" + json)> <json>\n", printable ASCII only,
 * at most 2048 bytes. Both sides drop anything that does not match frame and signature.
 *
 * Directory: FLEX_KODI_BRIDGE_DIR, always the real path (/tmp/flexkodi-rt/b), never the
 * flexkodi/rt symlink flex.js uses. It must be a directory of root or uid 10009 that is not
 * writable for others.
 *
 * Key: 16 bytes per Kodi run, read once from the root-only file FLEX_KODI_BRIDGE_KEYFILE
 * (32 hex characters, written by kodi.sh start). There is no secret in the environment and no
 * unsetenv() in this multi-threaded process. Neither the key nor anything derived from it is
 * ever logged.
 *
 * Writing is atomic: a temporary ".kp.<random>" in the directory (O_EXCL|O_NOFOLLOW), then
 * renameat() onto the name, which replaces a planted symlink instead of following it. Reading
 * uses O_NOFOLLOW|O_NONBLOCK and only accepts a small regular file.
 *
 * Used by the TVPlayer thread and the evdev reader thread (tvkeys/tvack).
 */
class CTVBridgeIO
{
public:
  static CTVBridgeIO& Get();

  //! key file valid and bridge directory configured
  bool IsUsable() const { return m_keyOk && !m_dir.empty(); }

  /*!
   * \brief The watcher switched the bridge off for this boot (D20, section 6): the marker
   *        "bridge-off" exists next to the key file (/tmp/flexkodi.d/bridge-off, root 0700
   *        directory, written by kodi-watch.sh bridge_off). One lstat() per call, so it also
   *        catches a switch-off during a Kodi run. Not part of IsUsable(): reading and writing
   *        stay possible (a running playback still gets its ctl stop:1).
   */
  bool IsSwitchedOff() const;

  //! json: compact printable ASCII, at most 1900 bytes
  bool WriteSigned(const char* name, const std::string& json);

  //! false: missing, too large, not a regular file, bad frame or signature
  bool ReadSigned(const char* name, std::string& json);

  //! unlinkat(dirfd, name, 0); missing is fine
  void Remove(const char* name);

private:
  CTVBridgeIO();
  CTVBridgeIO(const CTVBridgeIO&) = delete;
  CTVBridgeIO& operator=(const CTVBridgeIO&) = delete;

  bool LoadKey(const char* path);
  std::string Mac(const char* name, const std::string& json) const;
  int OpenDir() const;

  std::array<unsigned char, 16> m_key{};
  bool m_keyOk = false;
  std::string m_dir;
  std::string m_offMarker; //!< <directory of the key file>/bridge-off
  mutable std::atomic<bool> m_dirErrorLogged{false};
  mutable std::atomic<bool> m_offLogged{false};
  CCriticalSection m_lock;
};

//! bytes from /dev/urandom as lowercase hex (2 characters per byte)
std::string RandomHex(size_t bytes);

bool IsPrintableAscii(const std::string& text);

} // namespace TVPLAYER
