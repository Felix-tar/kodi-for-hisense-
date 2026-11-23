/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TVPlayerHttpFeeder.h"

#include "TVBridgeIO.h"
#include "filesystem/File.h"
#include "threads/Event.h"
#include "utils/HttpRangeUtils.h"
#include "utils/Mime.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#ifndef HAS_WEB_SERVER
#error "TVPlayer needs libmicrohttpd (FindMicroHttpd.cmake sets HAS_WEB_SERVER)"
#endif

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

// like network/httprequesthandler/IHTTPRequestHandler.h:25-28 (own name: that header may be in
// the same translation unit one day)
#if MHD_VERSION >= 0x00097002
using FeederResult = MHD_Result;
#else
using FeederResult = int;
#endif

using namespace TVPLAYER;

namespace
{
// response block size: the TV reads several ranges in parallel (RD:359)
constexpr size_t BLOCK_SIZE = 256 * 1024;
// MHD's own hard limit lies above ours, so that a new connection can still push out the one
// that was idle longest (REV:121-127)
constexpr unsigned int EXTRA_CONNECTIONS = 4;

int64_t SteadyMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

FeederResult Status(MHD_Connection* connection, unsigned int code, const char* contentRange = nullptr)
{
  MHD_Response* response = MHD_create_response_from_buffer(0, nullptr, MHD_RESPMEM_PERSISTENT);
  if (!response)
    return MHD_NO;
  if (contentRange)
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_RANGE, contentRange);
  const FeederResult ret = MHD_queue_response(connection, code, response);
  MHD_destroy_response(response);
  return ret;
}

/*!
 * \brief uid of the socket 127.0.0.1:peerPort -> 127.0.0.1:ourPort, from /proc/net/tcp.
 * \return the uid, -1 if not found
 */
int PeerUid(uint16_t peerPort, uint16_t ourPort)
{
  FILE* f = std::fopen("/proc/net/tcp", "re");
  if (!f)
    return -1;

  // the kernel prints the address as the raw 32-bit value: 127.0.0.1 is 0100007F here
  char wantLocal[24];
  char wantRemote[24];
  std::snprintf(wantLocal, sizeof(wantLocal), "0100007F:%04X", static_cast<unsigned int>(peerPort));
  std::snprintf(wantRemote, sizeof(wantRemote), "0100007F:%04X", static_cast<unsigned int>(ourPort));

  int uid = -1;
  char line[512];
  // "sl local_address rem_address st tx_queue:rx_queue tr:tm->when retrnsmt uid timeout inode"
  while (std::fgets(line, sizeof(line), f))
  {
    char local[64];
    char remote[64];
    int lineUid = -1;
    if (std::sscanf(line, "%*s %63s %63s %*s %*s %*s %*s %d", local, remote, &lineUid) != 3)
      continue;
    if (strcasecmp(local, wantLocal) == 0 && strcasecmp(remote, wantRemote) == 0)
    {
      uid = lineUid;
      break;
    }
  }
  std::fclose(f);
  return uid;
}
} // namespace

struct CTVHttpFeeder::Impl
{
  struct Reader
  {
    CTVHttpFeeder* feeder = nullptr;
    std::shared_ptr<Connection> connection;
    std::unique_ptr<XFILE::CFile> file; // nullptr for HEAD
    uint64_t first = 0;
    uint64_t length = 0;
    uint64_t filePos = std::numeric_limits<uint64_t>::max();
    unsigned int generation = 0;
    bool cached = false;
  };

