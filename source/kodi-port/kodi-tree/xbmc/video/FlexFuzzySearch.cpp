/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "FlexFuzzySearch.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace FLEXSEARCH
{
namespace
{
// U+00C0..U+00FF to one letter; ' ' for the multiplication/division signs, '#' for sharp s (ss)
const char LATIN_1[] = "aaaaaaaceeeeiiii"
                       "dnooooo ouuuuyt#"
                       "aaaaaaaceeeeiiii"
                       "dnooooo ouuuuyty";
static_assert(sizeof(LATIN_1) == 64 + 1, "U+00C0..U+00FF");

// U+0100..U+017F (Latin Extended-A) to one letter
const char LATIN_EXT_A[] = "aaaaaa" "cccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh" "iiiiiiiiii"
                           "ii" "jj" "kkk" "llllllllll" "nnnnnnnnn" "oooooo" "oo" "rrrrrr"
                           "ssssssss" "tttttt" "uuuuuuuuuuuu" "ww" "yyy" "zzzzzz" "s";
static_assert(sizeof(LATIN_EXT_A) == 128 + 1, "U+0100..U+017F");

// words that carry no meaning for finding a title (German, English, a few Romance articles)
const char* const STOP_WORDS[] = {"a",   "an",  "and",  "am",  "at",   "auf", "das", "de",
                                  "dem", "den", "der",  "des", "die",  "du",  "ein", "eine",
                                  "einem", "einen", "einer", "eines", "el", "il", "im", "in",
                                  "la",  "le",  "les",  "of",  "on",   "part", "teil", "the",
                                  "to",  "und", "vom",  "von", "zu",   "zum", "zur"};

const std::pair<const char*, const char*> ROMAN[] = {{"ii", "2"},  {"iii", "3"},  {"iv", "4"},
                                                     {"vi", "6"},  {"vii", "7"},  {"viii", "8"},
                                                     {"ix", "9"}};

bool IsStop(const std::string& w)
{
  for (const char* s : STOP_WORDS)
  {
    if (w == s)
      return true;
  }
  return false;
}

bool AllDigits(const std::string& w)
{
  return !w.empty() && std::all_of(w.begin(), w.end(), [](char c) { return c >= '0' && c <= '9'; });
}

//! next code point; an invalid byte counts as Latin-1 (old files)
uint32_t Next(const std::string& s, size_t& i, size_t& len)
{
  const unsigned char c = static_cast<unsigned char>(s[i]);
  len = 1;
  if (c < 0x80)
    return c;
  int more = 0;
  uint32_t cp = 0;
  if ((c & 0xE0) == 0xC0)
  {
    more = 1;
    cp = c & 0x1F;
  }
  else if ((c & 0xF0) == 0xE0)
  {
    more = 2;
    cp = c & 0x0F;
  }
  else if ((c & 0xF8) == 0xF0)
  {
    more = 3;
    cp = c & 0x07;
  }
  else
    return c;
  if (i + static_cast<size_t>(more) >= s.size())
    return c;
  for (int k = 1; k <= more; k++)
  {
    const unsigned char d = static_cast<unsigned char>(s[i + k]);
    if ((d & 0xC0) != 0x80)
      return c;
    cp = (cp << 6) | (d & 0x3F);
  }
  len = static_cast<size_t>(more) + 1;
  return cp;
}

std::vector<std::string> Words(const std::string& folded)
{
  std::vector<std::string> words;
  size_t start = 0;
  while (start < folded.size())
  {
    size_t end = folded.find(' ', start);
    if (end == std::string::npos)
      end = folded.size();
    if (end > start)
    {
      std::string w = folded.substr(start, end - start);
      for (const auto& r : ROMAN)
      {
        if (w == r.first)
        {
          w = r.second;
          break;
        }
      }
      words.push_back(std::move(w));
    }
    start = end + 1;
  }
  return words;
}

std::string NoSpaces(const std::string& s)
{
  std::string out;
  for (const char c : s)
  {
    if (c != ' ')
      out += c;
  }
  return out;
}

//! 0: no match, 1: same word, less: cut off (typing) or typing errors
double WordSim(const std::string& q, const std::string& f, bool last)
{
  if (q == f)
    return 1.0;
  if (AllDigits(q) || AllDigits(f))
    return 0.0; // numbers only as they are
  if (f.size() > q.size() && f.compare(0, q.size(), q) == 0 &&
      (q.size() >= 3 || (last && q.size() >= 2)))
    return 0.9;
  const int allowed = q.size() <= 3 ? 0 : q.size() <= 5 ? 1 : q.size() <= 9 ? 2 : 3;
  if (allowed == 0)
    return 0.0;
  int d = Distance(q, f, allowed);
  if (d <= allowed)
    return 0.85 - 0.1 * d;
  if (allowed >= 2 && f.size() > q.size())
  {
    // cut off and mistyped: one error less ("matrix" is no cut-off "martian")
    d = Distance(q, f.substr(0, q.size()), allowed - 1);
    if (d <= allowed - 1)
      return 0.7 - 0.1 * d;
  }
  return 0.0;
}
} // namespace

std::string Fold(const std::string& utf8)
{
  std::string out;
  out.reserve(utf8.size());
  const auto space = [&out]() {
    if (!out.empty() && out.back() != ' ')
      out += ' ';
  };
  for (size_t i = 0; i < utf8.size();)
  {
    size_t len = 1;
    const uint32_t cp = Next(utf8, i, len);
    if (cp < 0x80)
    {
      const char c = static_cast<char>(cp);
      if (c >= 'A' && c <= 'Z')
        out += static_cast<char>(c - 'A' + 'a');
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        out += c;
      else if (c == '\'' || c == '`')
      {
        // dropped: "Schindler's" = "Schindlers"
      }
      else if (c == '&')
      {
        space();
        out += "and ";
      }
      else
        space();
    }
    else if (cp >= 0xC0 && cp <= 0xFF)
    {
      const char m = LATIN_1[cp - 0xC0];
      if (m == '#')
        out += "ss";
      else if (m == ' ')
        space();
      else
        out += m;
    }
    else if (cp == 0xB4)
    {
      // acute accent used as apostrophe: dropped
    }
    else if (cp < 0xC0)
      space(); // Latin-1 punctuation and symbols, no-break space
    else if (cp >= 0x100 && cp <= 0x17F)
      out += LATIN_EXT_A[cp - 0x100];
    else if (cp == 0x2BC || cp == 0x2018 || cp == 0x2019)
    {
      // apostrophes: dropped
    }
    else if ((cp >= 0x2000 && cp <= 0x206F) || cp == 0x3000)
      space(); // dashes, quotes, spaces
    else
      out.append(utf8, i, len); // other scripts as they are
    i += len;
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  // ae/oe/ue are the typed umlauts: "Muenchen", "Munchen" and the umlaut fold the same
  std::string folded;
  folded.reserve(out.size());
  for (size_t i = 0; i < out.size(); i++)
  {
    folded += out[i];
    if ((out[i] == 'a' || out[i] == 'o' || out[i] == 'u') && i + 1 < out.size() && out[i + 1] == 'e')
      i++;
  }
  return folded;
}

Query Prepare(const std::string& utf8)
{
  Query q;
  q.folded = Fold(utf8);
  q.words = Words(q.folded);
  for (const auto& w : q.words)
  {
    if (!IsStop(w))
      q.keys.push_back(w);
  }
  if (q.keys.empty())
    q.keys = q.words;
  return q;
}

int Score(const Query& query, const std::string& utf8Title, int year)
{
  if (query.keys.empty())
    return 0;
  const std::string f = Fold(utf8Title);
  if (f.empty())
    return 0;
  const std::string& s = query.folded;
  if (f == s)
    return SAME;

  // the typed text inside the title as it is
  if (s.size() >= 2)
  {
    if (f.compare(0, s.size(), s) == 0)
      return f[s.size()] == ' ' ? 960 : 940;
    const size_t pos = f.find(" " + s);
    if (pos != std::string::npos)
    {
      const size_t end = pos + 1 + s.size();
      return (end == f.size() || f[end] == ' ') ? 920 : 900;
    }
    if (s.size() >= 4 && f.find(s) != std::string::npos)
      return 860;
  }
  const std::string sn = NoSpaces(s);
  if (sn.size() >= 4 && NoSpaces(f).find(sn) != std::string::npos)
    return PHRASE; // "Spiderman" = "Spider-Man"

  // word by word: every key has to be found somewhere
  const std::vector<std::string> fw = Words(f);
  const size_t n = fw.size();
  std::vector<bool> used(n, false);
  const std::string yearText = year > 0 ? std::to_string(year) : std::string();
  double sum = 0.0;
  size_t matched = 0;
  size_t missed = 0;
  for (size_t k = 0; k < query.keys.size(); k++)
  {
    const std::string& key = query.keys[k];
    const bool last = k + 1 == query.keys.size() && !query.words.empty() && key == query.words.back();
    double best = 0.0;
    size_t bestIndex = n;
    if (!yearText.empty() && key == yearText)
      best = 1.0;
    for (size_t i = 0; i < n && best < 1.0; i++)
    {
      double v = WordSim(key, fw[i], last);
      if (v > best)
      {
        best = v;
        bestIndex = i;
      }
      if (i + 1 < n)
      {
        v = 0.95 * WordSim(key, fw[i] + fw[i + 1], last); // written together
        if (v > best)
        {
          best = v;
          bestIndex = i;
        }
      }
    }
    if (best <= 0.0)
    {
      // a short word ("wy" for "way") may be wrong if the rest fits; numbers never
      if (key.size() <= 3 && !AllDigits(key))
      {
        missed++;
        continue;
      }
      return 0;
    }
    matched++;
    sum += best;
    if (bestIndex < n)
      used[bestIndex] = true;
  }
  if (missed && (matched < 2 || matched * 4 < query.keys.size() * 3))
    return 0;
  // how much of the title the query covers
  size_t titleKeys = 0;
  size_t covered = 0;
  for (size_t i = 0; i < n; i++)
  {
    if (IsStop(fw[i]))
      continue;
    titleKeys++;
    if (used[i])
      covered++;
  }
  const double coverage = titleKeys ? static_cast<double>(covered) / titleKeys : 1.0;
  return 400 + static_cast<int>(350.0 * sum / query.keys.size()) + static_cast<int>(50.0 * coverage);
}

int Cutoff(int best)
{
  return best >= PHRASE ? 650 : 1;
}

int Distance(const std::string& a, const std::string& b, int max)
{
  const int n = static_cast<int>(a.size());
  const int m = static_cast<int>(b.size());
  if (std::abs(n - m) > max)
    return max + 1;
  std::vector<int> before(m + 1, 0);
  std::vector<int> prev(m + 1);
  std::vector<int> cur(m + 1, 0);
  for (int j = 0; j <= m; j++)
    prev[j] = j;
  for (int i = 1; i <= n; i++)
  {
    cur[0] = i;
    int rowMin = cur[0];
    for (int j = 1; j <= m; j++)
    {
      const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
      int v = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
        v = std::min(v, before[j - 2] + 1); // swapped neighbours
      cur[j] = v;
      rowMin = std::min(rowMin, v);
    }
    if (rowMin > max)
      return max + 1;
    std::swap(before, prev);
    std::swap(prev, cur);
  }
  return std::min(prev[m], max + 1);
}
} // namespace FLEXSEARCH
