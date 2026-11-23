/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemDirectFBGLESContext.h"

#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "ServiceBroker.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "rendering/gles/ScreenshotSurfaceGLES.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"
#include "windowing/WindowSystemFactory.h"

#include <cstdlib>
#include <cstring>
#include <memory>

using namespace KODI::WINDOWING::DIRECTFB;
using namespace std::chrono_literals;

namespace
{
// Divides the colour by alpha in place (straight alpha for a GOP that mixes a*OSD + (1-a)*video).
// Framebuffer fetch reads the pixel under the fragment, so no copy of the frame buffer is needed.
const char* UNPREMULTIPLY_FS = "#extension GL_ARM_shader_framebuffer_fetch : require\n"
                               "precision mediump float;\n"
                               "void main()\n"
                               "{\n"
                               "  vec4 c = gl_LastFragColorARM;\n"
                               "  gl_FragColor = c.a > 0.0 ? vec4(min(c.rgb / c.a, vec3(1.0)), c.a)"
                               " : vec4(0.0);\n"
                               "}\n";
const char* UNPREMULTIPLY_VS = "attribute vec2 a_pos;\n"
                               "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

GLuint CompileShader(GLenum type, const char* source)
{
  const GLuint shader = glCreateShader(type);
  if (!shader)
    return 0;
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE)
  {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
    CLog::Log(LOGERROR, "CWinSystemDirectFBGLESContext: unpremultiply shader: {}", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}
} // namespace

void CWinSystemDirectFBGLESContext::Register()
{
  KODI::WINDOWING::CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem, "directfb");
}

std::unique_ptr<CWinSystemBase> CWinSystemDirectFBGLESContext::CreateWinSystem()
{
  return std::make_unique<CWinSystemDirectFBGLESContext>();
}

bool CWinSystemDirectFBGLESContext::InitWindowSystem()
{
  const char* keepAlpha = std::getenv("FLEX_KODI_WINDOW_ALPHA");
  m_keepAlpha = keepAlpha && std::strcmp(keepAlpha, "1") == 0;
  // only if the TV turns out to mix the layer with straight alpha (P10/P15)
  const char* straight = std::getenv("FLEX_KODI_GOP_STRAIGHT");
  m_unpremultiply = straight && std::strcmp(straight, "1") == 0;
  m_underlay = false;

  VIDEOPLAYER::CRendererFactory::ClearRenderer();
  CDVDFactoryCodec::ClearHWAccels();
  CLinuxRendererGLES::Register();
  CScreenshotSurfaceGLES::Register();

  // DirectFB first: the EGL implementation attaches to the DirectFB session of this process
  if (!CWinSystemDirectFB::InitWindowSystem())
    return false;

  bool ok = m_eglContext.CreateDisplay(EGL_DEFAULT_DISPLAY) &&
            m_eglContext.InitializeDisplay(EGL_OPENGL_ES_API) &&
            m_eglContext.ChooseConfig(EGL_OPENGL_ES2_BIT);

  if (ok)
  {
    CEGLAttributesVec contextAttribs;
    contextAttribs.Add({{EGL_CONTEXT_CLIENT_VERSION, 2}});

    ok = m_eglContext.CreateContext(contextAttribs);
    if (!ok)
      CEGLUtils::Log(LOGERROR, "CWinSystemDirectFBGLESContext: failed to create EGL context");
  }

  if (!ok)
  {
    CLog::Log(LOGERROR, "CWinSystemDirectFBGLESContext::{} - EGL initialization failed",
              __FUNCTION__);
    // Do not leave the (black) DirectFB window on top of the TV UI
    DestroyWindowSystem();
    return false;
  }

  CLog::Log(LOGINFO, "CWinSystemDirectFBGLESContext::{} - EGL initialized", __FUNCTION__);
  return true;
}

bool CWinSystemDirectFBGLESContext::DestroyWindowSystem()
{
  CDVDFactoryCodec::ClearHWAccels();
  VIDEOPLAYER::CRendererFactory::ClearRenderer();

  if (m_unpremultiplyProgram)
  {
    glDeleteProgram(m_unpremultiplyProgram); // while the context still exists
    m_unpremultiplyProgram = 0;
  }

  // context, surface and display; must happen before the DirectFB surface is released
  m_eglContext.Destroy();
  m_bWindowCreated = false;

  return CWinSystemDirectFB::DestroyWindowSystem();
}

