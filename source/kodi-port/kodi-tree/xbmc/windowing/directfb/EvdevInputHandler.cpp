/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "EvdevInputHandler.h"

#include "FlexVolumeBar.h"

#include "ServiceBroker.h"
#include "application/AppInboundProtocol.h"
#include "application/ApplicationComponents.h"
#include "application/ApplicationPlayer.h"
#include "application/ApplicationPowerHandling.h"
#include "cores/TVPlayer/TVBridgeIO.h"
#include "cores/TVPlayer/TVPlayerBridge.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "input/keyboard/XBMC_keysym.h"
#include "messaging/ApplicationMessenger.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "windowing/WinSystem.h"
#include "windowing/XBMC_events.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace KODI::WINDOWING::DIRECTFB;

namespace
{
constexpr const char* DEFAULT_DEVICE = "/dev/input/event0";
constexpr const char* ENV_DEVICE = "FLEX_KODI_INPUT";
constexpr const char* ENV_GRAB = "FLEX_KODI_GRAB";
constexpr const char* ENV_REPEAT_DELAY = "FLEX_KODI_REPEAT_DELAY_MS";
// Directory for small state files read by the TV launcher integration (kodi-run.sh): "ready"
// once the remote is grabbed, "lastkey" = time (s) of the last key press. Unset = no files.
constexpr const char* ENV_STATE_DIR = "FLEX_KODI_STATE_DIR";
// "1": volume/mute go to the TV or AVR through flex.js (bridge file tvkeys), see the class doc
constexpr const char* ENV_TVKEYS = "FLEX_KODI_TVKEYS";

// Holding a volume key: at most one step per interval (kernel autorepeat period is 70 ms)
constexpr int64_t TVKEY_STEP_MS = 140;
// Registration with flex.js: repeated every second, at most this long (flex.js gets the run key
// only up to 3 s after "ready")
constexpr int64_t TVKEYS_REGISTER_MS = 10000;
// Home during a film: Kodi hides this long after the stop (flex.js stops the TV player 0.4 s after
// resetting its window, the stop itself takes well under a second)
constexpr int64_t MINIMIZE_AFTER_STOP_MS = 1500;
constexpr int64_t TVKEYS_REGISTER_REPEAT_MS = 1000;
// every tvkeys must be acknowledged in tvack within this time
constexpr int64_t TVKEYS_ACK_MS = 1000;
// the volume hint shows the acknowledged value once the keys are quiet for this long
constexpr int64_t TVKEYS_HINT_QUIET_MS = 300;
// tvack is still read this long after the last key press (second acknowledgement with the value)
constexpr int64_t TVKEYS_HINT_WINDOW_MS = 2000;
constexpr unsigned int TVKEYS_HINT_DISPLAY_MS = 1500;

// Upper bound for the time StopThread() has to wait for the reader thread
constexpr int POLL_TIMEOUT_MS = 100;
// Interval for re-opening the device after a failure (interrupted immediately by StopThread())
constexpr auto RETRY_INTERVAL = std::chrono::seconds(2);
// Upper bound for FLEX_KODI_REPEAT_DELAY_MS
constexpr long MAX_REPEAT_DELAY_MS = 10000;
// Holding Power/Sleep this long while Kodi does not quit ends the process at once (see ForceQuit)
constexpr int FORCE_QUIT_HOLD_MS = 5000;

// evdev EV_KEY values
constexpr int32_t KEY_VALUE_RELEASE = 0;
constexpr int32_t KEY_VALUE_PRESS = 1;
constexpr int32_t KEY_VALUE_REPEAT = 2;

// Marks a key event without a preceding MSC_SCAN in the log set
constexpr int64_t NO_SCAN_CODE = -1;

constexpr size_t BITS_PER_ULONG = sizeof(unsigned long) * 8;

std::string ResolveDevice(std::string device)
{
  if (!device.empty())
    return device;

  const char* env = std::getenv(ENV_DEVICE);
  if (env && *env)
    return env;

  return DEFAULT_DEVICE;
}

bool IsGrabRequested()
{
  const char* env = std::getenv(ENV_GRAB);
  return env && std::strcmp(env, "1") == 0;
}

bool IsTvKeysRequested()
{
  const char* env = std::getenv(ENV_TVKEYS);
  return env && std::strcmp(env, "1") == 0;
}

std::string MakeTvKeysKid()
{
  return StringUtils::Format("{:08x}-{:04x}",
                             static_cast<unsigned int>(std::time(nullptr)) & 0xffffffffU,
                             static_cast<unsigned int>(getpid()) & 0xffffU);
}

int64_t SteadyNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool IsVolumeKey(uint16_t code)
{
  return code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN || code == KEY_MUTE;
}

int GetRepeatDelayMs()
{
  const char* env = std::getenv(ENV_REPEAT_DELAY);
  if (!env || !*env)
    return 0;

  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(env, &end, 10);
  if (errno != 0 || end == env || *end != '\0' || value < 0 || value > MAX_REPEAT_DELAY_MS)
  {
    CLog::Log(LOGWARNING,
              "CEvdevInputHandler::{} - ignoring invalid {}=\"{}\" (expected 0 to {} ms)",
              __FUNCTION__, ENV_REPEAT_DELAY, env, MAX_REPEAT_DELAY_MS);
    return 0;
  }

  return static_cast<int>(value);
}

int64_t EventTimeMs(const input_event& event)
{
#if defined(input_event_sec)
  const int64_t sec = static_cast<int64_t>(event.input_event_sec);
  const int64_t usec = static_cast<int64_t>(event.input_event_usec);
#else
  const int64_t sec = static_cast<int64_t>(event.time.tv_sec);
  const int64_t usec = static_cast<int64_t>(event.time.tv_usec);
#endif
  return sec * 1000 + usec / 1000;
}

template<size_t N>
bool TestBit(unsigned int bit, const std::array<unsigned long, N>& bits)
{
  const size_t index = bit / BITS_PER_ULONG;
  return index < N && ((bits[index] >> (bit % BITS_PER_ULONG)) & 1UL) != 0;
}

const char* YesNo(bool value)
{
  return value ? "yes" : "no";
}

/*!
 * \brief Keys the TV handles itself: Kodi leaves them alone while the device is not grabbed.
 */
bool IsTvKey(uint16_t code)
{
  switch (code)
  {
    case KEY_POWER:
    case KEY_SLEEP:
    case KEY_VOLUMEUP:
    case KEY_VOLUMEDOWN:
    case KEY_MUTE:
      return true;
    default:
      return false;
  }
}

/*!
 * \brief Keys that make Kodi quit while the device is grabbed, so the TV gets its remote back.
 */
bool IsQuitKey(uint16_t code)
{
  return code == KEY_POWER || code == KEY_SLEEP;
}

/*!
 * \brief Keys that toggle a state: holding them must not toggle it again with every autorepeat.
 */
bool IsToggleKey(uint16_t code)
{
  return code == KEY_MUTE || code == KEY_RECORD;
}

/*!
 * \brief Writes a small state file into FLEX_KODI_STATE_DIR (no-op if unset). Never follows a
 *        symlink; errors are ignored (the files only feed the TV launcher's screen saver logic).
 */
//! true if the state file \p name existed; it is removed (a request from kodi.sh, e.g. "restore")
bool TakeStateFile(const char* name)
{
  const char* dir = std::getenv(ENV_STATE_DIR);
  if (!dir || !*dir)
    return false;
  return unlink((std::string(dir) + "/" + name).c_str()) == 0;
}

void RemoveStateFile(const char* name)
{
  TakeStateFile(name);
}

bool MinimizeEnabled()
{
  const char* dir = std::getenv(ENV_STATE_DIR);
  const char* off = std::getenv("FLEX_KODI_HOME_MINIMIZE");
  return dir && *dir && !(off && std::strcmp(off, "0") == 0);
}

void WriteStateFile(const char* name, const std::string& content)
{
  const char* dir = std::getenv(ENV_STATE_DIR);
  if (!dir || !*dir)
    return;

  const std::string path = std::string(dir) + "/" + name;
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0)
    return;
  const ssize_t ignored = write(fd, content.data(), content.size());
  (void)ignored;
  close(fd);
}

