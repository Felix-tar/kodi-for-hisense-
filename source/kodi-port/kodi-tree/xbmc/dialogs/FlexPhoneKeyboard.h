/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <chrono>
#include <mutex>
#include <string>

struct MHD_Daemon;

/*!
 * \brief Type on the phone into Kodi's on-screen keyboard (FLEX-TV, 17./18.11.2026).
 *
 * While Kodi runs, a small web server listens on port 8781 (IPv4, home network only). The
 * keyboard dialog (CGUIDialogKeyboardGeneric, not for hidden input such as passwords) shows a QR
 * code with http://<TV>:8781/k/<token>. The page there stays usable for the whole Kodi run and
 * across Kodi restarts:
 * - a keyboard dialog is open: the text goes into it as it is typed, the same way speech
 *   recognition and JSON-RPC Input.SendText do (GUI_MSG_SET_TEXT through the application
 *   messenger); "Fertig" also confirms the dialog;
 * - no keyboard dialog is open: "Suchen" opens Kodi's video library search on the TV and puts the
 *   typed text into it, so searching again needs neither the remote nor a new scan.
 *
 * The token is kept in userdata/flexkbd.token (mode 0600); deleting that file and restarting Kodi
 * makes a new one (old phone pages then get 404). Off switch: the file /APPS/kodi-phonekbd.off (or
 * FLEX_KODI_PHONEKBD=0).
 */
class CFlexPhoneKeyboard
{
public:
  static CFlexPhoneKeyboard& GetInstance();

  //! Kodi starts (the keyboard dialog is created): server on
  void Startup();
  //! Kodi ends (the keyboard dialog is destroyed, before the application messenger): server off
  void Shutdown();

  /*!
   * \brief A keyboard dialog opens.
   * \return path of the QR code image for the skin, empty if there is none (off, no network,
   *         port taken)
   */
  std::string DialogOpened(int dialogId, const std::string& heading, const std::string& text);
  //! the dialog closes; the phone page stays connected
  void DialogClosed();

private:
  CFlexPhoneKeyboard() = default;

  friend struct CFlexPhoneKeyboardServer; // the libmicrohttpd callbacks (FlexPhoneKeyboard.cpp)

  bool EnsureServer();
  std::string EnsureImage(const std::string& url);
  int Answer(struct MHD_Connection* connection,
             const std::string& path,
             bool post,
             const std::string& body);
  void SendText(const std::string& text, bool done);
  void StartSearch(const std::string& text);

  // GUI thread only (Startup, Shutdown, DialogOpened)
  std::string m_url; // in the current QR code image
  std::string m_imageFile;
  int m_imageCount = 0;

  std::mutex m_lock; // the fields below; never held while starting or stopping the server
  MHD_Daemon* m_daemon = nullptr;
  bool m_closing = false;
  std::string m_token;
  std::string m_heading;
  std::string m_text;
  int m_dialogId = 0; // 0: no keyboard dialog open
  unsigned int m_session = 0; // counts opened dialogs; the page takes the text of a new one
  std::string m_pendingText; // "Suchen" on the phone: goes into the next dialog and confirms it
  std::chrono::steady_clock::time_point m_pendingUntil;
};
