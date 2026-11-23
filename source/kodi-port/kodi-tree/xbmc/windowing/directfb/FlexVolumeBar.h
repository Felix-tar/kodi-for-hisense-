/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string>

/*!
 * \brief Kodi's volume bar for a volume that is not Kodi's own (FLEX-TV, 18.11.2026).
 *
 * The volume keys change the TV or, over HDMI ARC, the AV receiver (EvdevInputHandler, tvkeys);
 * Kodi's own volume stays at 100 %. This shows Kodi's volume bar with that device's value: the
 * home window properties flexvol.pct (0..100, the ring) and flexvol.text (the number) are read by
 * the skin's DialogVolumeBar.xml, then the bar opens like for a volume action (TMSG_VOLUME_SHOW).
 *
 * The receiver's value comes from its HTTP status page (Denon/Marantz
 * goform/formMainZone_MainZoneXmlStatusLite.xml: MasterVolume in dB, Mute), the URL from
 * FLEX_KODI_AVR_URL (kodi.sh reads /APPS/kodi/avr.url). Read only, no commands to the receiver.
 */
namespace FLEXVOL
{
//! the bar with \p percent (ring) and \p text; any thread
void Show(int percent, const std::string& text);

//! Kodi's own volume again (no values of ours in the bar)
void Clear();

/*!
 * \brief Read the receiver's volume in the background and show it (twice: right away and once
 *        more shortly after, when the last steps arrived over CEC).
 * \param fallback hint shown as a notification if the receiver does not answer
 * \return false: no receiver URL configured (nothing started)
 */
bool ShowReceiver(const std::string& fallback);

//! parses the status page; false if it has no volume. Public for the host test.
bool ParseDenonStatus(const std::string& xml, int& percent, std::string& text);
} // namespace FLEXVOL