XBMCKey DigitKey(unsigned int digit, uint16_t& unicode)
{
  unicode = static_cast<uint16_t>('0' + digit);
  return static_cast<XBMCKey>(XBMCK_0 + digit);
}

/*!
 * \brief Maps a Linux evdev key code to an XBMC key symbol.
 *
 * Only symbols that have an entry in Kodi's key table (XBMC_keytable.cpp) are used, so every
 * mapped key can be bound in a keymap. KEY_POWER and KEY_SLEEP are handled by the caller.
 *
 * \param code The EV_KEY code (KEY_*)
 * \param[out] unicode The character produced by the key, 0 for non-printing keys
 * \return The XBMC key symbol, XBMCK_UNKNOWN if the code is not mapped
 */
XBMCKey TranslateKeyCode(uint16_t code, uint16_t& unicode)
{
  unicode = 0;

  switch (code)
  {
    // Navigation
    case KEY_UP:
      return XBMCK_UP;
    case KEY_DOWN:
      return XBMCK_DOWN;
    case KEY_LEFT:
      return XBMCK_LEFT;
    case KEY_RIGHT:
      return XBMCK_RIGHT;
    case KEY_OK:
    case KEY_SELECT:
    case KEY_ENTER:
    case KEY_KPENTER:
      return XBMCK_RETURN;
    case KEY_BACK:
    case KEY_EXIT:
      return XBMCK_BROWSER_BACK;
    case KEY_BACKSPACE:
      return XBMCK_BACKSPACE;
    case KEY_ESC:
      return XBMCK_ESCAPE;
    case KEY_MENU:
    case KEY_OPTION:
#ifdef KEY_CONTEXT_MENU
    case KEY_CONTEXT_MENU:
#endif
      return XBMCK_MENU;
    case KEY_HOME:
    case KEY_HOMEPAGE:
      return XBMCK_HOMEPAGE;
    case KEY_INFO:
      // keyboard.xml has no <info> binding, but <i> is Info in all relevant windows. Unicode 0
      // keeps the key from typing an "i" into text fields.
      return XBMCK_i;
    case KEY_EPG:
      return XBMCK_EPG;
    case KEY_PAGEUP:
    case KEY_CHANNELUP:
      return XBMCK_PAGEUP;
    case KEY_PAGEDOWN:
    case KEY_CHANNELDOWN:
      return XBMCK_PAGEDOWN;
    case KEY_SETUP:
    case KEY_CONFIG:
      // keyboard.xml: <config>ActivateWindow(Settings)</config> (<settings> is not bound)
      return XBMCK_CONFIG;
    case KEY_FAVORITES:
      return XBMCK_FAVORITES;
    case KEY_SEARCH:
      return XBMCK_BROWSER_SEARCH;
    case KEY_TEXT:
      return XBMCK_TEXT;
    case KEY_ZOOM:
    case KEY_SCREEN: // also known as KEY_ASPECT_RATIO
      return XBMCK_ZOOM;

    // Colour keys
    case KEY_RED:
      return XBMCK_RED;
    case KEY_GREEN:
      return XBMCK_GREEN;
    case KEY_YELLOW:
      return XBMCK_YELLOW;
    case KEY_BLUE:
      return XBMCK_BLUE;

    // Media keys
    case KEY_PLAYPAUSE:
    case KEY_PLAY:
    case KEY_PLAYCD:
    case KEY_PAUSE:
    case KEY_PAUSECD:
      return XBMCK_MEDIA_PLAY_PAUSE;
    case KEY_STOP:
    case KEY_STOPCD:
      return XBMCK_MEDIA_STOP;
    case KEY_REWIND:
      return XBMCK_MEDIA_REWIND;
    case KEY_FASTFORWARD:
      return XBMCK_MEDIA_FASTFORWARD;
    case KEY_NEXTSONG:
    case KEY_NEXT:
      return XBMCK_MEDIA_NEXT_TRACK;
    case KEY_PREVIOUSSONG:
    case KEY_PREVIOUS:
      return XBMCK_MEDIA_PREV_TRACK;
    case KEY_RECORD:
      return XBMCK_RECORD;
    case KEY_EJECTCD:
    case KEY_EJECTCLOSECD:
    case KEY_SHUFFLE:
      // XBMCK_EJECT / XBMCK_SHUFFLE have no key table entry and could never be bound: report
      // these keys as unmapped instead
      return XBMCK_UNKNOWN;

    // Volume keys (only reach this point while the device is grabbed)
    case KEY_MUTE:
      return XBMCK_VOLUME_MUTE;
    case KEY_VOLUMEDOWN:
      return XBMCK_VOLUME_DOWN;
    case KEY_VOLUMEUP:
      return XBMCK_VOLUME_UP;

    // Digits (keyboard row, remote numeric keys and keypad all produce the plain digit)
    case KEY_0:
    case KEY_KP0:
#ifdef KEY_NUMERIC_0
    case KEY_NUMERIC_0:
#endif
      return DigitKey(0, unicode);
    case KEY_1:
    case KEY_KP1:
#ifdef KEY_NUMERIC_1
    case KEY_NUMERIC_1:
#endif
      return DigitKey(1, unicode);
    case KEY_2:
    case KEY_KP2:
#ifdef KEY_NUMERIC_2
    case KEY_NUMERIC_2:
#endif
      return DigitKey(2, unicode);
    case KEY_3:
    case KEY_KP3:
#ifdef KEY_NUMERIC_3
    case KEY_NUMERIC_3:
#endif
      return DigitKey(3, unicode);
    case KEY_4:
    case KEY_KP4:
#ifdef KEY_NUMERIC_4
    case KEY_NUMERIC_4:
#endif
      return DigitKey(4, unicode);
    case KEY_5:
    case KEY_KP5:
#ifdef KEY_NUMERIC_5
    case KEY_NUMERIC_5:
#endif
      return DigitKey(5, unicode);
    case KEY_6:
    case KEY_KP6:
#ifdef KEY_NUMERIC_6
    case KEY_NUMERIC_6:
#endif
      return DigitKey(6, unicode);
    case KEY_7:
    case KEY_KP7:
#ifdef KEY_NUMERIC_7
    case KEY_NUMERIC_7:
#endif
      return DigitKey(7, unicode);
    case KEY_8:
    case KEY_KP8:
#ifdef KEY_NUMERIC_8
    case KEY_NUMERIC_8:
#endif
      return DigitKey(8, unicode);
    case KEY_9:
    case KEY_KP9:
#ifdef KEY_NUMERIC_9
    case KEY_NUMERIC_9:
#endif
      return DigitKey(9, unicode);

    default:
      return XBMCK_UNKNOWN;
  }
}
} // namespace