bool CWinSystemDirectFBGLESContext::CreateNewWindow(const std::string& name,
                                                    bool fullScreen,
                                                    RESOLUTION_INFO& res)
{
  IDirectFBSurface* surface = GetDirectFBSurface();
  if (!surface)
  {
    CLog::Log(LOGERROR, "CWinSystemDirectFBGLESContext::{} - no DirectFB window surface",
              __FUNCTION__);
    return false;
  }

  // The DirectFB window lives as long as the window system, so the EGL surface is only created
  // once (or again after DestroyWindow()).
  if (m_eglContext.GetEGLSurface() == EGL_NO_SURFACE)
  {
    // EGLNativeWindowType is a pointer or an integer type depending on the EGL headers
    static_assert(sizeof(EGLNativeWindowType) >= sizeof(IDirectFBSurface*),
                  "EGLNativeWindowType cannot hold an IDirectFBSurface pointer");

    if (!m_eglContext.CreateSurface(reinterpret_cast<EGLNativeWindowType>(surface)))
    {
      CLog::Log(LOGERROR, "CWinSystemDirectFBGLESContext::{} - failed to create EGL window surface",
                __FUNCTION__);
      return false;
    }
  }

  if (!m_eglContext.BindContext())
  {
    CLog::Log(LOGERROR, "CWinSystemDirectFBGLESContext::{} - failed to bind EGL context",
              __FUNCTION__);
    return false;
  }

  m_nWidth = res.iWidth;
  m_nHeight = res.iHeight;
  m_fRefreshRate = res.fRefreshRate;
  m_bFullScreen = fullScreen;
  m_bWindowCreated = true;
  m_swapFailureLogged = false;

  CLog::Log(LOGINFO, "CWinSystemDirectFBGLESContext::{} - {}x{} @ {:.2f} Hz on a {}x{} surface",
            __FUNCTION__, m_nWidth, m_nHeight, m_fRefreshRate, GetSurfaceWidth(),
            GetSurfaceHeight());
  return true;
}

bool CWinSystemDirectFBGLESContext::DestroyWindow()
{
  // Only the EGL surface; the DirectFB window is released in DestroyWindowSystem()
  m_eglContext.DestroySurface();
  m_bWindowCreated = false;
  return true;
}

bool CWinSystemDirectFBGLESContext::ResizeWindow(int newWidth,
                                                 int newHeight,
                                                 int newLeft,
                                                 int newTop)
{
  // The DirectFB window always covers the whole layer (CanDoWindowed() is false)
  m_nWidth = newWidth;
  m_nHeight = newHeight;
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);
  return true;
}

bool CWinSystemDirectFBGLESContext::SetFullScreen(bool fullScreen,
                                                  RESOLUTION_INFO& res,
                                                  bool blankOtherDisplays)
{
  // There is no mode switch: the window and its EGL surface stay, only the GUI size is applied.
  // No OnLostDisplay()/OnResetDisplay() either, the GL context survives.
  if (res.iWidth != GetSurfaceWidth() || res.iHeight != GetSurfaceHeight())
  {
    CLog::Log(LOGWARNING,
              "CWinSystemDirectFBGLESContext::{} - resolution {}x{} differs from the {}x{} "
              "DirectFB surface",
              __FUNCTION__, res.iWidth, res.iHeight, GetSurfaceWidth(), GetSurfaceHeight());
  }

  m_nWidth = res.iWidth;
  m_nHeight = res.iHeight;
  m_fRefreshRate = res.fRefreshRate;
  m_bFullScreen = fullScreen;

  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);
  return true;
}

void CWinSystemDirectFBGLESContext::SetVSyncImpl(bool enable)
{
  if (!m_eglContext.SetVSync(enable))
    CEGLUtils::Log(LOGWARNING, "CWinSystemDirectFBGLESContext: eglSwapInterval failed");
}

void CWinSystemDirectFBGLESContext::PresentRender(bool rendered, bool videoLayer)
{
  const bool underlay = videoLayer || IsVideoUnderlayRequested();
  if (underlay != m_underlay)
  {
    m_underlay = underlay;
    ApplyVideoUnderlay(underlay);
    // the frame on screen was made for the other mode: draw a complete new one
    if (CServiceBroker::GetGUI())
      CServiceBroker::GetGUI()->GetWindowManager().MarkDirty();
  }

  CRenderSystemGLES::PresentRender(rendered, videoLayer);

  // CRenderSystemGLES only sleeps without a video layer (RenderSystemGLES.cpp:244-245); the TV's
  // player does not pace this loop, so it would spin without a pause
  if (!rendered && videoLayer)
    KODI::TIME::Sleep(20ms);
}

