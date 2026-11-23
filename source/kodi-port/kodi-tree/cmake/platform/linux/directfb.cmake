# DirectFB windowing for TVs whose user interface runs on DirectFB 1.4 (e.g. MStar based
# Hisense/VIDAA sets). Kodi joins the running DirectFB session as one more client, renders with
# OpenGL ES 2 through EGL on a DirectFB window surface and reads the remote control via evdev.
# These devices have no DRM/GBM, X11, Wayland, udev or libinput.

if(NOT APP_RENDER_SYSTEM STREQUAL "gles")
  message(FATAL_ERROR "CORE_PLATFORM_NAME=directfb only supports OpenGL ES rendering. Please set APP_RENDER_SYSTEM to \"gles\"")
endif()

list(APPEND PLATFORM_REQUIRED_DEPS DirectFB OpenGLES EGL)
list(APPEND ARCH_DEFINES -DTARGET_DIRECTFB)