CEvdevInputHandler::CEvdevInputHandler(std::string device)
  : CThread("EvdevInput"),
    m_device(ResolveDevice(std::move(device))),
    m_grab(IsGrabRequested()),
    m_repeatDelayMs(GetRepeatDelayMs()),
    m_tvKeysRequested(IsTvKeysRequested()),
    m_tvKeysKid(MakeTvKeysKid()),
    m_minimizeEnabled(MinimizeEnabled())
{
}

CEvdevInputHandler::~CEvdevInputHandler()
{
  StopThread(true);
  CloseDevice();
}

void CEvdevInputHandler::Start()
{
  if (IsRunning())
    return;

  CLog::Log(LOGINFO,
            "CEvdevInputHandler::{} - reading input from {} (exclusive grab {}, autorepeat "
            "guard {} ms)",
            __FUNCTION__, m_device, m_grab ? "requested" : "off", m_repeatDelayMs);
  Create();
}

void CEvdevInputHandler::Process()
{
  while (!m_bStop)
  {
    // registration with flex.js, acknowledgements and the volume hint (at most every 100 ms)
    TvKeysTick();
    // Home during a film: hide once the film had time to stop; minimized: the Kodi tile asks for
    // Kodi again (kodi.sh start -> state file "restore")
    if (m_minimizeAtMs >= 0 && SteadyNowMs() >= m_minimizeAtMs)
      FinishMinimize();
    CheckRestore();

    if (m_fd < 0 && !OpenDevice())
    {
      // Not available (yet): keep Kodi running without input and try again later
      CThread::Sleep(RETRY_INTERVAL);
      continue;
    }

    pollfd pfd{};
    pfd.fd = m_fd;
    pfd.events = POLLIN;

    const int ret = poll(&pfd, 1, POLL_TIMEOUT_MS);
    if (ret < 0)
    {
      if (errno == EINTR)
        continue;

      CLog::Log(LOGERROR, "CEvdevInputHandler::{} - poll on {} failed ({})", __FUNCTION__,
                m_device, std::strerror(errno));
      CloseDevice();
      CThread::Sleep(RETRY_INTERVAL);
      continue;
    }

    if (ret == 0)
      continue;

    if (pfd.revents & POLLIN)
    {
      if (!ReadEvents())
        CThread::Sleep(RETRY_INTERVAL);
    }
    else if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    {
      CLog::Log(LOGERROR, "CEvdevInputHandler::{} - error condition on {} (revents {:#x})",
                __FUNCTION__, m_device, static_cast<unsigned int>(pfd.revents));
      CloseDevice();
      CThread::Sleep(RETRY_INTERVAL);
    }
  }

  CloseDevice();
}

