/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "FlexVolumeBar.h"

#include "ServiceBroker.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "filesystem/CurlFile.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindow.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/WindowIDs.h"
#include "input/actions/ActionIDs.h"
#include "messaging/ApplicationMessenger.h"
#include "threads/CriticalSection.h"
#include "utils/Job.h"
#include "utils/JobManager.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace
{
constexpr const char* ENV_URL = "FLEX_KODI_AVR_URL";
constexpr unsigned int HINT_MS = 1500;

std::atomic<unsigned int> generation{0};
std::atomic<bool> failureLogged{false};

void SetProperties(const std::string& percent, const std::string& text)
{
  auto gui = CServiceBroker::GetGUI();
  CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
  if (!gui || !winSystem)
    return;
  // as xbmcgui.Window.setProperty does it from other threads: under the GUI lock
  std::unique_lock<CCriticalSection> lock(winSystem->GetGfxContext());
  CGUIWindow* home = gui->GetWindowManager().GetWindow(WINDOW_HOME);
  if (!home)
    return;
  home->SetProperty("flexvol.pct", percent);
  home->SetProperty("flexvol.text", text);
}

std::string ReceiverUrl()
{
  const char* url = std::getenv(ENV_URL);
  if (!url || std::strncmp(url, "http://", 7) != 0 || std::strlen(url) > 200)
    return "";
  return url;
}

class CReceiverJob : public CJob
{
public:
  CReceiverJob(std::string url, std::string fallback, unsigned int gen)
    : m_url(std::move(url)), m_fallback(std::move(fallback)), m_gen(gen)
  {
  }

  bool DoWork() override
  {
    using namespace std::chrono_literals;
    std::string shown;
    // first right after the last step left the TV, once more when CEC has delivered everything
    for (const auto wait : {150ms, 650ms})
    {
      std::this_thread::sleep_for(wait);
      if (generation != m_gen)
        return true; // a newer press shows its own value
      XFILE::CCurlFile http;
      http.SetTimeout(2);
      std::string xml;
      int percent = 0;
      std::string text;
      if (!http.Get(m_url, xml) || !FLEXVOL::ParseDenonStatus(xml, percent, text))
      {
        if (shown.empty())
        {
          if (!failureLogged.exchange(true))
            CLog::Log(LOGWARNING, "FlexVolumeBar: receiver status not readable, hint instead");
          CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Lautst\xC3\xA4rke",
                                                m_fallback, HINT_MS, false);
        }
        return false;
      }
      if (generation != m_gen)
        return true;
      const std::string key = std::to_string(percent) + "|" + text;
      if (key != shown)
      {
        FLEXVOL::Show(percent, text);
        shown = key;
      }
    }
    return true;
  }

  const char* GetType() const override { return "flexvolumebar"; }

private:
  std::string m_url;
  std::string m_fallback;
  unsigned int m_gen;
};
} // namespace

void FLEXVOL::Show(int percent, const std::string& text)
{
  SetProperties(std::to_string(std::clamp(percent, 0, 100)), text);
  const auto messenger = CServiceBroker::GetAppMessenger();
  if (messenger)
    messenger->PostMsg(TMSG_VOLUME_SHOW, ACTION_VOLUME_UP);
}

void FLEXVOL::Clear()
{
  SetProperties("", "");
}

bool FLEXVOL::ShowReceiver(const std::string& fallback)
{
  const std::string url = ReceiverUrl();
  if (url.empty())
    return false;
  const auto jobs = CServiceBroker::GetJobManager();
  if (!jobs)
    return false;
  const unsigned int gen = ++generation;
  jobs->AddJob(new CReceiverJob(url, fallback, gen), nullptr, CJob::PRIORITY_HIGH);
  return true;
}