void CWinSystemDirectFBGLESContext::MakeAlphaOpaque()
{
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  if (scissor)
    glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
}

bool CWinSystemDirectFBGLESContext::CreateUnpremultiplyProgram()
{
  const GLuint vs = CompileShader(GL_VERTEX_SHADER, UNPREMULTIPLY_VS);
  const GLuint fs = vs ? CompileShader(GL_FRAGMENT_SHADER, UNPREMULTIPLY_FS) : 0;
  if (!vs || !fs)
  {
    if (vs)
      glDeleteShader(vs);
    return false;
  }
  const GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glBindAttribLocation(program, 0, "a_pos");
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok != GL_TRUE)
  {
    glDeleteProgram(program);
    CLog::Log(LOGERROR, "CWinSystemDirectFBGLESContext: unpremultiply program does not link");
    return false;
  }
  m_unpremultiplyProgram = program;
  return true;
}

void CWinSystemDirectFBGLESContext::UnpremultiplyFrameBuffer()
{
  // IsExtSupported(): RenderSystemGLES.cpp:217-233
  if (!m_unpremultiplyProgram &&
      (!IsExtSupported("GL_ARM_shader_framebuffer_fetch") || !CreateUnpremultiplyProgram()))
  {
    CLog::Log(LOGWARNING, "CWinSystemDirectFBGLESContext: FLEX_KODI_GOP_STRAIGHT needs "
                          "GL_ARM_shader_framebuffer_fetch, colours stay premultiplied");
    m_unpremultiply = false;
    return;
  }

  GLint program = 0;
  GLint buffer = 0;
  GLint viewport[4] = {};
  GLint attribEnabled = GL_FALSE;
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &buffer);
  glGetIntegerv(GL_VIEWPORT, viewport);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attribEnabled);
  const GLboolean blend = glIsEnabled(GL_BLEND);
  const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, GetSurfaceWidth(), GetSurfaceHeight());

  static const GLfloat quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
  glUseProgram(m_unpremultiplyProgram);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  if (attribEnabled != GL_TRUE)
    glDisableVertexAttribArray(0);

  glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(buffer));
  glUseProgram(static_cast<GLuint>(program));
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  if (blend)
    glEnable(GL_BLEND);
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
}

void CWinSystemDirectFBGLESContext::PresentRenderImpl(bool rendered)
{
  if (!rendered || m_eglContext.GetEGLSurface() == EGL_NO_SURFACE)
    return;

  // Kodi clears with alpha 0 and some draws (dim screen saver, video bars) leave alpha below 1.
  // The TV's graphics layer blends by per-pixel alpha with what lies below (other layers, the
  // video plane), so make every pixel opaque before the swap: only the alpha channel is cleared
  // to 1, colours stay. Not during the video underlay (TVPlayer): then Kodi's alpha is exactly
  // what shows the TV picture. FLEX_KODI_WINDOW_ALPHA=1 keeps Kodi's alpha always (test).
  if (!m_underlay && !m_keepAlpha)
    MakeAlphaOpaque();
  else if (m_underlay && m_unpremultiply)
    UnpremultiplyFrameBuffer(); // only if P10 shows straight mixing (FLEX_KODI_GOP_STRAIGHT=1)

  // each eglSwapBuffers() flips the DirectFB window surface
  if (m_eglContext.TrySwapBuffers())
  {
    m_swapFailureLogged = false;
  }
  else if (!m_swapFailureLogged)
  {
    // log once per series of failures instead of once per frame
    CEGLUtils::Log(LOGERROR, "CWinSystemDirectFBGLESContext: eglSwapBuffers failed");
    m_swapFailureLogged = true;
  }
}

EGLDisplay CWinSystemDirectFBGLESContext::GetEGLDisplay() const
{
  return m_eglContext.GetEGLDisplay();
}

EGLSurface CWinSystemDirectFBGLESContext::GetEGLSurface() const
{
  return m_eglContext.GetEGLSurface();
}

EGLContext CWinSystemDirectFBGLESContext::GetEGLContext() const
{
  return m_eglContext.GetEGLContext();
}

EGLConfig CWinSystemDirectFBGLESContext::GetEGLConfig() const
{
  return m_eglContext.GetEGLConfig();
}