bool CEvdevInputHandler::OpenDevice()
{
  const int fd = open(m_device.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0)
  {
    const int err = errno;
    if (!m_openFailureLogged)
    {
      CLog::Log(LOGERROR,
                "CEvdevInputHandler::{} - failed to open {} ({}), no remote control input; "
                "retrying every {} s",
                __FUNCTION__, m_device, std::strerror(err),
                static_cast<long long>(RETRY_INTERVAL.count()));
      m_openFailureLogged = true;
    }
    return false;
  }

  m_openFailureLogged = false;

  std::array<char, 256> name{};
  if (ioctl(fd, EVIOCGNAME(name.size() - 1), name.data()) < 0)
    std::strncpy(name.data(), "unknown", name.size() - 1);

  if (m_grab && !m_minimized)
  {
    if (ioctl(fd, EVIOCGRAB, 1) == 0)
      m_grabbed = true;
    else
      CLog::Log(LOGWARNING, "CEvdevInputHandler::{} - exclusive grab of {} failed ({})",
                __FUNCTION__, m_device, std::strerror(errno));
  }

  m_fd = fd;
  m_dropUntilSync = false;
  m_hasScanCode = false;
  m_keyInFrame = false;

  CLog::Log(LOGINFO, "CEvdevInputHandler::{} - opened {} (\"{}\"), exclusive grab: {}",
            __FUNCTION__, m_device, name.data(), YesNo(m_grabbed));

  // The window exists (the window system starts input after creating it) and the remote is ours
  if (m_grabbed)
  {
    WriteStateFile("ready", "1\n");
    // volume to the TV/AVR through flex.js (FLEX_KODI_TVKEYS=1), only with the grab
    StartTvKeys();
  }

  // Event types and kernel autorepeat: scrolling and long presses depend on EV_REP
  std::array<unsigned long, (EV_CNT + BITS_PER_ULONG - 1) / BITS_PER_ULONG> evBits{};
  if (ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits.data()) >= 0)
  {
    unsigned int evMask = 0;
    for (unsigned int type = 0; type < EV_CNT && type < 32; ++type)
    {
      if (TestBit(type, evBits))
        evMask |= 1U << type;
    }
    CLog::Log(LOGINFO,
              "CEvdevInputHandler::{} - {} event types {:#x} (EV_KEY: {}, EV_MSC: {}, EV_REP: "
              "{})",
              __FUNCTION__, m_device, evMask, YesNo(TestBit(EV_KEY, evBits)),
              YesNo(TestBit(EV_MSC, evBits)), YesNo(TestBit(EV_REP, evBits)));
  }
  else
  {
    CLog::Log(LOGINFO, "CEvdevInputHandler::{} - reading the event types of {} failed ({})",
              __FUNCTION__, m_device, std::strerror(errno));
  }

  std::array<unsigned int, 2> rep{};
  if (ioctl(fd, EVIOCGREP, rep.data()) == 0)
    CLog::Log(LOGINFO,
              "CEvdevInputHandler::{} - {} kernel autorepeat: delay {} ms, period {} ms",
              __FUNCTION__, m_device, rep[0], rep[1]);
  else
    CLog::Log(LOGINFO,
              "CEvdevInputHandler::{} - reading the kernel autorepeat settings of {} failed "
              "(EVIOCGREP: {})",
              __FUNCTION__, m_device, std::strerror(errno));

  return true;
}

void CEvdevInputHandler::CloseDevice()
{
  if (m_fd < 0)
    return;

  // The releases of keys still held can no longer be read
  ReleaseAllKeys();

  if (m_grabbed)
  {
    ioctl(m_fd, EVIOCGRAB, 0);
    m_grabbed = false;
  }

  close(m_fd);
  m_fd = -1;
  m_dropUntilSync = false;
  m_hasScanCode = false;
  m_keyInFrame = false;
}

bool CEvdevInputHandler::ReadEvents()
{
  std::array<input_event, 64> events;

  while (!m_bStop)
  {
    const ssize_t bytes = read(m_fd, events.data(), sizeof(events));
    if (bytes < 0)
    {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return true; // drained

      CLog::Log(LOGERROR, "CEvdevInputHandler::{} - read from {} failed ({}), reopening",
                __FUNCTION__, m_device, std::strerror(errno));
      CloseDevice();
      return false;
    }

    if (bytes == 0)
    {
      CLog::Log(LOGERROR, "CEvdevInputHandler::{} - {} was closed, reopening", __FUNCTION__,
                m_device);
      CloseDevice();
      return false;
    }

    // evdev always returns whole events
    const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
    for (size_t i = 0; i < count; ++i)
      HandleEvent(events[i]);

    if (static_cast<size_t>(bytes) < sizeof(events))
      return true; // drained
  }

  return true;
}

