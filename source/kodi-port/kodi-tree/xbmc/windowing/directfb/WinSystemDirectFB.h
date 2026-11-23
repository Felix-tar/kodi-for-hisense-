/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "threads/CriticalSection.h"
#include "windowing/WinSystem.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// DirectFB 1.4 declares its interfaces with DECLARE_INTERFACE(IFACE), which expands to
// "typedef struct _IFACE IFACE;". Repeating these typedefs keeps <directfb.h> (and its global
// u8/u16/... typedefs and macros) out of everything that includes this header.
typedef struct _IDirectFB IDirectFB;
typedef struct _IDirectFBDisplayLayer IDirectFBDisplayLayer;
typedef struct _IDirectFBWindow IDirectFBWindow;
typedef struct _IDirectFBSurface IDirectFBSurface;

class IDispResource;

namespace KODI
{
namespace WINDOWING
{
namespace DIRECTFB
{

class CEvdevInputHandler;

/*!
 * \brief Window system for TVs whose graphics stack is DirectFB 1.4 (MStar based Hisense TVs).
 *
 * Kodi joins the running DirectFB session of the TV as an additional client (like the launcher
 * or the browser of the TV), takes the primary layer in shared mode and creates one full-screen
 * ARGB window on it. The surface of that window is the native window the render system specific
 * subclass hands to EGL.
 *
 * DirectFB is configured by the environment only (DIRECTFB_SESSION, DFBARGS, set by the launch
 * script), so DirectFBInit() is called without command line.
 *
 * The window surface is ARGB with DWCAPS_ALPHACHANNEL (the combination EGL is proven to work
 * with), but the window is composited opaque: DWOP_ALPHACHANNEL is switched off after creation,
 * because several GLES draw paths of Kodi leave the destination alpha below 1.0 and the TV UI
 * below would shine through. Setting the environment variable FLEX_KODI_WINDOW_ALPHA to "1" keeps
 * per-pixel alpha blending on (for testing); \ref SetPerPixelAlpha switches it at runtime.
 *
 * Remote control input does not come from DirectFB but from an evdev device that is read by a
 * \ref CEvdevInputHandler. Its device is taken from the environment variable FLEX_KODI_INPUT
 * (default "/dev/input/event0").
 *
 * Video underlay (TVPlayer: the TV's media player plays on the video plane below Kodi): while a
 * player reports CDataCacheCore::SetVideoRender(true), the render context stops making every
 * frame opaque, so Kodi's transparent pixels show the TV picture. Switches, read once:
 * - FLEX_KODI_UNDERLAY_BLEND=1: "blend" mode, DWOP_ALPHACHANNEL is on during the underlay
 *   (default "copy": the window manager copies the ARGB pixels with their alpha into the layer).
 * - FLEX_KODI_DFB_PREMULT=1: window surface with DSCAPS_PREMULTIPLIED (creation time only).
 * - FLEX_KODI_DFB_GHOST=1: window with DWOP_GHOST (no focus, no DirectFB keys; test for P9).
 * - FLEX_KODI_UNDERLAY_TEST=1: underlay requested from the start, without a player (test S5).
 */
class CWinSystemDirectFB : public CWinSystemBase
{
public:
  CWinSystemDirectFB();
  ~CWinSystemDirectFB() override;

  const std::string GetName() override { return "directfb"; }

  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;

  void UpdateResolutions() override;

  bool CanDoWindowed() override { return false; }
  bool HasCursor() override { return false; }
  bool SupportsScreenMove() override { return false; }

  bool Hide() override;
  bool Show(bool raise = true) override;

  void Register(IDispResource* resource) override;
  void Unregister(IDispResource* resource) override;

  /*!
   * \brief Request (from any thread) that the TV's video plane shows through Kodi's transparent
   *        pixels. Only stores the request; it takes effect on the render thread before the next
   *        present. A player that sets CDataCacheCore::SetVideoRender(true) gets the same without
   *        calling this.
   */
  void SetVideoUnderlay(bool enable) { m_underlayRequested = enable; }

protected:
  bool IsVideoUnderlayRequested() const { return m_underlayRequested; }

  /*!
   * \brief DirectFB side of the video underlay, render thread only.
   *
   * "copy" (default): nothing changes on DirectFB. The window stays without DWOP_ALPHACHANNEL,
   * the window manager copies its ARGB pixels including alpha into the layer, the launcher
   * window below stays covered and the TV mixes the alpha with the video plane.
   * "blend" (FLEX_KODI_UNDERLAY_BLEND=1): DWOP_ALPHACHANNEL is on while enabled; the window is
   * then blended with the launcher window below, which must be transparent itself (flex.js).
   */
  void ApplyVideoUnderlay(bool enable);

  /*!
   * \brief The surface of the DirectFB window, valid between a successful InitWindowSystem() and
   *        DestroyWindowSystem(). Anything created on top of it (e.g. an EGL window surface) must
   *        be destroyed before DestroyWindowSystem() of this class runs.
   */
  IDirectFBSurface* GetDirectFBSurface() const { return m_dfbSurface; }

  /*!
   * \brief Size of the DirectFB window surface in pixels (0 before InitWindowSystem()).
   */
  int GetSurfaceWidth() const { return m_surfaceWidth; }
  int GetSurfaceHeight() const { return m_surfaceHeight; }

  /*!
   * \brief Switches per-pixel alpha blending (DWOP_ALPHACHANNEL) of the DirectFB window.
   *
   * Off (the default after InitWindowSystem()), the window manager copies the window opaque and
   * the alpha channel Kodi renders is ignored. On, the window is blended with the layer content
   * below it using its alpha channel; this is meant for a later stage where video is shown on a
   * plane below the GUI and only while that player is active.
   *
   * Only the compositing changes; the surface (and with it the EGL surface) stays ARGB.
   *
   * \param enable true to blend with the alpha channel, false for an opaque window
   * \return true if the window reports the requested state afterwards
   */
  bool SetPerPixelAlpha(bool enable);

private:
  bool CreateDirectFBWindow();
  void ReleaseDirectFB();
  void StartInput();
  void StopInput();

  // Released in reverse order of creation: surface, window, layer, IDirectFB
  IDirectFB* m_dfb = nullptr;
  IDirectFBDisplayLayer* m_dfbLayer = nullptr;
  IDirectFBWindow* m_dfbWindow = nullptr;
  IDirectFBSurface* m_dfbSurface = nullptr;

  int m_surfaceWidth = 0;
  int m_surfaceHeight = 0;

  std::unique_ptr<CEvdevInputHandler> m_inputHandler;

  std::atomic<bool> m_underlayRequested{false};
  // FLEX_KODI_UNDERLAY_BLEND=1
  bool m_underlayBlend = false;

  CCriticalSection m_resourceSection;
  std::vector<IDispResource*> m_resources;
};

} // namespace DIRECTFB
} // namespace WINDOWING
} // namespace KODI