  static FeederResult AcceptPolicy(void* cls, const sockaddr* addr, socklen_t addrLength)
  {
    auto* self = static_cast<CTVHttpFeeder*>(cls);
    if (!addr || addr->sa_family != AF_INET || addrLength < static_cast<socklen_t>(sizeof(sockaddr_in)))
      return MHD_NO;
    const auto* in = reinterpret_cast<const sockaddr_in*>(addr);
    if (in->sin_addr.s_addr != htonl(INADDR_LOOPBACK) || self->m_port == 0)
      return MHD_NO;

    // only the TV player's process (P18: uid 2910), not other apps under the launcher's uid
    const int uid = PeerUid(ntohs(in->sin_port), self->m_port);
    const bool allowed = self->m_config.peerUid < 0 || uid == self->m_config.peerUid;
    {
      std::unique_lock<std::mutex> lock(self->m_connLock);
      if (self->m_loggedUids.insert(allowed ? uid : -1000 - uid).second)
      {
        if (allowed)
          CLog::Log(LOGINFO, "CTVHttpFeeder: connection from uid {}{}", uid,
                    self->m_config.peerUid < 0 ? " (uid check off)" : "");
        else
          CLog::Log(LOGWARNING, "CTVHttpFeeder: connection from uid {} refused (expected uid {})",
                    uid, self->m_config.peerUid);
      }
    }
    return allowed ? MHD_YES : MHD_NO;
  }

  static void NotifyConnection(void* cls,
                               MHD_Connection* connection,
                               void** /* socketContext */,
                               enum MHD_ConnectionNotificationCode toe)
  {
    auto* self = static_cast<CTVHttpFeeder*>(cls);
    std::unique_lock<std::mutex> lock(self->m_connLock);
    if (toe == MHD_CONNECTION_NOTIFY_CLOSED)
    {
      // MHD closes the socket only after this call: a kept fd is never a reused one
      self->m_connections.erase(connection);
      return;
    }

    auto entry = std::make_shared<Connection>();
    const MHD_ConnectionInfo* info =
        MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CONNECTION_FD);
    entry->fd = info ? info->connect_fd : -1;
    entry->lastActivityMs = SteadyMs();

