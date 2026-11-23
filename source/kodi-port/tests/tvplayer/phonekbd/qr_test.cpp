// Gegentest für FLEXQR::Encode (xbmc/dialogs/FlexQr.cpp): gibt je Text Version, Stufe, gewählte Maske
// und die Matrix für alle acht Masken aus ('#' dunkel, '.' hell). compare.py vergleicht mit qrref (Rust,
// Crate qrcode 0.14.1, gleicher Bitstrom: Byte-Modus, Abschluss, Füllbytes).
#include "FlexQr.h"

#include <cstdio>

int main(int argc, char** argv)
{
  for (int a = 1; a < argc; a++)
  {
    FLEXQR::Code best;
    if (!FLEXQR::Encode(argv[a], best))
    {
      std::printf("TOOLONG %s\n", argv[a]);
      continue;
    }
    std::printf("TEXT %s\nVERSION %d LEVEL %c BEST %d\n", argv[a], best.version, best.level, best.mask);
    for (int m = 0; m < 8; m++)
    {
      FLEXQR::Code c;
      FLEXQR::Encode(argv[a], c, m);
      std::printf("MASK %d\n", m);
      for (int y = 0; y < c.size; y++)
      {
        for (int x = 0; x < c.size; x++)
          std::putchar(c.Dark(x, y) ? '#' : '.');
        std::putchar('\n');
      }
    }
  }
  return 0;
}