void CEvdevInputHandler::HandleEvent(const input_event& event)
{
  switch (event.type)
  {
    case EV_SYN:
      if (event.code == SYN_DROPPED)
      {
        // The kernel buffer overflowed: ignore everything up to and including the next
        // SYN_REPORT. Releases may have been lost, so release every key Kodi thinks is held.
        CLog::Log(LOGINFO, "CEvdevInputHandler::{} - events dropped by the kernel on {}",
                  __FUNCTION__, m_device);
        m_dropUntilSync = true;
        m_hasScanCode = false;
        m_keyInFrame = false;
        ReleaseAllKeys();
      }
      else if (event.code == SYN_REPORT)
      {
        // A scan code without a key event: the button is not in the kernel keymap (rc-core
        // sends no EV_KEY for KEY_RESERVED). Report every such scan code once.
        if (!m_dropUntilSync && m_hasScanCode && !m_keyInFrame &&
            m_knownScanCodes.insert(m_scanCode).second)
          CLog::Log(LOGINFO,
                    "CEvdevInputHandler::{} - scan code {:#x} on {} arrived without a key event "
                    "(not in the kernel keymap?) - ignored",
                    __FUNCTION__, m_scanCode, m_device);

        m_dropUntilSync = false;
        m_hasScanCode = false;
        m_keyInFrame = false;
      }
      break;

    case EV_MSC:
      // IR drivers usually report the raw scan code right before the key event
      if (!m_dropUntilSync && event.code == MSC_SCAN)
      {
        m_scanCode = static_cast<uint32_t>(event.value);
        m_hasScanCode = true;
      }
      break;

    case EV_KEY:
      if (!m_dropUntilSync)
        HandleKey(event.code, event.value, EventTimeMs(event));
      break;

    default:
      break;
  }
}

void CEvdevInputHandler::HandleKey(uint16_t code, int32_t value, int64_t timeMs)
{
  if (value != KEY_VALUE_RELEASE && value != KEY_VALUE_PRESS && value != KEY_VALUE_REPEAT)
    return;

  // minimized (or about to be): the TV launcher has the remote (no grab); Kodi ignores the keys
  if (m_minimized || m_minimizeAtMs >= 0)
    return;

  // The scan code of this frame belongs to a key: its scan-code-only repeat frames (rc-core
  // repeat) must not be reported as unmapped buttons
  m_keyInFrame = true;
  if (m_hasScanCode)
    m_knownScanCodes.insert(m_scanCode);

  uint16_t unicode = 0;
  XBMCKey sym = XBMCK_UNKNOWN;
  KeyAction action = KeyAction::FORWARD;
  if (IsTvKey(code) && !m_grabbed)
    action = KeyAction::TV;
  else if (m_grabbed && IsVolumeKey(code) && m_tvKeys == TvKeys::ON)
    action = KeyAction::TV_FORWARD;
  else if (IsQuitKey(code))
    action = KeyAction::QUIT;
  else if (m_minimizeEnabled && m_grabbed && (code == KEY_HOME || code == KEY_HOMEPAGE))
    action = KeyAction::MINIMIZE;
  else
  {
    sym = TranslateKeyCode(code, unicode);
    if (sym == XBMCK_UNKNOWN)
      action = KeyAction::UNMAPPED;
  }

  if (value == KEY_VALUE_PRESS)
  {
    LogKeyOnce(code, action, sym);
    // user activity for the TV: the launcher sees no keys while Kodi holds the grab, so it
    // would otherwise start its auto-standby countdown during normal Kodi use
    WriteStateFile("lastkey", std::to_string(static_cast<long long>(std::time(nullptr))) + "\n");
  }

  switch (action)
  {
    case KeyAction::UNMAPPED:
    case KeyAction::TV:
      return;
    case KeyAction::TV_FORWARD:
      ForwardTvKey(code, value, timeMs);
      return;
    case KeyAction::MINIMIZE:
      if (value == KEY_VALUE_PRESS)
        Minimize();
      return;
    case KeyAction::QUIT:
      if (value == KEY_VALUE_PRESS)
      {
        m_quitKeyDownMs = timeMs;
        RequestQuit(code);
      }
      else if (value == KEY_VALUE_RELEASE)
        m_quitKeyDownMs = -1;
      else if (m_quitKeyDownMs >= 0 && timeMs - m_quitKeyDownMs >= FORCE_QUIT_HOLD_MS)
        ForceQuit(code);
      return;
    case KeyAction::FORWARD:
      break;
  }

  if (value == KEY_VALUE_RELEASE)
  {
    // Only release keys Kodi has seen going down (not keys held while the device was opened, or
    // keys already released after SYN_DROPPED)
    const auto it = m_pressedKeys.find(code);
    if (it == m_pressedKeys.end())
      return;

    const PressedKey key = it->second;
    m_pressedKeys.erase(it);
    SendKey(false, code, key.sym, key.unicode);
    return;
  }

  if (value == KEY_VALUE_PRESS)
  {
    CLog::Log(LOGDEBUG, "CEvdevInputHandler::{} - key code {} pressed -> XBMC key {:#x}",
              __FUNCTION__, code, static_cast<unsigned int>(sym));
    m_pressedKeys[code] = PressedKey{sym, unicode, timeMs};
    SendKey(true, code, sym, unicode);
    return;
  }

  // Autorepeat (value 2) is forwarded as another key down, like the keyboard repeat of libinput
  if (IsToggleKey(code))
    return;

  const auto it = m_pressedKeys.find(code);
  if (it == m_pressedKeys.end())
  {
    // The press was lost (SYN_DROPPED) or happened before the device was opened
    m_pressedKeys[code] = PressedKey{sym, unicode, timeMs};
  }
  else if (m_repeatDelayMs > 0)
  {
    const int64_t sincePress = timeMs - it->second.pressTimeMs;
    if (sincePress >= 0 && sincePress < m_repeatDelayMs)
      return;
  }

  SendKey(true, code, sym, unicode);
}

