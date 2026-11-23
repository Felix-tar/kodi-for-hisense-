/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "input/keyboard/XBMC_keysym.h"
#include "threads/Thread.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

struct input_event;

namespace KODI
{
namespace WINDOWING
{
namespace DIRECTFB
{

/*!
 * \brief Reads key events from a Linux evdev device (e.g. the IR receiver of the TV) in its own
 *        thread and injects them into Kodi as XBMC_KEYDOWN / XBMC_KEYUP events.
 *
 * Kernel autorepeat events are forwarded as additional XBMC_KEYDOWN events, so long presses work
 * like on a keyboard (except for toggle keys like mute). The environment variable
 * FLEX_KODI_REPEAT_DELAY_MS (default 0 = off) drops autorepeat events that arrive less than that
 * many milliseconds after the press of the same key.
 *
 * Every (key code, scan code) pair is logged once at LOGINFO the first time it is pressed, and
 * so is every IR scan code that arrives without a key event, so the buttons of the remote can be
 * learned from the log. Opening the device logs its event types and autorepeat settings.
 *
 * Exclusive access (EVIOCGRAB) is only requested if the environment variable FLEX_KODI_GRAB is
 * set to "1". While the device is grabbed, the power and sleep keys make Kodi quit (so the TV
 * gets its remote back) and the volume keys control Kodi's volume. Without the grab these keys
 * are left to the TV and ignored by Kodi.
 *
 * TV volume (FLEX_KODI_TVKEYS=1 and a usable flex.js bridge, TVPLAYER::CTVBridgeIO): while the
 * device is grabbed, volume up/down/mute are counted in the signed bridge file "tvkeys" instead,
 * and the launcher mod flex.js passes them on to the TV or the AVR (HDMI ARC) the way the
 * launcher does. Kodi registers with a tvkeys without key presses (repeated every second, at
 * most 10 s) until flex.js acknowledges it in "tvack"; afterwards every write must be
 * acknowledged within 1 s. Without registration or acknowledgement the keys go back to Kodi's
 * own volume for the rest of this run. Kodi's own volume is set to 100 % once registered, and a
 * short hint shows the TV volume (the launcher's volume bar lies below Kodi's window).
 *
 * Keys still held when events are lost (SYN_DROPPED) or the device is closed are released with
 * an XBMC_KEYUP. If the device cannot be opened, the failure is logged and the open is retried
 * periodically; Kodi keeps running without remote input in the meantime.
 */
class CEvdevInputHandler : private CThread
{
public:
  /*!
   * \param device Path of the evdev device, e.g. "/dev/input/event0". If empty, the value of the
   *               environment variable FLEX_KODI_INPUT is used, or "/dev/input/event0".
   */
  explicit CEvdevInputHandler(std::string device);
  ~CEvdevInputHandler() override;

  /*!
   * \brief Starts the reader thread. Calling it again while the thread runs has no effect.
   */
  void Start();

private:
  enum class KeyAction
  {
    UNMAPPED, // no XBMC key for this code: ignored
    TV, // handled by the TV because the device is not grabbed: ignored
    TV_FORWARD, // volume/mute while grabbed: counted in tvkeys for flex.js (TV/AVR volume)
    QUIT, // makes Kodi quit
    MINIMIZE, // Home with the TV launcher integration: Kodi steps aside (Minimize)
    FORWARD, // injected as XBMC key event
  };

  enum class TvKeys
  {
    OFF, // FLEX_KODI_TVKEYS unset, no bridge or not grabbed (yet): Kodi's own volume
    REGISTERING, // waiting for the first tvack of this run
    ON, // volume keys go to flex.js
    FAILED, // no acknowledgement: Kodi's own volume for the rest of this run
  };

  struct PressedKey
  {
    XBMCKey sym;
    uint16_t unicode;
    int64_t pressTimeMs;
  };

  void Process() override;

