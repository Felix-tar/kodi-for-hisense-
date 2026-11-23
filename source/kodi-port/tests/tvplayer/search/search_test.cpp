// Host test for xbmc/video/FlexFuzzySearch.cpp (tolerant library search, FLEX-TV 18.11.2026).
//
//   search_test                      runs the checks below, exit code 0 = all passed
//   search_test <titles.tsv> <query>  ranks a list of "title<TAB>original<TAB>year" lines (top 10)
//
// The sample titles are well-known films, written here for the test; they are not taken from
// anyone's library. Non-ASCII letters are written as \x escapes so the file stays ASCII.

#include "FlexFuzzySearch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace FLEXSEARCH;

namespace
{
int failures = 0;
int checks = 0;

void Check(bool ok, const std::string& what)
{
  checks++;
  if (!ok)
  {
    failures++;
    std::printf("FAIL %s\n", what.c_str());
  }
}

struct Title
{
  std::string title;
  std::string original;
  int year;
};

// best of German and original title, like CGUIWindowVideoNav::DoSearch
int Best(const Query& q, const Title& t)
{
  return std::max(Score(q, t.title, t.year), Score(q, t.original, t.year));
}

std::vector<std::pair<int, const Title*>> Rank(const std::vector<Title>& lib, const std::string& text)
{
  const Query q = Prepare(text);
  std::vector<std::pair<int, const Title*>> hits;
  for (const auto& t : lib)
  {
    const int s = Best(q, t);
    if (s > 0)
      hits.emplace_back(s, &t);
  }
  std::stable_sort(hits.begin(), hits.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });
  if (!hits.empty())
  {
    const int cutoff = Cutoff(hits[0].first);
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [cutoff](const auto& h) { return h.first < cutoff; }),
               hits.end());
  }
  return hits;
}

const std::vector<Title> LIBRARY = {
    {"Der Herr der Ringe: Die Gef\xC3\xA4hrten", "The Lord of the Rings: The Fellowship of the Ring", 2001},
    {"Der Herr der Ringe: Die zwei T\xC3\xBCrme", "The Lord of the Rings: The Two Towers", 2002},
    {"Zur\xC3\xBC" "ck in die Zukunft II", "Back to the Future Part II", 1989},
    {"Zur\xC3\xBC" "ck in die Zukunft", "Back to the Future", 1985},
    {"F\xC3\xBCr eine Handvoll Dollar", "Per un pugno di dollari", 1964},
    {"Chihiros Reise ins Zauberland", "Spirited Away", 2001},
    {"Spider-Man: No Way Home", "Spider-Man: No Way Home", 2021},
    {"Die fabelhafte Welt der Am\xC3\xA9lie", "Le Fabuleux Destin d'Am\xC3\xA9lie Poulain", 2001},
    {"Guardians of the Galaxy", "Guardians of the Galaxy", 2014},
    {"2001: Odyssee im Weltraum", "2001: A Space Odyssey", 1968},
    {"Das Boot", "Das Boot", 1981},
    {"Matrix", "The Matrix", 1999},
    {"Schindlers Liste", "Schindler's List", 1993},
    {"Die Stra\xC3\x9F" "e", "La Strada", 1954},
    {"Ocean's Eleven", "Ocean's Eleven", 2001},
    {"Der Marsianer", "The Martian", 2015},
};

void Top(const std::string& query, const std::string& expected)
{
  const auto hits = Rank(LIBRARY, query);
  const bool ok = !hits.empty() && hits[0].second->title == expected;
  Check(ok, "\"" + query + "\" -> \"" + expected + "\", got " +
                (hits.empty() ? std::string("nothing") : "\"" + hits[0].second->title + "\" (" +
                                                             std::to_string(hits[0].first) + ")"));
}

void Within(const std::string& query, const std::string& expected, size_t places)
{
  const auto hits = Rank(LIBRARY, query);
  bool ok = false;
  for (size_t i = 0; i < hits.size() && i < places; i++)
    ok = ok || hits[i].second->title == expected;
  Check(ok, "\"" + query + "\" has \"" + expected + "\" in the first " + std::to_string(places));
}

void Nothing(const std::string& query)
{
  const auto hits = Rank(LIBRARY, query);
  Check(hits.empty(), "\"" + query + "\" finds nothing, got " + std::to_string(hits.size()) +
                          (hits.empty() ? "" : " (first \"" + hits[0].second->title + "\")"));
}

