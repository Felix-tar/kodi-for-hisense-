/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include <string>
#include <vector>

namespace FLEXQR
{

struct Code
{
  int version = 0; //!< 1..10
  int size = 0; //!< modules per side: 17 + 4 * version
  int mask = -1; //!< 0..7
  char level = 'M'; //!< 'M', or 'L' when M does not fit
  std::vector<bool> modules; //!< row by row, true = dark
  bool Dark(int x, int y) const { return modules[static_cast<size_t>(y * size + x)]; }
};

/*!
 * \brief QR code (ISO/IEC 18004) for a short text such as a URL: byte mode, versions 1 to 10,
 *        the smallest version with error correction level M (L if M does not fit anywhere).
 *
 * Written for the phone keyboard (FlexPhoneKeyboard), no Kodi headers: tested on its own against
 * another implementation (kodi-port/tests/tvplayer/phonekbd).
 *
 * \param forcedMask 0..7 for tests; -1 = the mask with the lowest penalty (the standard's rules)
 * \return false if the text does not fit into version 10
 */
bool Encode(const std::string& text, Code& out, int forcedMask = -1);

} // namespace FLEXQR