void CEvdevInputHandler::LogKeyOnce(uint16_t code, KeyAction action, XBMCKey sym)
{
  const int64_t scanKey = m_hasScanCode ? static_cast<int64_t>(m_scanCode) : NO_SCAN_CODE;
  if (!m_loggedKeys.insert(std::make_pair(code, scanKey)).second)
    return;

  const std::string scan = m_hasScanCode ? StringUtils::Format("{:#x}", m_scanCode) : "n/a";

  switch (action)
  {
    case KeyAction::UNMAPPED:
      CLog::Log(LOGINFO,
                "CEvdevInputHandler::{} - unknown key code {} ({:#x}), scan code {}, on {} - "
                "ignored",
                __FUNCTION__, code, code, scan, m_device);
      break;
    case KeyAction::TV:
      CLog::Log(LOGINFO,
                "CEvdevInputHandler::{} - key code {} ({:#x}), scan code {}, on {} - left to "
                "the TV (device not grabbed)",
                __FUNCTION__, code, code, scan, m_device);
      break;
    case KeyAction::TV_FORWARD:
      CLog::Log(LOGINFO,
                "CEvdevInputHandler::{} - key code {} ({:#x}), scan code {}, on {} -> TV/AVR "
                "volume via flex.js (tvkeys)",
                __FUNCTION__, code, code, scan, m_device);
      break;
    case KeyAction::QUIT:
      CLog::Log(LOGINFO,
                "CEvdevInputHandler::{} - key code {} ({:#x}), scan code {}, on {} -> quit Kodi",
                __FUNCTION__, code, code, scan, m_device);
      break;
    case KeyAction::MINIMIZE:
      CLog::Log(LOGINFO,
                "CEvdevInputHandler::{} - key code {} ({:#x}), scan code {}, on {} -> minimize Kodi",
                __FUNCTION__, code, code, scan, m_device);
      break;
    case KeyAction::FORWARD:
      CLog::Log(LOGINFO,
                "CEvdevInputHandler::{} - key code {} ({:#x}), scan code {}, on {} -> XBMC key "
                "{:#x}",
                __FUNCTION__, code, code, scan, m_device, static_cast<unsigned int>(sym));
      break;
  }
}

void CEvdevInputHandler::SendKey(bool down, uint16_t code, XBMCKey sym, uint16_t unicode)
{
  XBMC_Event event{};
  event.type = down ? XBMC_KEYDOWN : XBMC_KEYUP;
  event.key.keysym.scancode = code;
  event.key.keysym.sym = sym;
  event.key.keysym.mod = XBMCKMOD_NONE;
  event.key.keysym.unicode = unicode;

  std::shared_ptr<CAppInboundProtocol> appPort = CServiceBroker::GetAppPort();
  if (appPort)
    appPort->OnEvent(event);
}

void CEvdevInputHandler::ReleaseAllKeys()
{
  if (m_pressedKeys.empty())
    return;

  CLog::Log(LOGDEBUG, "CEvdevInputHandler::{} - releasing {} held key(s) of {}", __FUNCTION__,
            m_pressedKeys.size(), m_device);

  for (const auto& [code, key] : m_pressedKeys)
    SendKey(false, code, key.sym, key.unicode);

  m_pressedKeys.clear();
}

void CEvdevInputHandler::ForceQuit(uint16_t code)
{
  // Kodi did not finish quitting while the key was held (e.g. the main thread hangs). This
  // reader thread still runs, so end the process right here: the kernel then releases the
  // exclusive grab and the TV gets its remote back. No CLog (it may be blocked by the hung
  // thread), no destructors (they may hang too).
  std::fprintf(stderr,
               "CEvdevInputHandler: key code %u held for %d ms without Kodi quitting, ending "
               "the process\n",
               static_cast<unsigned int>(code), FORCE_QUIT_HOLD_MS);
  std::fflush(stderr);
  _exit(3);
}

void CEvdevInputHandler::RequestQuit(uint16_t code)
{
  if (m_quitRequested)
    return;

  std::shared_ptr<CAppInboundProtocol> appPort = CServiceBroker::GetAppPort();
  if (!appPort)
    return;

  CLog::Log(LOGINFO,
            "CEvdevInputHandler::{} - key code {} pressed on {}, quitting Kodi to give the "
            "remote back to the TV",
            __FUNCTION__, code, m_device);

  // CApplication::HandlePortEvents() turns XBMC_QUIT into TMSG_QUIT -> Stop(EXITCODE_QUIT); the
  // window system then destroys this handler, which releases the grab
  XBMC_Event event{};
  event.type = XBMC_QUIT;
  appPort->OnEvent(event);
  m_quitRequested = true;
}

void CEvdevInputHandler::ApplyVisible(void* visible) // app thread (TMSG_CALLBACK)
{
  const bool show = visible != nullptr;
  // nothing drawn while hidden: the GUI costs no CPU, the render system sleeps 40 ms per frame
  const auto power = CServiceBroker::GetAppComponents().GetComponent<CApplicationPowerHandling>();
  if (power)
    power->SetRenderGUI(show);
  CWinSystemBase* winSystem = CServiceBroker::GetWinSystem();
  if (winSystem)
  {
    if (show)
      winSystem->Show(true);
    else
      winSystem->Hide();
  }
  auto gui = CServiceBroker::GetGUI();
  if (show && gui)
    gui->GetWindowManager().MarkDirty();
}