    // full: close the connection that was idle longest (the player may leave old ranges open
    // when jumping around, P11). Only live connections count and can be chosen: one pushed out
    // earlier may stay in the map for a while (a read blocked in SMB, MHD's cleanup), and
    // choosing it again would close nothing.
    size_t live = 0;
    for (const auto& it : self->m_connections)
    {
      if (!it.second->evicted)
        ++live;
    }
    while (live >= self->m_config.connections)
    {
      auto oldest = self->m_connections.end();
      for (auto it = self->m_connections.begin(); it != self->m_connections.end(); ++it)
      {
        if (!it->second->evicted && (oldest == self->m_connections.end() ||
                                     it->second->lastActivityMs < oldest->second->lastActivityMs))
          oldest = it;
      }
      if (oldest == self->m_connections.end())
        break;
      Connection& victim = *oldest->second;
      CLog::Log(LOGINFO, "CTVHttpFeeder: {} connections, closing the one idle for {} ms", live,
                SteadyMs() - victim.lastActivityMs.load());
      victim.evicted = true;
      if (victim.fd >= 0)
        shutdown(victim.fd, SHUT_RDWR);
      victim.fd = -1;
      --live;
    }
    self->m_connections[connection] = std::move(entry);
    CLog::Log(LOGDEBUG, "CTVHttpFeeder: {} open connection(s), {} closing", live + 1,
              self->m_connections.size() - live - 1);
  }

  static ssize_t ReadCb(void* cls, uint64_t pos, char* buffer, size_t max)
  {
    auto* r = static_cast<Reader*>(cls);
    if (!r->file || r->feeder->m_generation.load() != r->generation)
      return MHD_CONTENT_READER_END_WITH_ERROR; // stopped, or HEAD
    if (r->connection && r->connection->evicted)
      return MHD_CONTENT_READER_END_WITH_ERROR; // pushed out: MHD closes it
    if (pos >= r->length)
      return MHD_CONTENT_READER_END_OF_STREAM;
    if (r->connection)
      r->connection->lastActivityMs = SteadyMs();

    // pos is the offset in the response, not in the file
    const uint64_t at = r->first + pos;
    if (r->filePos != at)
    {
      if (r->file->Seek(static_cast<int64_t>(at), SEEK_SET) != static_cast<int64_t>(at))
        return MHD_CONTENT_READER_END_WITH_ERROR;
      r->filePos = at;
    }
    const size_t want = static_cast<size_t>(std::min<uint64_t>(max, r->length - pos));
    // READ_TRUNCATED: short reads are fine. Never return 0: with one thread per connection that
    // means "ask again at once"
    const ssize_t n = r->file->Read(buffer, want);
    if (n <= 0)
      return MHD_CONTENT_READER_END_WITH_ERROR;
    r->filePos += static_cast<uint64_t>(n);
    return n;
  }

  static void FreeCb(void* cls)
  {
    auto* r = static_cast<Reader*>(cls);
    if (r->cached)
      r->feeder->m_cacheInUse = false;
    delete r; // closes the file
  }

  static FeederResult Answer(void* cls,
                             MHD_Connection* connection,
                             const char* url,
                             const char* method,
                             const char* version,
                             const char* uploadData,
                             size_t* uploadDataSize,
                             void** requestContext)
  {
    auto* self = static_cast<CTVHttpFeeder*>(cls);
    if (uploadDataSize && *uploadDataSize != 0)
      return MHD_NO; // GET/HEAD only, there is no body

    const bool head = std::strcmp(method, MHD_HTTP_METHOD_HEAD) == 0;
    if (!head && std::strcmp(method, MHD_HTTP_METHOD_GET) != 0)
      return Status(connection, MHD_HTTP_METHOD_NOT_ALLOWED);

    const ServedFile* served = nullptr;
    for (const ServedFile& file : self->m_files)
    {
      if (url && url == "/" + self->m_token + "/" + file.name)
        served = &file;
    }
    if (!served)
      return Status(connection, MHD_HTTP_NOT_FOUND);

    if (served->subtitle && !self->m_subtitleRequested.exchange(true))
      CLog::Log(LOGINFO, "CTVHttpFeeder: the TV player asks for the subtitle file {}", served->name);

    uint64_t first = 0;
    uint64_t length = served->length;
    bool ranged = false;
    const char* rangeHeader =
        MHD_lookup_connection_value(connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_RANGE);
    if (rangeHeader)
    {
      CHttpRanges ranges;
      if (!ranges.Parse(rangeHeader, served->length))
      {
        CLog::Log(LOGDEBUG, "CTVHttpFeeder: {} {}: unsatisfiable range", method, served->name);
        return Status(connection, 416 /* Range Not Satisfiable */,
                      ("bytes */" + std::to_string(served->length)).c_str());
      }
      CHttpRange range;
      // several ranges: the whole file with 200 (allowed by RFC 9110)
      if (ranges.Size() == 1 && ranges.GetFirst(range))
      {
        first = range.GetFirstPosition();
        length = range.GetLength();
        ranged = true;
      }
    }
    CLog::Log(LOGDEBUG, "CTVHttpFeeder: {} {} bytes {}-{}", method, served->name, first,
              first + length - (length ? 1 : 0));

    auto reader = std::make_unique<Reader>();
    reader->feeder = self;
    reader->first = first;
    reader->length = length;
    reader->generation = self->m_generation.load();
    {
      std::unique_lock<std::mutex> lock(self->m_connLock);
      const auto it = self->m_connections.find(connection);
      if (it != self->m_connections.end())
      {
        if (it->second->evicted)
          return MHD_NO; // pushed out while idle (keep-alive): close instead of serving
        reader->connection = it->second;
        reader->connection->lastActivityMs = SteadyMs();
      }
    }
    if (!head)
    {
      // own CFile per request, without Kodi's 20 MB cache per connection (KI:721-728); with
      // <filecache>true</filecache> one connection at a time gets CFileCache
      unsigned int flags = XFILE::READ_TRUNCATED | XFILE::READ_CHUNKED;
      if (self->m_config.fileCache && !served->subtitle && !self->m_cacheInUse.exchange(true))
      {
        flags |= XFILE::READ_AUDIO_VIDEO | XFILE::READ_MULTI_STREAM;
        reader->cached = true;
      }
      reader->file = std::make_unique<XFILE::CFile>();
      if (!reader->file->Open(served->path, flags))
      {
        if (reader->cached)
          self->m_cacheInUse = false;
        CLog::Log(LOGWARNING, "CTVHttpFeeder: {} cannot be opened (source gone?)", served->name);
        return Status(connection, MHD_HTTP_NOT_FOUND);
      }
    }

    MHD_Response* response =
        MHD_create_response_from_callback(length, BLOCK_SIZE, &ReadCb, reader.get(), &FreeCb);
    if (!response)
    {
      if (reader->cached)
        self->m_cacheInUse = false;
      return MHD_NO;
    }
    reader.release(); // owned by MHD now, freed in FreeCb

    MHD_add_response_header(response, MHD_HTTP_HEADER_ACCEPT_RANGES, "bytes");
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, served->mime.c_str());
    if (ranged)
      MHD_add_response_header(
          response, MHD_HTTP_HEADER_CONTENT_RANGE,
          HttpRangeUtils::GenerateContentRangeHeaderValue(first, first + length - 1,
                                                          served->length)
              .c_str());
    const FeederResult ret =
        MHD_queue_response(connection, ranged ? MHD_HTTP_PARTIAL_CONTENT : MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return ret;
  }

  static void Logger(void* cls, const char* format, va_list args)
  {
    auto* self = static_cast<CTVHttpFeeder*>(cls);
    char text[512];
    std::vsnprintf(text, sizeof(text), format, args);
    std::string message(text);
    StringUtils::TrimRight(message);
    // never the token in a log (it is the only secret of the URL)
    if (!self->m_token.empty())
      StringUtils::Replace(message, self->m_token, "<token>");
    CLog::Log(LOGINFO, "CTVHttpFeeder: libmicrohttpd: {}", message);
  }
};

