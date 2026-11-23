/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemDirectFB.h"

#include "EvdevInputHandler.h"
#include "guilib/DispResource.h"
#include "settings/DisplaySettings.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/Resolution.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <directfb.h>

using namespace KODI::WINDOWING::DIRECTFB;

namespace
{

// Used when the primary layer does not report its size
constexpr int DEFAULT_WIDTH = 1920;
constexpr int DEFAULT_HEIGHT = 1080;

// DirectFB 1.4 has no portable way to query the refresh rate of the panel
constexpr float DEFAULT_REFRESH_RATE = 60.0f;

constexpr const char* DEFAULT_INPUT_DEVICE = "/dev/input/event0";

// "1" keeps per-pixel alpha blending of the Kodi window on (default: opaque window)
constexpr const char* ENV_WINDOW_ALPHA = "FLEX_KODI_WINDOW_ALPHA";
// "1": the video underlay blends the window with the layer below (DWOP_ALPHACHANNEL) instead of
// copying it; the launcher page must then be transparent itself
constexpr const char* ENV_UNDERLAY_BLEND = "FLEX_KODI_UNDERLAY_BLEND";
// "1": create the window surface with DSCAPS_PREMULTIPLIED (Kodi's frame buffer holds
// premultiplied colour, analysis/tvplayer-20261028/kodi-overlay-alpha.md). Only matters with
// DWOP_ALPHACHANNEL; opt-in until tested with the TV's EGL
constexpr const char* ENV_DFB_PREMULT = "FLEX_KODI_DFB_PREMULT";
// "1": the window never gets focus or DirectFB key events (DWOP_GHOST). Kodi reads the remote
// via evdev, so it loses nothing; test for keys injected with send_key_to_dfb (P9)
constexpr const char* ENV_DFB_GHOST = "FLEX_KODI_DFB_GHOST";
// "1": video underlay from the start without a player (test of the transparent GUI, step S5)
constexpr const char* ENV_UNDERLAY_TEST = "FLEX_KODI_UNDERLAY_TEST";

const char* GetEnv(const char* name)
{
  const char* value = std::getenv(name);
  return value ? value : "<unset>";
}

bool IsEnvOne(const char* name)
{
  const char* env = std::getenv(name);
  return env && std::strcmp(env, "1") == 0;
}

bool IsWindowAlphaRequested()
{
  return IsEnvOne(ENV_WINDOW_ALPHA);
}

// Number of the DirectFB display layer for Kodi's window (overrides the detection below)
constexpr const char* ENV_LAYER = "FLEX_KODI_DFB_LAYER";
// The TV's DirectFB has 3 layers (0..2) on separate GOPs
constexpr int MAX_LAYERS = 3;

/*!
 * \brief Picks the display layer for Kodi's window.
 *
 * The TV launcher draws into an opaque full-screen window on one of the layers, and which one
 * differs between boots (seen on the TV: layer 0 one night, layer 1 after a cold boot, while a
 * window on the other layer stayed hidden below it). Kodi therefore goes onto the layer that
 * currently has a visible (opacity > 0) window and raises itself to the top there, like the
 * launcher's own windows. MStar's GetWindow() returns the layer's first window for any id, which
 * is enough to tell whether a layer has a visible window. Read-only: nothing is changed on other
 * clients' windows. Falls back to DLID_PRIMARY.
 */
DFBDisplayLayerID ChooseLayer(IDirectFB* dfb)
{
  const char* env = std::getenv(ENV_LAYER);
  if (env && *env >= '0' && *env < '0' + MAX_LAYERS && env[1] == '\0')
  {
    CLog::Log(LOGINFO, "CWinSystemDirectFB: layer {} from {}", env, ENV_LAYER);
    return static_cast<DFBDisplayLayerID>(*env - '0');
  }

  int chosen = -1;
  for (int id = 0; id < MAX_LAYERS; ++id)
  {
    IDirectFBDisplayLayer* layer = nullptr;
    if (dfb->GetDisplayLayer(dfb, static_cast<DFBDisplayLayerID>(id), &layer) != DFB_OK || !layer)
      continue;
    layer->SetCooperativeLevel(layer, DLSCL_SHARED);
    IDirectFBWindow* window = nullptr;
    u8 opacity = 0;
    const bool hasWindow = layer->GetWindow(layer, 1, &window) == DFB_OK && window;
    if (hasWindow)
    {
      window->GetOpacity(window, &opacity);
      window->Release(window);
    }
    layer->Release(layer);
    CLog::Log(LOGINFO, "CWinSystemDirectFB: layer {}: {}", id,
              hasWindow ? StringUtils::Format("window, opacity {}", static_cast<int>(opacity))
                        : std::string("no window"));
    // the last layer with a visible window wins (the launcher has exactly one)
    if (hasWindow && opacity > 0)
      chosen = id;
  }

  if (chosen < 0)
  {
    CLog::Log(LOGINFO, "CWinSystemDirectFB: no layer with a visible window, using the primary layer");
    return DLID_PRIMARY;
  }
  CLog::Log(LOGINFO, "CWinSystemDirectFB: using layer {} (has the visible launcher window)", chosen);
  return static_cast<DFBDisplayLayerID>(chosen);
}

/*!
 * \brief A DirectFB structure with zeroed spare room behind it.
 *
 * The TV's DirectFB is an MStar fork of 1.4.2 whose public structures are larger than in the
 * 1.4.2 headers: GetConfiguration() writes 36 bytes into the 32-byte DFBDisplayLayerConfig,
 * GetDeviceDescription() 240 into 236 (measured on the TV with kodi-port/tests/
 * dfb-struct-probe.c). On the stack that overwrote a saved register and Kodi crashed. Every
 * structure DirectFB fills in or reads is therefore wrapped in this.
 */
template<typename T>
struct WithSpareRoom
{
  T value;
  unsigned char spare[256];
};

/*!
 * \brief Logs a failed DirectFB call with its error string.
 * \return true if result is DFB_OK
 */
bool Succeeded(DFBResult result, const char* what)
{
  if (result == DFB_OK)
    return true;

  const char* error = DirectFBErrorString(result);
  CLog::Log(LOGERROR, "CWinSystemDirectFB: {} failed: {} ({})", what,
            error ? error : "unknown error", static_cast<int>(result));
  return false;
}

} // namespace

