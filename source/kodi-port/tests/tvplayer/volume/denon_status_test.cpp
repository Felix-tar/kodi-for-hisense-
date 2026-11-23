// Host test for FLEXVOL::ParseDenonStatus (xbmc/windowing/directfb/FlexDenonStatus.cpp): the
// receiver's status page -> ring percent and the number in Kodi's volume bar (FLEX-TV 18.11.2026).
// The sample pages follow the format of goform/formMainZone_MainZoneXmlStatusLite.xml.

#include "FlexVolumeBar.h"

#include <cstdio>
#include <string>

namespace
{
int failures = 0;
int checks = 0;

std::string Page(const std::string& volume, const std::string& mute, const std::string& display)
{
  return "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<item>\n<Power><value>ON</value></Power>\n"
         "<InputFuncSelect><value>TV</value></InputFuncSelect>\n<VolumeDisplay><value>" +
         display + "</value></VolumeDisplay>\n<MasterVolume><value>" + volume +
         "</value></MasterVolume>\n<Mute><value>" + mute + "</value></Mute>\n</item>\n";
}

void Expect(const std::string& xml, bool ok, int percent, const std::string& text, const char* what)
{
  checks++;
  int p = -1;
  std::string t;
  const bool r = FLEXVOL::ParseDenonStatus(xml, p, t);
  if (r != ok || (ok && (p != percent || t != text)))
  {
    failures++;
    std::printf("FAIL %s: got %d %d \"%s\"\n", what, r ? 1 : 0, p, t.c_str());
  }
}
} // namespace

int main()
{
  Expect(Page("-32.5", "off", "Absolute"), true, 48, "47,5", "absolute with half step");
  Expect(Page("-40.0", "off", "Absolute"), true, 41, "40", "absolute whole");
  Expect(Page("-80.0", "off", "Absolute"), true, 0, "0", "bottom");
  Expect(Page("--", "off", "Absolute"), true, 0, "0", "bottom shown as --");
  Expect(Page("18.0", "off", "Absolute"), true, 100, "98", "top");
  Expect(Page("-32.5", "on", "Absolute"), true, 48, "Stumm", "muted");
  Expect(Page("-32.5", "off", "Relative"), true, 48, "-32,5 dB", "relative display");
  Expect(Page("0.0", "off", "Relative"), true, 82, "0 dB", "relative zero");
  Expect(Page(" -32.5 ", "off", "Absolute"), true, 48, "47,5", "blanks around the value");
  Expect(Page("-32,5", "off", "Absolute"), false, 0, "", "comma is no number");
  Expect(Page("-99.0", "off", "Absolute"), false, 0, "", "below the scale");
  Expect(Page("19.0", "off", "Absolute"), false, 0, "", "above the scale");
  Expect(Page("abc", "off", "Absolute"), false, 0, "", "no number");
  Expect(Page("", "off", "Absolute"), false, 0, "", "empty");
  Expect("<item><Power><value>ON</value></Power></item>", false, 0, "", "no volume");
  Expect("Error 403: Forbidden", false, 0, "", "error page");
  Expect("", false, 0, "", "nothing");
  Expect("<MasterVolume><value>-3", false, 0, "", "cut off");
  std::printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
