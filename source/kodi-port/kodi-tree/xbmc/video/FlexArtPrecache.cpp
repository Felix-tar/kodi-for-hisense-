/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "FlexArtPrecache.h"

#include "ServiceBroker.h"
#include "TextureCache.h"
#include "dbwrappers/dataset.h"
#include "utils/Job.h"
#include "utils/JobManager.h"
#include "utils/log.h"
#include "video/VideoDatabase.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <string>
#include <vector>

namespace
{
//! reads the art table directly (CVideoDatabase has no call for "all posters")
class CArtUrls : public CVideoDatabase
{
public:
  std::vector<std::string> Get()
  {
    std::vector<std::string> urls;
    if (!m_pDB || !m_pDS)
      return urls;
    try
    {
      // movie posters first (Movies A-Z), then sets, shows, seasons, then the fanart
      if (!m_pDS->query("SELECT url FROM art WHERE media_type IN "
                        "('movie','set','tvshow','season') AND type IN ('poster','fanart') "
                        "GROUP BY url ORDER BY MIN(CASE type WHEN 'poster' THEN 0 ELSE 10 END + "
                        "CASE media_type WHEN 'movie' THEN 0 WHEN 'set' THEN 1 "
                        "WHEN 'tvshow' THEN 2 ELSE 3 END)"))
        return urls;
      while (!m_pDS->eof())
      {
        urls.push_back(m_pDS->fv(0).get_asString());
        m_pDS->next();
      }
      m_pDS->close();
    }
    catch (const std::exception& e)
    {
      CLog::Log(LOGERROR, "FlexArtPrecache: art table not readable ({})", e.what());
      urls.clear();
    }
    catch (...)
    {
      CLog::Log(LOGERROR, "FlexArtPrecache: art table not readable");
      urls.clear();
    }
    return urls;
  }
};

class CPrecacheJob : public CJob
{
public:
  bool DoWork() override
  {
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::string> urls;
    {
      CArtUrls db;
      if (!db.Open())
        return false;
      urls = db.Get();
      db.Close();
    }
    const auto cache = CServiceBroker::GetTextureCache();
    if (!cache)
      return false;
    for (const auto& url : urls)
    {
      if (!url.empty())
        cache->BackgroundCacheImage(url); // skips what is cached already
    }
    CLog::Log(LOGINFO, "FlexArtPrecache: {} posters and fanart checked in {} ms, missing ones are "
                       "loaded in the background",
              urls.size(),
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count());
    return true;
  }

  const char* GetType() const override { return "flexartprecache"; }
};

std::atomic<bool> started{false};
} // namespace

void FLEXART::StartPrecache()
{
  if (started.exchange(true))
    return;
  const auto jobs = CServiceBroker::GetJobManager();
  if (jobs)
    jobs->AddJob(new CPrecacheJob(), nullptr, CJob::PRIORITY_LOW_PAUSABLE);
}
