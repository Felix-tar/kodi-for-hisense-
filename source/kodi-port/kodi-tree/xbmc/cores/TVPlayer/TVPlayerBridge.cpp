/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TVPlayerBridge.h"

#include "TVBridgeIO.h"
#include "utils/JSONVariantParser.h"
#include "utils/JSONVariantWriter.h"
#include "utils/Variant.h"
#include "utils/log.h"

#include <cmath>
#include <cstring>
#include <ctime>

using namespace TVPLAYER;

namespace
{
constexpr const char* FILE_PLAY = "play.json";
constexpr const char* FILE_CTL = "ctl";
constexpr const char* FILE_PLAYER = "player.json";
constexpr const char* FILE_TVKEYS = "tvkeys";
constexpr const char* FILE_TVACK = "tvack";

// largest position/duration the TV player accepts (s)
constexpr int64_t MAX_PLAYTIME_S = 360000;

const char* const STATES[] = {"switching", "esready", "preparing", "playing", "buffering",
                              "paused",    "ended",   "stopped",   "error",   "refused"};
const char* const REASONS[] = {"key",         "kodi",     "notHome",    "dialog",   "app",
                               "busy",        "bad",      "dmpForeign", "dmpTimeout",
                               "prepTimeout", "player",   "sourceOut",  "taken",    "superseded",
                               "kodiGone",    "noSeek",   "noResume",   "early",    "stopped"};
const int SPEEDS[] = {100, 200, 400, 800, 1600, 100200, 100400, 100800, 101600};

template<size_t N>
bool InList(const char* const (&list)[N], const std::string& value)
{
  for (const char* entry : list)
  {
    if (value == entry)
      return true;
  }
  return false;
}

bool ToJson(const CVariant& value, std::string& json)
{
  return CJSONVariantWriter::Write(value, json, true) && !json.empty();
}

/*!
 * \brief Reads an integer member.
 * \return false if it is present but not an integer in [min, max], or missing but required
 */
bool GetInt(const CVariant& object,
            const char* key,
            int64_t& out,
            int64_t min,
            int64_t max,
            bool required = false)
{
  if (!object.isMember(key))
    return !required;
  const CVariant& value = object[key];
  int64_t number = 0;
  if (value.isSignedInteger())
    number = value.asInteger();
  else if (value.isUnsignedInteger())
  {
    if (value.asUnsignedInteger() > static_cast<uint64_t>(INT64_MAX))
      return false;
    number = static_cast<int64_t>(value.asUnsignedInteger());
  }
  else if (value.isDouble())
  {
    // JSON.stringify writes whole numbers without a fraction, but be tolerant
    const double d = value.asDouble();
    if (!std::isfinite(d) || std::floor(d) != d || std::fabs(d) > 1e15)
      return false;
    number = static_cast<int64_t>(d);
  }
  else
    return false;
  if (number < min || number > max)
    return false;
  out = number;
  return true;
}

bool GetString(const CVariant& object, const char* key, std::string& out, size_t maxLength)
{
  if (!object.isMember(key))
    return true;
  const CVariant& value = object[key];
  if (!value.isString())
    return false;
  out = value.asString();
  return out.size() <= maxLength && IsPrintableAscii(out);
}

bool IsCodecText(const std::string& text)
{
  for (const char c : text)
  {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          c == ' ' || c == '.' || c == '_' || c == '+' || c == '-'))
      return false;
  }
  return true;
}

bool IsWindowText(const std::string& text)
{
  for (const char c : text)
  {
    if (!((c >= '0' && c <= '9') || c == 'x'))
      return false;
  }
  return true;
}

bool IsSpeed(int64_t value)
{
  for (const int speed : SPEEDS)
  {
    if (value == speed)
      return true;
  }
  return false;
}
} // namespace

void CTVBridge::Reset()
{
  m_lastN = 0;
  m_lastJson.clear();
  m_invalidLogged = false;
}

bool CTVBridge::WritePlay(const std::string& sid,
                          uint16_t port,
                          const std::string& tok,
                          const std::string& name,
                          int64_t resumeS,
                          float dar,
                          int ov,
                          int fb)
{
  // dar: 0 (unknown) or 0.5..4, four decimals are plenty for the window
  double darValue = 0.0;
  if (std::isfinite(dar) && dar >= 0.5f && dar <= 4.0f)
    darValue = std::round(static_cast<double>(dar) * 10000.0) / 10000.0;

  CVariant play(CVariant::VariantTypeObject);
  play["v"] = 1;
  play["sid"] = sid;
  // flex.js accepts it only within 3 s of this time (system clock, like Date.now() there)
  play["t"] = static_cast<int64_t>(std::time(nullptr));
  play["port"] = static_cast<int>(port);
  play["tok"] = tok;
  play["name"] = name;
  play["resume"] = resumeS < 0 ? 0 : (resumeS > MAX_PLAYTIME_S ? MAX_PLAYTIME_S : resumeS);
  play["dar"] = darValue;
  play["ov"] = ov ? 1 : 0;
  play["fb"] = fb ? 1 : 0;

  std::string json;
  return ToJson(play, json) && CTVBridgeIO::Get().WriteSigned(FILE_PLAY, json);
}

bool CTVBridge::WritePlayEs(const std::string& sid)
{
  CVariant play(CVariant::VariantTypeObject);
  play["v"] = 1;
  play["sid"] = sid;
  play["t"] = static_cast<int64_t>(std::time(nullptr)); // as in WritePlay
  play["es"] = 1;

  std::string json;
  return ToJson(play, json) && CTVBridgeIO::Get().WriteSigned(FILE_PLAY, json);
}