CTVHttpFeeder::CTVHttpFeeder(const Config& config) : m_config(config)
{
}

CTVHttpFeeder::~CTVHttpFeeder()
{
  // normally Stop() ran already; the background stop holds a reference until MHD is done
  if (m_daemon)
    MHD_stop_daemon(std::exchange(m_daemon, nullptr));
}

std::string CTVHttpFeeder::MediaName(const std::string& extension)
{
  // URIUtils::GetExtension() returns ".MKV" with dot and case of the file name (URIUtils.cpp:54-66)
  std::string ext = StringUtils::ToLower(extension);
  if (!ext.empty() && ext[0] == '.')
    ext.erase(0, 1);
  bool ok = !ext.empty() && ext.size() <= 5;
  for (const char c : ext)
    ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
  return ok ? "media." + ext : std::string("media");
}

void CTVHttpFeeder::AddSubtitleFile(const std::string& vfsPath, const std::string& extension)
{
  ServedFile file;
  file.name = MediaName(extension);
  file.path = vfsPath;
  file.mime = CMime::GetMimeType(extension);
  if (file.mime.empty())
    file.mime = "text/plain";
  file.subtitle = true;
  m_files.push_back(std::move(file));
}

bool CTVHttpFeeder::Start(const std::string& vfsPath, const std::string& extension)
{
  if (m_daemon)
    return false;

  XFILE::CFile probe;
  if (!probe.Open(vfsPath, XFILE::READ_NO_CACHE))
    return false;
  const int64_t length = probe.GetLength();
  probe.Close();
  if (length <= 0)
    return false;

  ServedFile media;
  media.name = MediaName(extension);
  media.path = vfsPath;
  media.mime = CMime::GetMimeType(extension); // takes the extension with or without the dot
  if (media.mime.empty())
    media.mime = "application/octet-stream";
  media.length = static_cast<uint64_t>(length);

  // subtitles next to the film: only those that can be read, never under the film's name
  std::vector<ServedFile> files;
  files.push_back(media);
  for (ServedFile& sub : m_files)
  {
    XFILE::CFile subFile;
    if (sub.name == media.name || !subFile.Open(sub.path, XFILE::READ_NO_CACHE))
      continue;
    const int64_t subLength = subFile.GetLength();
    subFile.Close();
    if (subLength <= 0)
      continue;
    sub.length = static_cast<uint64_t>(subLength);
    files.push_back(sub);
  }
  m_files = std::move(files);
  m_name = media.name;
  m_length = media.length;
  m_token = RandomHex(16);
  ++m_generation;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0; // the kernel picks a free port
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only

  // MHD_USE_ITC: needed for MHD_quiesce_daemon() in Stop()
  const unsigned int flags = MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION |
                             MHD_USE_ITC | MHD_USE_ERROR_LOG;
  m_daemon = MHD_start_daemon(
      flags, 0, &Impl::AcceptPolicy, this, &Impl::Answer, this, MHD_OPTION_SOCK_ADDR,
      reinterpret_cast<const sockaddr*>(&addr), MHD_OPTION_CONNECTION_LIMIT,
      static_cast<unsigned int>(m_config.connections + EXTRA_CONNECTIONS),
      MHD_OPTION_CONNECTION_TIMEOUT, static_cast<unsigned int>(m_config.timeoutS),
      MHD_OPTION_NOTIFY_CONNECTION, &Impl::NotifyConnection, this, MHD_OPTION_EXTERNAL_LOGGER,
      &Impl::Logger, this, MHD_OPTION_END);
  if (!m_daemon)
  {
    CLog::Log(LOGERROR, "CTVHttpFeeder: cannot start the local HTTP server");
    return false;
  }

  const MHD_DaemonInfo* info = MHD_get_daemon_info(m_daemon, MHD_DAEMON_INFO_BIND_PORT);
  m_port = info ? info->port : 0;
  if (m_port == 0)
  {
    CLog::Log(LOGERROR, "CTVHttpFeeder: no port for the local HTTP server");
    MHD_stop_daemon(std::exchange(m_daemon, nullptr));
    return false;
  }

  CLog::Log(LOGINFO,
            "CTVHttpFeeder: serving {} ({} bytes{}) on 127.0.0.1:{}, at most {} connections, "
            "peer uid {}",
            m_name, m_length, m_files.size() > 1 ? ", with subtitle file" : "", m_port,
            m_config.connections,
            m_config.peerUid < 0 ? std::string("not checked") : std::to_string(m_config.peerUid));
  return true;
}