CWinSystemDirectFB::CWinSystemDirectFB() = default;

CWinSystemDirectFB::~CWinSystemDirectFB()
{
  // Normally already done by DestroyWindowSystem(). A subclass destroys its own objects that
  // depend on the DirectFB surface (EGL surface) before this destructor runs.
  StopInput();
  ReleaseDirectFB();
}

bool CWinSystemDirectFB::InitWindowSystem()
{
  if (m_dfb)
  {
    CLog::Log(LOGERROR, "CWinSystemDirectFB::{} - DirectFB is already initialized", __FUNCTION__);
    return false;
  }

  CLog::Log(LOGINFO, "CWinSystemDirectFB::{} - DIRECTFB_SESSION={}, DFBARGS={}", __FUNCTION__,
            GetEnv("DIRECTFB_SESSION"), GetEnv("DFBARGS"));

  // DirectFB reads its options from the DFBARGS environment variable (set by the launch script),
  // there is no command line to hand over.
  if (!Succeeded(DirectFBInit(nullptr, nullptr), "DirectFBInit"))
    return false;

  if (!Succeeded(DirectFBCreate(&m_dfb), "DirectFBCreate"))
  {
    m_dfb = nullptr;
    return false;
  }

  if (!CreateDirectFBWindow())
  {
    ReleaseDirectFB();
    return false;
  }

  StartInput();

  if (IsEnvOne(ENV_UNDERLAY_TEST))
  {
    CLog::Log(LOGINFO, "CWinSystemDirectFB::{} - {}=1: video underlay on without a player",
              __FUNCTION__, ENV_UNDERLAY_TEST);
    SetVideoUnderlay(true);
  }

  return CWinSystemBase::InitWindowSystem();
}

bool CWinSystemDirectFB::DestroyWindowSystem()
{
  CLog::Log(LOGINFO, "CWinSystemDirectFB::{}", __FUNCTION__);

  StopInput();
  ReleaseDirectFB();

  // only resets the screen saver manager, its "false" is not an error
  CWinSystemBase::DestroyWindowSystem();

  return true;
}