void CEvdevInputHandler::Minimize()
{
  // FLEX-TV (18.11.2026, owner: "Home in Kodi minimizes Kodi, other apps then end it"). Kodi keeps
  // running hidden: the window becomes transparent (CWinSystemDirectFB::Hide), the GUI is not
  // drawn, the remote goes back to the TV launcher (no grab). A running film stops first (Kodi
  // keeps its resume point) so the TV player is free. kodi-run.sh reports the state file
  // "minimized" to flex.js in flexkodi/running; the Kodi tile then asks for Kodi again (kodi.sh
  // start writes "restore", CheckRestore), any other app ends Kodi as before.
  //
  // A film is stopped first and Kodi stays opaque and keeps the remote for MINIMIZE_AFTER_STOP_MS:
  // on 18.11.2026 hiding Kodi at the same moment as the TV player's window reset and stop left
  // Mm_Stop hanging for about 100 s (the TV stayed on DMP, the stretched film showed through, the
  // PS5 had no picture). A normal stop with Kodi visible was fine.
  if (m_minimized || m_minimizeAtMs >= 0 || m_fd < 0)
    return;
  ReleaseAllKeys();
  m_quitKeyDownMs = -1;
  bool playing = false;
  const auto player = CServiceBroker::GetAppComponents().GetComponent<CApplicationPlayer>();
  if (player)
    playing = player->IsPlaying();
  const auto messenger = CServiceBroker::GetAppMessenger();
  if (messenger)
    messenger->PostMsg(TMSG_MEDIA_STOP);
  CLog::Log(LOGINFO, "CEvdevInputHandler::{} - Home: Kodi steps aside{}", __FUNCTION__,
            playing ? " after the film has stopped" : ", the remote goes back to the TV launcher");
  m_minimizeAtMs = SteadyNowMs() + (playing ? MINIMIZE_AFTER_STOP_MS : 0);
  if (!playing)
    FinishMinimize();
}

void CEvdevInputHandler::FinishMinimize()
{
  m_minimizeAtMs = -1;
  if (m_minimized || m_fd < 0)
    return;
  const auto messenger = CServiceBroker::GetAppMessenger();
  if (messenger)
  {
    static KODI::MESSAGING::ThreadMessageCallback hide{&CEvdevInputHandler::ApplyVisible, nullptr};
    messenger->PostMsg(TMSG_CALLBACK, -1, -1, static_cast<void*>(&hide));
  }
  RemoveStateFile("restore"); // an old request must not bring Kodi back at once
  WriteStateFile("minimized", "1\n");
  if (m_grabbed)
  {
    ioctl(m_fd, EVIOCGRAB, 0);
    m_grabbed = false;
  }
  m_minimized = true;
  m_nextRestoreCheckMs = SteadyNowMs() + 300;
}

void CEvdevInputHandler::CheckRestore()
{
  if (!m_minimized)
    return;
  const int64_t now = SteadyNowMs();
  if (now < m_nextRestoreCheckMs)
    return;
  m_nextRestoreCheckMs = now + 300;
  if (!TakeStateFile("restore"))
    return;
  if (m_fd >= 0 && m_grab)
  {
    if (ioctl(m_fd, EVIOCGRAB, 1) == 0)
      m_grabbed = true;
    else
      CLog::Log(LOGWARNING, "CEvdevInputHandler::{} - grab of {} failed ({}), Kodi shows anyway",
                __FUNCTION__, m_device, std::strerror(errno));
  }
  m_minimized = false;
  m_dropUntilSync = false;
  RemoveStateFile("minimized");
  WriteStateFile("lastkey", std::to_string(static_cast<long long>(std::time(nullptr))) + "\n");
  const auto messenger = CServiceBroker::GetAppMessenger();
  if (messenger)
  {
    static KODI::MESSAGING::ThreadMessageCallback show{&CEvdevInputHandler::ApplyVisible, reinterpret_cast<void*>(1)};
    messenger->PostMsg(TMSG_CALLBACK, -1, -1, static_cast<void*>(&show));
  }
  CLog::Log(LOGINFO, "CEvdevInputHandler::{} - Kodi tile: Kodi is back (exclusive grab: {})",
            __FUNCTION__, YesNo(m_grabbed));
}

void CEvdevInputHandler::StartTvKeys()
{
  if (!m_tvKeysRequested || m_tvKeys != TvKeys::OFF)
    return;

  // reads the run key once (TVPLAYER::CTVBridgeIO); without it the volume stays with Kodi
  if (!TVPLAYER::CTVBridgeIO::Get().IsUsable())
  {
    CLog::Log(LOGINFO, "CEvdevInputHandler::{} - {}=1, but no bridge to flex.js: Kodi's own volume",
              __FUNCTION__, ENV_TVKEYS);
    m_tvKeys = TvKeys::FAILED;
    return;
  }

  // a new kid only makes flex.js take over the counters: register with a tvkeys without presses
  CLog::Log(LOGINFO, "CEvdevInputHandler::{} - registering TV volume with flex.js", __FUNCTION__);
  m_tvKeys = TvKeys::REGISTERING;
  m_registerStartMs = SteadyNowMs();
  m_nextRegisterMs = m_registerStartMs;
}

void CEvdevInputHandler::ForwardTvKey(uint16_t code, int32_t value, int64_t timeMs)
{
  if (value == KEY_VALUE_RELEASE)
  {
    // a key that went down while Kodi's own volume was still in use
    const auto it = m_pressedKeys.find(code);
    if (it != m_pressedKeys.end())
    {
      const PressedKey key = it->second;
      m_pressedKeys.erase(it);
      SendKey(false, code, key.sym, key.unicode);
    }
    return;
  }
  if (value == KEY_VALUE_REPEAT)
  {
    // mute toggles: never repeat it; volume: thin out the autorepeat to ~7 steps/s
    if (code == KEY_MUTE || (m_lastTvStepMs >= 0 && timeMs - m_lastTvStepMs < TVKEY_STEP_MS))
      return;
  }
  m_lastTvStepMs = timeMs;

  if (code == KEY_VOLUMEUP)
    ++m_volUp;
  else if (code == KEY_VOLUMEDOWN)
    ++m_volDown;
  else
    ++m_muteCount;
  ++m_tvKeysSeq;
  m_lastTvKeyCode = code;
  m_lastTvKeyMs = SteadyNowMs();
  WriteTvKeys();
}