void CTVHttpFeeder::Stop(std::chrono::milliseconds wait)
{
  MHD_Daemon* daemon = std::exchange(m_daemon, nullptr);
  if (!daemon)
    return;

  ++m_generation; // running readers fail on their next callback
  // accept nothing new; the listen socket is ours now and closed after the stop
  const MHD_socket listenSocket = MHD_quiesce_daemon(daemon);
  {
    // connections waiting to send end at once (the TV player was told to stop already)
    std::unique_lock<std::mutex> lock(m_connLock);
    for (auto& entry : m_connections)
    {
      if (entry.second->fd >= 0)
        shutdown(entry.second->fd, SHUT_RDWR);
      entry.second->fd = -1;
      entry.second->evicted = true;
    }
  }

  // a connection blocked in a read (e.g. SMB) holds MHD_stop_daemon(): never block CloseFile for
  // that (D14). The readers point into this object, the thread keeps it alive.
  auto self = shared_from_this();
  auto done = std::make_shared<CEvent>(true);
  std::thread reaper([daemon, self, done, listenSocket]() {
    MHD_stop_daemon(daemon);
    if (listenSocket != MHD_INVALID_SOCKET)
      close(listenSocket);
    done->Set();
  });
  if (done->Wait(wait))
  {
    reaper.join();
    CLog::Log(LOGDEBUG, "CTVHttpFeeder: stopped");
  }
  else
  {
    CLog::Log(LOGWARNING,
              "CTVHttpFeeder: a connection is still blocked in a read, finishing in background");
    reaper.detach();
  }
}