bool CWinSystemDirectFB::CreateDirectFBWindow()
{
  if (!Succeeded(m_dfb->GetDisplayLayer(m_dfb, ChooseLayer(m_dfb), &m_dfbLayer),
                 "IDirectFB::GetDisplayLayer"))
  {
    m_dfbLayer = nullptr;
    return false;
  }

  if (!Succeeded(m_dfbLayer->SetCooperativeLevel(m_dfbLayer, DLSCL_SHARED),
                 "IDirectFBDisplayLayer::SetCooperativeLevel(DLSCL_SHARED)"))
    return false;

  WithSpareRoom<DFBDisplayLayerConfig> layerConfigBuffer{};
  DFBDisplayLayerConfig& layerConfig = layerConfigBuffer.value;
  if (!Succeeded(m_dfbLayer->GetConfiguration(m_dfbLayer, &layerConfig),
                 "IDirectFBDisplayLayer::GetConfiguration"))
    return false;

  CLog::Log(LOGINFO,
            "CWinSystemDirectFB::{} - layer {}x{}, pixel format {:#010x}, buffer mode {}",
            __FUNCTION__, layerConfig.width, layerConfig.height,
            static_cast<unsigned int>(layerConfig.pixelformat),
            static_cast<int>(layerConfig.buffermode));

  // read by DirectFB, possibly with MStar's additional fields: those stay zero
  WithSpareRoom<DFBWindowDescription> descriptionBuffer{};
  DFBWindowDescription& description = descriptionBuffer.value;
  description.flags = static_cast<DFBWindowDescriptionFlags>(
      DWDESC_POSX | DWDESC_POSY | DWDESC_WIDTH | DWDESC_HEIGHT | DWDESC_CAPS | DWDESC_PIXELFORMAT);
  description.posx = 0;
  description.posy = 0;
  description.width = layerConfig.width > 0 ? layerConfig.width : DEFAULT_WIDTH;
  description.height = layerConfig.height > 0 ? layerConfig.height : DEFAULT_HEIGHT;
  // ARGB with an alpha channel is the surface EGL is proven to work with. DirectFB 1.4 enables
  // per-pixel alpha blending (DWOP_ALPHACHANNEL) for such a window; it is switched off below and
  // the options before and after are logged.
  description.caps = DWCAPS_ALPHACHANNEL;
  description.pixelformat = DSPF_ARGB;
  if (IsEnvOne(ENV_DFB_PREMULT))
  {
    // creation-time capability, cannot be switched later
    description.flags =
        static_cast<DFBWindowDescriptionFlags>(description.flags | DWDESC_SURFACE_CAPS);
    description.surface_caps = DSCAPS_PREMULTIPLIED;
    CLog::Log(LOGINFO, "CWinSystemDirectFB::{} - {}=1: premultiplied window surface",
              __FUNCTION__, ENV_DFB_PREMULT);
  }

  if (!Succeeded(m_dfbLayer->CreateWindow(m_dfbLayer, &description, &m_dfbWindow),
                 "IDirectFBDisplayLayer::CreateWindow"))
  {
    m_dfbWindow = nullptr;
    return false;
  }

  // Composite the window opaque: Kodi's GLES rendering does not keep the destination alpha at 1.0
  // (e.g. the dim screensaver quad, cross fades, the Clear(0) around video), and the layer
  // background is transparent, so the TV UI below would shine through. Opaque is also a plain
  // copy for the window manager instead of blending every window below on each Flip(). Not fatal:
  // the window still works with blending, only with those artifacts.
  SetPerPixelAlpha(IsWindowAlphaRequested());
  m_underlayBlend = IsEnvOne(ENV_UNDERLAY_BLEND);

  if (IsEnvOne(ENV_DFB_GHOST))
  {
    DFBWindowOptions options = DWOP_NONE;
    if (Succeeded(m_dfbWindow->GetOptions(m_dfbWindow, &options), "IDirectFBWindow::GetOptions"))
    {
      const DFBWindowOptions ghost =
          static_cast<DFBWindowOptions>(static_cast<unsigned int>(options) | DWOP_GHOST);
      if (Succeeded(m_dfbWindow->SetOptions(m_dfbWindow, ghost),
                    "IDirectFBWindow::SetOptions(+DWOP_GHOST)"))
        CLog::Log(LOGINFO, "CWinSystemDirectFB::{} - {}=1: window without focus and keys",
                  __FUNCTION__, ENV_DFB_GHOST);
    }
  }

  if (!Succeeded(m_dfbWindow->GetSurface(m_dfbWindow, &m_dfbSurface),
                 "IDirectFBWindow::GetSurface"))
  {
    m_dfbSurface = nullptr;
    return false;
  }

  int width = 0;
  int height = 0;
  if (!Succeeded(m_dfbSurface->GetSize(m_dfbSurface, &width, &height),
                 "IDirectFBSurface::GetSize") ||
      width <= 0 || height <= 0)
  {
    width = description.width;
    height = description.height;
  }
  m_surfaceWidth = width;
  m_surfaceHeight = height;

  // Show opaque black instead of uninitialized memory until Kodi renders its first frame
  Succeeded(m_dfbSurface->Clear(m_dfbSurface, 0x00, 0x00, 0x00, 0xff), "IDirectFBSurface::Clear");
  Succeeded(m_dfbSurface->Flip(m_dfbSurface, nullptr, DSFLIP_NONE), "IDirectFBSurface::Flip");

  // New DirectFB windows are invisible (opacity 0)
  if (!Succeeded(m_dfbWindow->SetOpacity(m_dfbWindow, 0xff), "IDirectFBWindow::SetOpacity(0xff)"))
    return false;

  // Upper stacking class: the TV launcher's window (default class DWSC_MIDDLE) can never cover
  // Kodi, even if the launcher raises or redraws its own window while Kodi runs (on the TV the
  // launcher's screen saver started behind Kodi and Kodi was no longer visible).
  Succeeded(m_dfbWindow->SetStackingClass(m_dfbWindow, DWSC_UPPER),
            "IDirectFBWindow::SetStackingClass(DWSC_UPPER)");
  Succeeded(m_dfbWindow->RaiseToTop(m_dfbWindow), "IDirectFBWindow::RaiseToTop");

  CLog::Log(LOGINFO, "CWinSystemDirectFB::{} - created {}x{} ARGB window, surface {}x{}",
            __FUNCTION__, description.width, description.height, m_surfaceWidth,
            m_surfaceHeight);
  return true;
}