  bool OpenDevice();
  void CloseDevice();
  bool ReadEvents();
  void HandleEvent(const input_event& event);
  void HandleKey(uint16_t code, int32_t value, int64_t timeMs);
  void LogKeyOnce(uint16_t code, KeyAction action, XBMCKey sym);
  void SendKey(bool down, uint16_t code, XBMCKey sym, uint16_t unicode);
  void ReleaseAllKeys();
  void RequestQuit(uint16_t code);
  [[noreturn]] void ForceQuit(uint16_t code);
  void StartTvKeys();
  void ForwardTvKey(uint16_t code, int32_t value, int64_t timeMs);
  void WriteTvKeys();
  void TvKeysTick();
  void TvKeysFailed(const char* reason);
  void ResetKodiVolume();
  // FLEX-TV (18.11.2026): Home hides Kodi and gives the remote back; the Kodi tile brings it back
  // (kodi.sh start writes the state file "restore"). See Minimize() in the .cpp.
  void Minimize();
  void FinishMinimize();
  void CheckRestore();
  static void ApplyVisible(void* visible); // app thread

  const std::string m_device;
  const bool m_grab;
  const int m_repeatDelayMs;
  // FLEX_KODI_TVKEYS=1
  const bool m_tvKeysRequested;
  // id of this Kodi run in tvkeys/tvack ("<time hex>-<pid hex>")
  const std::string m_tvKeysKid;

  // Only accessed by the reader thread (and by the destructor after the thread was stopped)
  int m_fd{-1};
  bool m_grabbed{false};
  bool m_openFailureLogged{false};
  bool m_quitRequested{false};
  // Home minimizes (FLEX_KODI_STATE_DIR set, FLEX_KODI_HOME_MINIMIZE not 0)
  const bool m_minimizeEnabled;
  bool m_minimized{false};
  // Home during a film: the film stops first, Kodi hides at this time (steady ms), -1 = nothing pending
  int64_t m_minimizeAtMs{-1};
  int64_t m_nextRestoreCheckMs{0};
  // kernel time (ms) of the current Power/Sleep press, -1 = not held
  int64_t m_quitKeyDownMs{-1};

  // State of the current event frame (up to the next SYN_REPORT)
  bool m_dropUntilSync{false};
  bool m_hasScanCode{false};
  bool m_keyInFrame{false};
  uint32_t m_scanCode{0};

  // Keys for which an XBMC_KEYDOWN was sent and no XBMC_KEYUP yet, by key code
  std::map<uint16_t, PressedKey> m_pressedKeys;

  // (key code, scan code or -1) pairs already logged
  std::set<std::pair<uint16_t, int64_t>> m_loggedKeys;
  // Scan codes seen with a key event, or already reported without one
  std::set<uint32_t> m_knownScanCodes;

  // tvkeys (only accessed by the reader thread); times are steady clock ms
  TvKeys m_tvKeys{TvKeys::OFF};
  uint32_t m_tvKeysSeq{0};
  uint32_t m_volUp{0};
  uint32_t m_volDown{0};
  uint32_t m_muteCount{0};
  int64_t m_lastTvStepMs{-1}; // kernel time of the last forwarded step (autorepeat thinning)
  int64_t m_registerStartMs{0};
  int64_t m_nextRegisterMs{0};
  bool m_ackPending{false};
  uint32_t m_ackWaitSeq{0}; // oldest unacknowledged tvkeys.seq
  int64_t m_ackDeadlineMs{0};
  uint16_t m_lastTvKeyCode{0};
  int64_t m_lastTvKeyMs{0}; // steady clock, for the hint
  int64_t m_shownAckSeq{-1};
  int m_shownVol{-2};
  bool m_shownMute{false};
  bool m_hintPending{false};
  std::string m_hintText;
  bool m_hintReceiver{false}; // over ARC: the receiver's value from its status page (FlexVolumeBar.h)
  int m_hintPercent{-1}; // TV loudspeakers: the TV's value for Kodi's volume bar, -1 none
  std::string m_hintNumber;
  bool m_kodiVolumeReset{false};
};

} // namespace DIRECTFB
} // namespace WINDOWING
} // namespace KODI
