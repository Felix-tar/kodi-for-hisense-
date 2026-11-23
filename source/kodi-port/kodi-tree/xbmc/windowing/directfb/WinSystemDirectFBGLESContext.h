/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "WinSystemDirectFB.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/EGLUtils.h"

#include <memory>
#include <string>

namespace KODI
{
namespace WINDOWING
{
namespace DIRECTFB
{

/*!
 * \brief OpenGL ES 2.0 rendering on the DirectFB window via EGL.
 *
 * The EGL implementation of the TV (Mali, DirectFB backend) uses the default EGL display and
 * takes the IDirectFBSurface* of the window as EGLNativeWindowType; every eglSwapBuffers() is a
 * Flip() of that surface.
 */
class CWinSystemDirectFBGLESContext : public CWinSystemDirectFB, public CRenderSystemGLES
{
public:
  CWinSystemDirectFBGLESContext() = default;
  ~CWinSystemDirectFBGLESContext() override = default;

  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // Implementation of CWinSystemBase via CWinSystemDirectFB
  CRenderSystemBase* GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name, bool fullScreen, RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;
  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  EGLDisplay GetEGLDisplay() const;
  EGLSurface GetEGLSurface() const;
  EGLContext GetEGLContext() const;
  EGLConfig GetEGLConfig() const;

  /*!
   * \brief Decides per frame whether Kodi stays opaque or lets the TV's video plane show through.
   *
   * videoLayer = CApplicationPlayer::IsRenderingVideoLayer() (Application.cpp:924-925): true only
   * while a player shows its picture on the TV's video plane and says so with
   * CDataCacheCore::SetVideoRender(true) (the TVPlayer). Kodi's own GLES renderer never does, so
   * without the TVPlayer every frame stays opaque as before.
   */
  void PresentRender(bool rendered, bool videoLayer) override;

protected:
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override;

private:
  void MakeAlphaOpaque();
  void UnpremultiplyFrameBuffer();
  bool CreateUnpremultiplyProgram();

  // As a member it is destroyed before the base classes, so even without DestroyWindowSystem()
  // the EGL surface is gone before ~CWinSystemDirectFB releases the DirectFB surface.
  CEGLContextUtils m_eglContext;
  bool m_swapFailureLogged = false;
  // FLEX_KODI_WINDOW_ALPHA=1: always keep Kodi's alpha (otherwise frames are made opaque)
  bool m_keepAlpha = false;
  // video underlay in effect for the frame being presented
  bool m_underlay = false;
  // FLEX_KODI_GOP_STRAIGHT=1: the TV mixes the layer with straight alpha -> divide colour by alpha
  bool m_unpremultiply = false;
  GLuint m_unpremultiplyProgram = 0;
};

} // namespace DIRECTFB
} // namespace WINDOWING
} // namespace KODI