bool CWinSystemDirectFB::SetPerPixelAlpha(bool enable)
{
  if (!m_dfbWindow)
    return false;

  DFBWindowOptions options = DWOP_NONE;
  if (!Succeeded(m_dfbWindow->GetOptions(m_dfbWindow, &options), "IDirectFBWindow::GetOptions"))
    return false;

  const unsigned int oldOptions = static_cast<unsigned int>(options);
  const unsigned int alphaOption = static_cast<unsigned int>(DWOP_ALPHACHANNEL);
  const unsigned int newOptions = enable ? (oldOptions | alphaOption) : (oldOptions & ~alphaOption);

  if (newOptions != oldOptions &&
      !Succeeded(m_dfbWindow->SetOptions(m_dfbWindow, static_cast<DFBWindowOptions>(newOptions)),
                 enable ? "IDirectFBWindow::SetOptions(+DWOP_ALPHACHANNEL)"
                        : "IDirectFBWindow::SetOptions(-DWOP_ALPHACHANNEL)"))
    return false;

  // Read back what the window manager actually applied
  DFBWindowOptions applied = static_cast<DFBWindowOptions>(newOptions);
  Succeeded(m_dfbWindow->GetOptions(m_dfbWindow, &applied), "IDirectFBWindow::GetOptions");
  const unsigned int appliedOptions = static_cast<unsigned int>(applied);

  CLog::Log(LOGINFO,
            "CWinSystemDirectFB::{} - per-pixel alpha {}: window options {:#010x} -> {:#010x}",
            __FUNCTION__, enable ? "on" : "off", oldOptions, appliedOptions);

  if (((appliedOptions & alphaOption) != 0) != enable)
  {
    CLog::Log(LOGWARNING,
              "CWinSystemDirectFB::{} - window manager did not apply per-pixel alpha {}",
              __FUNCTION__, enable ? "on" : "off");
    return false;
  }

  return true;
}