void CEvdevInputHandler::WriteTvKeys()
{
  if (!TVPLAYER::WriteTvKeys(m_tvKeysKid, m_tvKeysSeq, m_volUp, m_volDown, m_muteCount))
  {
    TvKeysFailed("tvkeys cannot be written");
    return;
  }
  if (!m_ackPending)
  {
    m_ackPending = true;
    m_ackWaitSeq = m_tvKeysSeq;
    m_ackDeadlineMs = SteadyNowMs() + TVKEYS_ACK_MS;
  }
}

void CEvdevInputHandler::TvKeysTick()
{
  if (m_tvKeys != TvKeys::REGISTERING && m_tvKeys != TvKeys::ON)
    return;

  const int64_t now = SteadyNowMs();
  TVPLAYER::TVAck ack;

  if (m_tvKeys == TvKeys::REGISTERING)
  {
    if (TVPLAYER::ReadTvAck(m_tvKeysKid, ack))
    {
      CLog::Log(LOGINFO, "CEvdevInputHandler::{} - TV volume via flex.js ({})", __FUNCTION__,
                ack.arc ? "AVR over HDMI ARC" : "TV loudspeakers");
      m_tvKeys = TvKeys::ON;
      ResetKodiVolume();
      return;
    }
    if (now - m_registerStartMs > TVKEYS_REGISTER_MS)
    {
      TvKeysFailed("flex.js did not answer the registration");
      return;
    }
    if (now >= m_nextRegisterMs)
    {
      m_nextRegisterMs = now + TVKEYS_REGISTER_REPEAT_MS;
      if (!TVPLAYER::WriteTvKeys(m_tvKeysKid, 0, 0, 0, 0))
        TvKeysFailed("tvkeys cannot be written");
    }
    return;
  }

  // ON: tvack is only read while something is outstanding
  if (!m_ackPending && !m_hintPending && now - m_lastTvKeyMs >= TVKEYS_HINT_WINDOW_MS)
    return;

  if (TVPLAYER::ReadTvAck(m_tvKeysKid, ack))
  {
    if (m_ackPending)
    {
      if (ack.seq >= m_tvKeysSeq)
        m_ackPending = false;
      else if (ack.seq >= m_ackWaitSeq)
      {
        // older writes acknowledged: the 1 s now counts for the next one
        m_ackWaitSeq = static_cast<uint32_t>(ack.seq + 1);
        m_ackDeadlineMs = now + TVKEYS_ACK_MS;
      }
    }
    // the value after the last press (flex.js acknowledges again once every step is applied)
    if (ack.seq == m_tvKeysSeq &&
        (ack.seq != m_shownAckSeq || ack.vol != m_shownVol || ack.mute != m_shownMute))
    {
      m_shownAckSeq = ack.seq;
      m_shownVol = ack.vol;
      m_shownMute = ack.mute;
      // the launcher's volume bar lies below Kodi's window: Kodi's own bar with the device's value
      // (FlexVolumeBar.h), the receiver's read from its status page; a hint if there is none
      m_hintReceiver = ack.arc;
      m_hintPercent = -1;
      if (ack.arc)
        m_hintText = m_lastTvKeyCode == KEY_VOLUMEUP     ? "AVR lauter"
                     : m_lastTvKeyCode == KEY_VOLUMEDOWN ? "AVR leiser"
                                                         : "AVR stumm";
      else if (ack.mute)
      {
        m_hintText = "Stumm";
        m_hintPercent = 0;
        m_hintNumber = "Stumm";
      }
      else if (ack.vol >= 0)
      {
        m_hintText = StringUtils::Format("TV-Lautstärke {}", ack.vol);
        m_hintPercent = ack.vol;
        m_hintNumber = std::to_string(ack.vol);
      }
      else
        m_hintText = "TV-Lautstärke";
      m_hintPending = true;
    }
  }

  if (m_ackPending && now > m_ackDeadlineMs)
  {
    TvKeysFailed("no acknowledgement from flex.js within 1 s");
    return;
  }

  if (m_hintPending && now - m_lastTvKeyMs >= TVKEYS_HINT_QUIET_MS)
  {
    m_hintPending = false;
    if (m_hintReceiver && FLEXVOL::ShowReceiver(m_hintText))
      return; // the job shows the bar, or the hint if the receiver does not answer
    if (m_hintPercent >= 0)
      FLEXVOL::Show(m_hintPercent, m_hintNumber);
    else
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Lautstärke", m_hintText,
                                            TVKEYS_HINT_DISPLAY_MS, false);
  }
}

void CEvdevInputHandler::TvKeysFailed(const char* reason)
{
  // the counted presses are lost; from now on XBMCK_VOLUME_* as without tvkeys
  CLog::Log(LOGWARNING,
            "CEvdevInputHandler: TV volume off for this run ({}), the volume keys control Kodi again",
            reason);
  m_tvKeys = TvKeys::FAILED;
  m_ackPending = false;
  m_hintPending = false;
  FLEXVOL::Clear(); // Kodi's volume bar shows Kodi's own volume again
  CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Info, "Lautstärke",
                                        "TV-Lautstärke nicht erreichbar", 5000, false);
}

void CEvdevInputHandler::ResetKodiVolume()
{
  // The TV/AVR is the only volume control now: Kodi's software volume must not attenuate its own
  // PCM output (GUI sounds, music, VideoPlayer). Posted from this thread, applied by the main
  // loop (Application.cpp:1776-1786, the same messages the UPnP renderer uses).
  if (m_kodiVolumeReset)
    return;
  const auto messenger = CServiceBroker::GetAppMessenger();
  if (!messenger)
    return;
  messenger->PostMsg(TMSG_SET_VOLUME, static_cast<int64_t>(100));
  messenger->PostMsg(TMSG_SET_MUTE, static_cast<int64_t>(0));
  m_kodiVolumeReset = true;
}
