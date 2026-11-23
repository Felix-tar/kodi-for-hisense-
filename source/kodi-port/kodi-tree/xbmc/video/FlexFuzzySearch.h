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

/*!
 * \brief Tolerant title matching for the video library search (FLEX-TV, 18.11.2026).
 *
 * Kodi's own search only finds the typed text as it is inside the (German) title. This ranks a
 * title instead: umlauts, sharp s and accents are folded (a-umlaut = ae = a, e-acute = e),
 * punctuation and articles do not matter, words may be cut off at the end (typing), have typing
 * errors (edit distance with swapped letters) or be written together/apart, roman numerals count
 * as numbers. The caller
 * scores both the German and the original (mostly English) title and takes the better one.
 *
 * No Kodi dependencies, so the host test (kodi-port/tests/tvplayer/search) builds it as is.
 */
namespace FLEXSEARCH
{
//! lower-case form: umlauts and accents folded, ae/oe/ue -> a/o/u, sharp s -> ss, "&" -> "and",
//! apostrophes dropped, other punctuation as single spaces
std::string Fold(const std::string& utf8);

struct Query
{
  std::string folded; //!< Fold(text)
  std::vector<std::string> words; //!< all words (roman numerals as digits)
  std::vector<std::string> keys; //!< the words that must match: no articles etc., unless only those
};

Query Prepare(const std::string& utf8);

constexpr int SAME = 1000; //!< same title
constexpr int PHRASE = 840; //!< from here up: the typed text is inside the title as it is

/*!
 * \brief How well a title matches the query.
 * \param year the title's year, 0 if none; a typed year then counts as a matching word
 * \return 0: no match; otherwise up to SAME, higher is better
 */
int Score(const Query& query, const std::string& utf8Title, int year = 0);

//! lowest score worth showing when the best hit has \p best: once the typed text was found as it
//! is, loose word matches are left out
int Cutoff(int best);

//! optimal string alignment distance (Levenshtein plus swapped neighbours); stops early and
//! returns max + 1 as soon as the distance is surely larger than max
int Distance(const std::string& a, const std::string& b, int max);
} // namespace FLEXSEARCH