void CWinSystemDirectFB::ApplyVideoUnderlay(bool enable)
{
  if (!m_dfbWindow)
    return;

  // surface, EGL surface, opacity, layer and stacking class stay as they are
  if (m_underlayBlend)
    SetPerPixelAlpha(enable || IsWindowAlphaRequested());

  CLog::Log(LOGINFO, "CWinSystemDirectFB::{} - video underlay {} ({})", __FUNCTION__,
            enable ? "on" : "off", m_underlayBlend ? "blend" : "copy");
}

void CWinSystemDirectFB::ReleaseDirectFB()
{
  if (m_dfbSurface)
  {
    m_dfbSurface->Release(m_dfbSurface);
    m_dfbSurface = nullptr;
  }

  if (m_dfbWindow)
  {
    m_dfbWindow->SetOpacity(m_dfbWindow, 0x00);
    m_dfbWindow->Destroy(m_dfbWindow);
    m_dfbWindow->Release(m_dfbWindow);
    m_dfbWindow = nullptr;
  }

  if (m_dfbLayer)
  {
    m_dfbLayer->Release(m_dfbLayer);
    m_dfbLayer = nullptr;
  }

  if (m_dfb)
  {
    m_dfb->Release(m_dfb);
    m_dfb = nullptr;
  }

  m_surfaceWidth = 0;
  m_surfaceHeight = 0;
}

void CWinSystemDirectFB::StartInput()
{
  const char* env = std::getenv("FLEX_KODI_INPUT");
  const std::string device = (env && *env) ? env : DEFAULT_INPUT_DEVICE;

  CLog::Log(LOGINFO, "CWinSystemDirectFB::{} - remote control input from {}", __FUNCTION__,
            device);

  m_inputHandler = std::make_unique<CEvdevInputHandler>(device);
  m_inputHandler->Start();
}

void CWinSystemDirectFB::StopInput()
{
  // stops the reader thread and closes the device
  m_inputHandler.reset();
}

void CWinSystemDirectFB::UpdateResolutions()
{
  CWinSystemBase::UpdateResolutions();

  const int width = m_surfaceWidth > 0 ? m_surfaceWidth : DEFAULT_WIDTH;
  const int height = m_surfaceHeight > 0 ? m_surfaceHeight : DEFAULT_HEIGHT;

  // The window covers the whole layer and DirectFB cannot switch display modes here, so there is
  // exactly one resolution: the desktop.
  RESOLUTION_INFO res;
  UpdateDesktopResolution(res, "DirectFB", width, height, DEFAULT_REFRESH_RATE, 0);
  GetGfxContext().ResetOverscan(res);

  CDisplaySettings::GetInstance().ClearCustomResolutions();
  CDisplaySettings::GetInstance().AddResolutionInfo(res);
  CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP) = res;

  CLog::Log(LOGINFO, "CWinSystemDirectFB::{} - desktop resolution {} ({}x{} @ {:.2f} Hz)",
            __FUNCTION__, res.strMode, res.iWidth, res.iHeight, res.fRefreshRate);

  CDisplaySettings::GetInstance().ApplyCalibrations();
}

bool CWinSystemDirectFB::Hide()
{
  if (!m_dfbWindow)
    return false;

  return Succeeded(m_dfbWindow->SetOpacity(m_dfbWindow, 0x00), "IDirectFBWindow::SetOpacity(0)");
}

bool CWinSystemDirectFB::Show(bool raise)
{
  if (!m_dfbWindow)
    return false;

  if (!Succeeded(m_dfbWindow->SetOpacity(m_dfbWindow, 0xff), "IDirectFBWindow::SetOpacity(0xff)"))
    return false;

  if (raise)
    Succeeded(m_dfbWindow->RaiseToTop(m_dfbWindow), "IDirectFBWindow::RaiseToTop");

  return true;
}

void CWinSystemDirectFB::Register(IDispResource* resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  m_resources.push_back(resource);
}

void CWinSystemDirectFB::Unregister(IDispResource* resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  m_resources.erase(std::remove(m_resources.begin(), m_resources.end(), resource),
                    m_resources.end());
}
