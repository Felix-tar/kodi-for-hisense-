/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct MHD_Daemon;
struct MHD_Connection;

namespace TVPLAYER
{

/*!
 * \brief Local HTTP server that hands exactly one file of Kodi's VFS (SMB, USB, http ...) to the
 *        TV's media player: http://127.0.0.1:<port>/<token>/media.<ext>.
 *
 * - libmicrohttpd, bound to 127.0.0.1 only on a port picked by the kernel, one thread per
 *   connection (a blocked SMB read only holds its own connection).
 * - Only the path /<32 hex token>/<name> (plus an optional subtitle file next to it), only GET
 *   and HEAD, byte ranges via CHttpRanges. Anything else: 404/405.
 * - Every connection is checked on accept: the peer must be 127.0.0.1 and, unless peerUid is -1,
 *   its socket must belong to that uid (looked up in /proc/net/tcp by its port).
 * - At most `connections` connections: a further one closes the connection that was idle longest.
 * - Stop() waits at most the given time; a connection still blocked in a read is finished in
 *   the background (the object stays alive through shared_from_this until then).
 *
 * Logs never contain the token or the file path, only offsets.
 */
class CTVHttpFeeder : public std::enable_shared_from_this<CTVHttpFeeder>
{
public:
  struct Config
  {
    unsigned int connections = 16;
    unsigned int timeoutS = 3600; //!< without activity (a long pause must not cut the connection)
    int peerUid = 2910; //!< uid of the TV player's process (P18), -1 = log only
    bool fileCache = false; //!< Kodi's CFileCache for one connection (after P14)
  };

  explicit CTVHttpFeeder(const Config& config);
  ~CTVHttpFeeder();
  CTVHttpFeeder(const CTVHttpFeeder&) = delete;
  CTVHttpFeeder& operator=(const CTVHttpFeeder&) = delete;

  /*!
   * \brief Offers a subtitle file next to the film as media.<ext> (before Start()). An .idx/.sub
   *        pair takes two calls.
   */
  void AddSubtitleFile(const std::string& vfsPath, const std::string& extension);

  bool Start(const std::string& vfsPath, const std::string& extension);
  void Stop(std::chrono::milliseconds wait = std::chrono::milliseconds(2000));

  uint16_t GetPort() const { return m_port; }
  const std::string& GetToken() const { return m_token; }
  const std::string& GetName() const { return m_name; } //!< "media.<ext>" or "media"
  uint64_t GetLength() const { return m_length; }

  //! "media.<ext lowercase>", or "media" if the extension is not [a-z0-9]{1,5}
  static std::string MediaName(const std::string& extension);

  //! used by the static MHD callbacks in the .cpp
  struct Impl;

private:
  struct ServedFile
  {
    std::string name;
    std::string path;
    std::string mime;
    uint64_t length = 0;
    bool subtitle = false;
  };

  struct Connection
  {
    int fd = -1;
    std::atomic<int64_t> lastActivityMs{0};
    //! pushed out (or Stop()): no longer counts against `connections`, never chosen again; it
    //! stays in m_connections until MHD reports it closed (a read may still be blocked in it)
    std::atomic<bool> evicted{false};
  };

  const Config m_config;
  MHD_Daemon* m_daemon = nullptr;
  // immutable after Start()
  std::vector<ServedFile> m_files;
  std::string m_token;
  std::string m_name;
  uint64_t m_length = 0;
  uint16_t m_port = 0;
  std::atomic<unsigned int> m_generation{0};
  std::atomic<bool> m_cacheInUse{false};
  std::atomic<bool> m_subtitleRequested{false};

  std::mutex m_connLock;
  std::map<MHD_Connection*, std::shared_ptr<Connection>> m_connections;
  std::set<int> m_loggedUids;
};

} // namespace TVPLAYER
