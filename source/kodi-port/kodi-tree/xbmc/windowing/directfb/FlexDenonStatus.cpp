/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

// FLEXVOL::ParseDenonStatus without Kodi dependencies (host test kodi-port/tests/tvplayer/volume).

#include "FlexVolumeBar.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
//! the text of <tag>...<value>TEXT</value>, trimmed
bool Value(const std::string& xml, const char* tag, std::string& out)
{
  size_t p = xml.find(std::string("<") + tag + ">");
  if (p == std::string::npos)
    return false;
  p = xml.find("<value>", p);
  if (p == std::string::npos)
    return false;
  p += 7;
  const size_t end = xml.find("</value>", p);
  if (end == std::string::npos || end - p > 16)
    return false;
  out = xml.substr(p, end - p);
  const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!out.empty() && blank(out.back()))
    out.pop_back();
  size_t start = 0;
  while (start < out.size() && blank(out[start]))
    start++;
  out.erase(0, start);
  return true;
}

//! "-32.5", "-80", "5.0" in tenths of a dB; no locale, no exponent
bool Tenths(const std::string& s, int& tenths)
{
  size_t i = 0;
  bool negative = false;
  if (i < s.size() && (s[i] == '-' || s[i] == '+'))
    negative = s[i++] == '-';
  int whole = 0;
  size_t digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 3)
  {
    whole = whole * 10 + (s[i++] - '0');
    digits++;
  }
  if (!digits)
    return false;
  int tenth = 0;
  if (i < s.size() && s[i] == '.')
  {
    i++;
    if (i >= s.size() || s[i] < '0' || s[i] > '9')
      return false;
    tenth = s[i++] - '0';
  }
  if (i != s.size())
    return false;
  tenths = (whole * 10 + tenth) * (negative ? -1 : 1);
  return true;
}

//! tenths as German text: 475 -> "47,5", 480 -> "48"
std::string German(int tenths)
{
  const bool negative = tenths < 0;
  const int a = std::abs(tenths);
  std::string out = (negative ? "-" : "") + std::to_string(a / 10);
  if (a % 10)
    out += "," + std::to_string(a % 10);
  return out;
}
} // namespace

bool FLEXVOL::ParseDenonStatus(const std::string& xml, int& percent, std::string& text)
{
  std::string volume;
  std::string mute;
  std::string display;
  if (!Value(xml, "MasterVolume", volume))
    return false;
  Value(xml, "Mute", mute);
  Value(xml, "VolumeDisplay", display);
  int db = 0; // tenths of a dB, 0 dB = 80 on the absolute scale
  if (volume == "--")
    db = -800; // the receiver shows "--" at the very bottom
  else if (!Tenths(volume, db))
    return false;
  if (db < -800 || db > 180)
    return false;
  const int absolute = db + 800; // 0..980: the receiver's "absolute" scale 0..98
  percent = std::clamp(static_cast<int>(std::lround(absolute / 9.8)), 0, 100);
  if (mute == "on" || mute == "ON")
    text = "Stumm";
  else if (display == "Relative")
    text = German(db) + " dB";
  else
    text = German(absolute);
  return true;
}