bool CTVBridge::WriteCtl(const std::string& sid, const TVCtl& ctl)
{
  CVariant c(CVariant::VariantTypeObject);
  c["v"] = 1;
  c["sid"] = sid;
  c["seq"] = static_cast<int64_t>(ctl.seq);
  c["hb"] = static_cast<int64_t>(ctl.hb);
  c["pause"] = ctl.pause ? 1 : 0;
  c["seekN"] = static_cast<int64_t>(ctl.seekN);
  c["seekTo"] = ctl.seekTo < 0 ? 0 : ctl.seekTo;
  c["seekAcc"] = ctl.seekAcc == 100 ? 100 : 0;
  c["speed"] = IsSpeed(ctl.speed) ? ctl.speed : 100;
  c["audio"] = ctl.audio;
  c["sub"] = ctl.sub;
  c["stop"] = ctl.stop ? 1 : 0;

  std::string json;
  return ToJson(c, json) && CTVBridgeIO::Get().WriteSigned(FILE_CTL, json);
}

bool CTVBridge::ReadPlayer(const std::string& sid, int64_t maxAck, TVStatus& out)
{
  std::string json;
  if (!CTVBridgeIO::Get().ReadSigned(FILE_PLAYER, json) || json == m_lastJson)
    return false;
  m_lastJson = json;

  CVariant o;
  if (!CJSONVariantParser::Parse(json, o) || !o.isObject())
    return false;

  // another playback (an older or the next one) or an older version of the protocol
  int64_t version = 0;
  if (!GetInt(o, "v", version, 1, 1, true) || !o["sid"].isString() || o["sid"].asString() != sid)
    return false;

  TVStatus st;
  st.sid = sid;
  int64_t value = 0;
  bool ok = GetInt(o, "n", st.n, 1, INT64_MAX, true) && o["st"].isString();
  if (ok)
    st.st = o["st"].asString();
  ok = ok && InList(STATES, st.st);
  ok = ok && GetInt(o, "ack", st.ack, -1, maxAck);
  ok = ok && GetString(o, "why", st.why, 32);
  if (ok && !InList(REASONS, st.why))
    st.why.clear();
  value = st.mp;
  ok = ok && GetInt(o, "mp", value, INT32_MIN, INT32_MAX);
  st.mp = static_cast<int>(value);
  value = st.flags;
  ok = ok && GetInt(o, "flags", value, INT32_MIN, INT32_MAX);
  st.flags = static_cast<int>(value);
  value = st.err;
  ok = ok && GetInt(o, "err", value, INT32_MIN, INT32_MAX);
  st.err = static_cast<int>(value);
  ok = ok && GetInt(o, "pos", st.pos, 0, MAX_PLAYTIME_S);
  ok = ok && GetInt(o, "dur", st.dur, 0, MAX_PLAYTIME_S);
  value = 0;
  ok = ok && GetInt(o, "seek", value, 0, 1);
  st.seek = value == 1;
  value = 0;
  ok = ok && GetInt(o, "eof", value, 0, 1);
  st.eof = value == 1;
  ok = ok && GetString(o, "acodec", st.acodec, 32) && IsCodecText(st.acodec);
  value = st.aud;
  ok = ok && GetInt(o, "aud", value, -1, 63);
  st.aud = static_cast<int>(value);
  value = st.spd;
  ok = ok && GetInt(o, "spd", value, 100, 101600) && IsSpeed(value);
  st.spd = static_cast<int>(value);
  ok = ok && GetString(o, "win", st.win, 32) && IsWindowText(st.win);
  // "t" (flex.js' wall clock) is for the log only: Kodi counts from its own steady clock

  if (!ok)
  {
    if (!m_invalidLogged)
      CLog::Log(LOGWARNING, "CTVBridge: ignoring a player.json with invalid values (sid {})", sid);
    m_invalidLogged = true;
    return false;
  }
  if (st.n <= m_lastN)
    return false; // repeated or older
  m_lastN = st.n;
  out = std::move(st);
  return true;
}

void CTVBridge::RemovePlay()
{
  CTVBridgeIO::Get().Remove(FILE_PLAY);
}

bool TVPLAYER::WriteTvKeys(
    const std::string& kid, uint32_t seq, uint32_t up, uint32_t down, uint32_t mute)
{
  CVariant keys(CVariant::VariantTypeObject);
  keys["v"] = 1;
  keys["kid"] = kid;
  keys["seq"] = static_cast<int64_t>(seq);
  keys["up"] = static_cast<int64_t>(up);
  keys["down"] = static_cast<int64_t>(down);
  keys["mute"] = static_cast<int64_t>(mute);

  std::string json;
  return ToJson(keys, json) && CTVBridgeIO::Get().WriteSigned(FILE_TVKEYS, json);
}

bool TVPLAYER::ReadTvAck(const std::string& kid, TVAck& out)
{
  std::string json;
  if (!CTVBridgeIO::Get().ReadSigned(FILE_TVACK, json))
    return false;

  CVariant o;
  if (!CJSONVariantParser::Parse(json, o) || !o.isObject())
    return false;

  int64_t version = 0;
  if (!GetInt(o, "v", version, 1, 1, true) || !o["kid"].isString() || o["kid"].asString() != kid)
    return false;

  TVAck ack;
  ack.kid = kid;
  int64_t arc = 0;
  int64_t vol = -1;
  int64_t mute = 0;
  if (!GetInt(o, "seq", ack.seq, 0, INT64_MAX, true) || !GetInt(o, "arc", arc, 0, 1) ||
      !GetInt(o, "vol", vol, -1, 100) || !GetInt(o, "mute", mute, 0, 1))
    return false;
  ack.arc = arc == 1;
  ack.vol = static_cast<int>(vol);
  ack.mute = mute == 1;
  out = std::move(ack);
  return true;
}