void RunChecks()
{
  // folding
  Check(Fold("F\xC3\xBCr eine Handvoll Dollar") == "fur eine handvoll dollar", "fold umlaut");
  Check(Fold("Fuer") == "fur", "fold ue");
  Check(Fold("STRA\xC3\x9F" "E") == "strasse", "fold sharp s");
  Check(Fold("Am\xC3\xA9lie") == "amelie", "fold accent");
  Check(Fold("Schindler's  List!") == "schindlers list", "fold apostrophe, punctuation");
  Check(Fold("Spider-Man: No Way Home") == "spider man no way home", "fold dash, colon");
  Check(Fold("Tom & Jerry") == "tom and jerry", "fold ampersand");
  Check(Fold("Schindler\xE2\x80\x99s") == "schindlers", "fold typographic apostrophe");
  Check(Fold("F\xFCr") == "fur", "fold Latin-1 byte");
  Check(Fold("\xC5\x81\xC3\xB3" "d\xC5\xBA") == "lodz", "fold Latin Extended-A");
  Check(Fold("  ") .empty(), "fold blank");

  // distance
  Check(Distance("rings", "rigns", 2) == 1, "distance swap");
  Check(Distance("kitten", "sitting", 3) == 3, "distance kitten");
  Check(Distance("abc", "abc", 0) == 0, "distance same");
  Check(Distance("galaxie", "galaxy", 2) == 2, "distance galaxie");
  Check(Distance("abcdef", "uvwxyz", 2) == 3, "distance capped");
  Check(Distance("", "abc", 3) == 3, "distance empty");

  // ranking
  Top("matrix", "Matrix");
  Top("the matrix", "Matrix");
  Top("das boot", "Das Boot");
  Top("boot", "Das Boot");
  Within("herr der ringe", "Der Herr der Ringe: Die Gef\xC3\xA4hrten", 2);
  Within("lord of the rigns", "Der Herr der Ringe: Die zwei T\xC3\xBCrme", 2);
  Top("herr der ringe zwei turme", "Der Herr der Ringe: Die zwei T\xC3\xBCrme");
  Top("two towers", "Der Herr der Ringe: Die zwei T\xC3\xBCrme");
  Top("zuruck in die zukunft 2", "Zur\xC3\xBC" "ck in die Zukunft II");
  Top("zurueck in die zukunft ii", "Zur\xC3\xBC" "ck in die Zukunft II");
  Within("back to the futur", "Zur\xC3\xBC" "ck in die Zukunft", 2);
  Top("handvol dolar", "F\xC3\xBCr eine Handvoll Dollar");
  Top("fuer eine handvoll", "F\xC3\xBCr eine Handvoll Dollar");
  Top("spirited away", "Chihiros Reise ins Zauberland");
  Top("chihiro", "Chihiros Reise ins Zauberland");
  Top("spiderman", "Spider-Man: No Way Home");
  Top("spider man no wy home", "Spider-Man: No Way Home");
  Top("amelie", "Die fabelhafte Welt der Am\xC3\xA9lie");
  Top("gardians of the galaxi", "Guardians of the Galaxy");
  Top("guardians galaxy", "Guardians of the Galaxy");
  Top("odysee im weltraum", "2001: Odyssee im Weltraum");
  Top("space odyssey 1968", "2001: Odyssee im Weltraum");
  Top("schindlers list", "Schindlers Liste");
  Top("schindler", "Schindlers Liste");
  Top("strasse", "Die Stra\xC3\x9F" "e");
  Top("oceans eleven", "Ocean's Eleven");
  Top("guardi", "Guardians of the Galaxy"); // still typing
  Nothing("titanic");
  Nothing("xyz");
  Nothing("herr der titanic");
  Nothing("zukunft 3");
  Check(Rank(LIBRARY, "matrix").size() == 1, "\"matrix\" is not \"The Martian\"");
  Top("marsianer", "Der Marsianer");
  Top("the martain", "Der Marsianer"); // swapped letters in the original title

  // people (the window ranks actor names the same way)
  const Query keanu = Prepare("keanu reevs");
  Check(Score(keanu, "Keanu Reeves") > 0, "actor with typing error");
  Check(Score(Prepare("reeves"), "Keanu Reeves") > 0, "actor by last name");
  Check(Score(Prepare("schwarzeneger"), "Arnold Schwarzenegger") > 0, "actor long name");
  Check(Score(Prepare("keanu reevs"), "Keanu Reeves") > Score(Prepare("keanu reevs"), "Keanu Rivers"),
        "closer name ranks higher");
  Check(Score(Prepare("tom"), "Tom Hanks") > 0 && Score(Prepare("tom"), "Tim Hanks") == 0,
        "short words exact only");

  // robustness
  Check(Score(Prepare(""), "Matrix") == 0, "empty query");
  Check(Score(Prepare("matrix"), "") == 0, "empty title");
  Check(Score(Prepare("\xFF\xFE\xC3"), "Matrix") == 0, "broken UTF-8");
  Check(Score(Prepare("m"), "Matrix") == 0, "one letter is no search");
}

int RankFile(const char* file, const std::string& query)
{
  std::ifstream in(file);
  if (!in)
  {
    std::printf("cannot read %s\n", file);
    return 2;
  }
  std::vector<Title> lib;
  std::string line;
  while (std::getline(in, line))
  {
    const size_t a = line.find('\t');
    const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos)
      continue;
    lib.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1), std::atoi(line.c_str() + b + 1)});
  }
  const auto start = std::chrono::steady_clock::now();
  const auto hits = Rank(lib, query);
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  std::printf("%zu titles, %zu hits, %lld us\n", lib.size(), hits.size(), static_cast<long long>(us));
  for (size_t i = 0; i < hits.size() && i < 10; i++)
    std::printf("%4d  %s / %s (%d)\n", hits[i].first, hits[i].second->title.c_str(),
                hits[i].second->original.c_str(), hits[i].second->year);
  return 0;
}
} // namespace

int main(int argc, char** argv)
{
  if (argc == 3)
    return RankFile(argv[1], argv[2]);
  RunChecks();
  std::printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
