/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "FlexQr.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace
{
constexpr int MAX_VERSION = 10;
// index 0 = level L, 1 = level M; column = version (0 unused). ISO/IEC 18004 table 9
constexpr int ECC_PER_BLOCK[2][MAX_VERSION + 1] = {{0, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18},
                                                   {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26}};
constexpr int NUM_BLOCKS[2][MAX_VERSION + 1] = {{0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4},
                                                {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5}};
constexpr int FORMAT_LEVEL_BITS[2] = {1, 0}; // L = 01, M = 00
constexpr int PENALTY_N1 = 3;
constexpr int PENALTY_N2 = 3;
constexpr int PENALTY_N3 = 40;
constexpr int PENALTY_N4 = 10;

bool Bit(long x, int i)
{
  return ((x >> i) & 1) != 0;
}

int RawDataModules(int ver)
{
  int result = (16 * ver + 128) * ver + 64;
  if (ver >= 2)
  {
    const int numAlign = ver / 7 + 2;
    result -= (25 * numAlign - 10) * numAlign - 55;
    if (ver >= 7)
      result -= 36;
  }
  return result;
}

int DataCodewords(int ver, int level)
{
  return RawDataModules(ver) / 8 - ECC_PER_BLOCK[level][ver] * NUM_BLOCKS[level][ver];
}

uint8_t GfMul(uint8_t x, uint8_t y)
{
  int z = 0;
  for (int i = 7; i >= 0; i--)
  {
    z = (z << 1) ^ ((z >> 7) * 0x11D);
    z ^= ((y >> i) & 1) * x;
  }
  return static_cast<uint8_t>(z);
}

std::vector<uint8_t> RsDivisor(int degree)
{
  std::vector<uint8_t> result(static_cast<size_t>(degree));
  result.back() = 1;
  uint8_t root = 1;
  for (int i = 0; i < degree; i++)
  {
    for (size_t j = 0; j < result.size(); j++)
    {
      result[j] = GfMul(result[j], root);
      if (j + 1 < result.size())
        result[j] ^= result[j + 1];
    }
    root = GfMul(root, 0x02);
  }
  return result;
}

std::vector<uint8_t> RsRemainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor)
{
  std::vector<uint8_t> result(divisor.size());
  for (const uint8_t b : data)
  {
    const uint8_t factor = b ^ result[0];
    result.erase(result.begin());
    result.push_back(0);
    for (size_t i = 0; i < result.size(); i++)
      result[i] ^= GfMul(divisor[i], factor);
  }
  return result;
}

class CMatrix
{
public:
  CMatrix(int version, int level) : m_ver(version), m_level(level), m_size(version * 4 + 17)
  {
    m_dark.assign(static_cast<size_t>(m_size * m_size), false);
    m_function.assign(static_cast<size_t>(m_size * m_size), false);
  }

  int Size() const { return m_size; }
  bool Dark(int x, int y) const { return m_dark[Index(x, y)]; }

  void DrawFunctionPatterns()
  {
    for (int i = 0; i < m_size; i++)
    {
      SetFunction(6, i, i % 2 == 0);
      SetFunction(i, 6, i % 2 == 0);
    }
    DrawFinder(3, 3);
    DrawFinder(m_size - 4, 3);
    DrawFinder(3, m_size - 4);
    const std::vector<int> pos = AlignmentPositions();
    const size_t n = pos.size();
    for (size_t i = 0; i < n; i++)
    {
      for (size_t j = 0; j < n; j++)
      {
        if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0))
          continue; // under the finder patterns
        DrawAlignment(pos[i], pos[j]);
      }
    }
    DrawFormatBits(0); // reserves the area, overwritten after the mask choice
    DrawVersion();
  }

  void DrawCodewords(const std::vector<uint8_t>& data)
  {
    size_t i = 0;
    for (int right = m_size - 1; right >= 1; right -= 2)
    {
      if (right == 6)
        right = 5;
      for (int vert = 0; vert < m_size; vert++)
      {
        for (int j = 0; j < 2; j++)
        {
          const int x = right - j;
          const bool upward = ((right + 1) & 2) == 0;
          const int y = upward ? m_size - 1 - vert : vert;
          if (!m_function[Index(x, y)] && i < data.size() * 8)
          {
            m_dark[Index(x, y)] = Bit(data[i >> 3], 7 - static_cast<int>(i & 7));
            i++;
          }
        }
      }
    }
  }

  void ApplyMask(int mask)
  {
    for (int y = 0; y < m_size; y++)
    {
      for (int x = 0; x < m_size; x++)
      {
        bool invert = false;
        switch (mask)
        {
          case 0: invert = (x + y) % 2 == 0; break;
          case 1: invert = y % 2 == 0; break;
          case 2: invert = x % 3 == 0; break;
          case 3: invert = (x + y) % 3 == 0; break;
          case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
          case 5: invert = x * y % 2 + x * y % 3 == 0; break;
          case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
          default: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
        }
        if (invert && !m_function[Index(x, y)])
          m_dark[Index(x, y)] = !m_dark[Index(x, y)];
      }
    }
  }

  void DrawFormatBits(int mask)
  {
    const int data = (FORMAT_LEVEL_BITS[m_level] << 3) | mask;
    int rem = data;
    for (int i = 0; i < 10; i++)
      rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = ((data << 10) | rem) ^ 0x5412;
    for (int i = 0; i <= 5; i++)
      SetFunction(8, i, Bit(bits, i));
    SetFunction(8, 7, Bit(bits, 6));
    SetFunction(8, 8, Bit(bits, 7));
    SetFunction(7, 8, Bit(bits, 8));
    for (int i = 9; i < 15; i++)
      SetFunction(14 - i, 8, Bit(bits, i));
    for (int i = 0; i < 8; i++)
      SetFunction(m_size - 1 - i, 8, Bit(bits, i));
    for (int i = 8; i < 15; i++)
      SetFunction(8, m_size - 15 + i, Bit(bits, i));
    SetFunction(8, m_size - 8, true); // the dark module
  }

  long Penalty() const
  {
    long result = 0;
    for (int pass = 0; pass < 2; pass++) // rows, then columns
    {
      for (int a = 0; a < m_size; a++)
      {
        bool runColor = false;
        int run = 0;
        std::array<int, 7> history{};
        for (int b = 0; b < m_size; b++)
        {
          const bool dark = pass == 0 ? Dark(b, a) : Dark(a, b);
          if (dark == runColor)
          {
            run++;
            if (run == 5)
              result += PENALTY_N1;
            else if (run > 5)
              result++;
          }
          else
          {
            AddHistory(run, history);
            if (!runColor)
              result += CountFinderLike(history) * PENALTY_N3;
            runColor = dark;
            run = 1;
          }
        }
        result += TerminateAndCount(runColor, run, history) * PENALTY_N3;
      }
    }
    for (int y = 0; y < m_size - 1; y++)
    {
      for (int x = 0; x < m_size - 1; x++)
      {
        const bool c = Dark(x, y);
        if (c == Dark(x + 1, y) && c == Dark(x, y + 1) && c == Dark(x + 1, y + 1))
          result += PENALTY_N2;
      }
    }
    long dark = 0;
    for (const bool d : m_dark)
      dark += d ? 1 : 0;
    const long total = static_cast<long>(m_size) * m_size;
    const long k = (std::labs(dark * 20 - total * 10) + total - 1) / total - 1;
    result += k * PENALTY_N4;
    return result;
  }

private:
  size_t Index(int x, int y) const { return static_cast<size_t>(y * m_size + x); }

  void SetFunction(int x, int y, bool dark)
  {
    m_dark[Index(x, y)] = dark;
    m_function[Index(x, y)] = true;
  }

  void DrawFinder(int x, int y)
  {
    for (int dy = -4; dy <= 4; dy++)
    {
      for (int dx = -4; dx <= 4; dx++)
      {
        const int dist = std::max(std::abs(dx), std::abs(dy));
        const int xx = x + dx;
        const int yy = y + dy;
        if (xx >= 0 && xx < m_size && yy >= 0 && yy < m_size)
          SetFunction(xx, yy, dist != 2 && dist != 4);
      }
    }
  }

  void DrawAlignment(int x, int y)
  {
    for (int dy = -2; dy <= 2; dy++)
      for (int dx = -2; dx <= 2; dx++)
        SetFunction(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
  }

  void DrawVersion()
  {
    if (m_ver < 7)
      return;
    int rem = m_ver;
    for (int i = 0; i < 12; i++)
      rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const long bits = (static_cast<long>(m_ver) << 12) | rem;
    for (int i = 0; i < 18; i++)
    {
      const bool bit = Bit(bits, i);
      const int a = m_size - 11 + i % 3;
      const int b = i / 3;
      SetFunction(a, b, bit);
      SetFunction(b, a, bit);
    }
  }

  std::vector<int> AlignmentPositions() const
  {
    if (m_ver == 1)
      return {};
    const int numAlign = m_ver / 7 + 2;
    const int step = (m_ver * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2) * 2;
    std::vector<int> result;
    for (int i = 0, pos = m_ver * 4 + 10; i < numAlign - 1; i++, pos -= step)
      result.insert(result.begin(), pos);
    result.insert(result.begin(), 6);
    return result;
  }

  int CountFinderLike(const std::array<int, 7>& h) const
  {
    const int n = h[1];
    const bool core = n > 0 && h[2] == n && h[3] == n * 3 && h[4] == n && h[5] == n;
    return (core && h[0] >= n * 4 && h[6] >= n ? 1 : 0) + (core && h[6] >= n * 4 && h[0] >= n ? 1 : 0);
  }

  int TerminateAndCount(bool runColor, int run, std::array<int, 7>& h) const
  {
    if (runColor)
    {
      AddHistory(run, h);
      run = 0;
    }
    run += m_size; // light border after the last run
    AddHistory(run, h);
    return CountFinderLike(h);
  }

  void AddHistory(int run, std::array<int, 7>& h) const
  {
    if (h[0] == 0)
      run += m_size; // light border before the first run
    std::copy_backward(h.begin(), h.end() - 1, h.end());
    h[0] = run;
  }

  int m_ver;
  int m_level;
  int m_size;
  std::vector<bool> m_dark;
  std::vector<bool> m_function;
};
} // namespace

bool FLEXQR::Encode(const std::string& text, Code& out, int forcedMask)
{
  // smallest version with level M, otherwise with level L
  int version = 0;
  int level = 1;
  for (int lv = 1; lv >= 0 && !version; lv--)
  {
    for (int v = 1; v <= MAX_VERSION; v++)
    {
      const size_t needed = 4 + (v <= 9 ? 8 : 16) + 8 * text.size();
      if (needed <= static_cast<size_t>(DataCodewords(v, lv)) * 8)
      {
        version = v;
        level = lv;
        break;
      }
    }
  }
  if (!version)
    return false;

  // data bits: byte mode, length, bytes, terminator, padding
  std::vector<bool> bits;
  const auto append = [&bits](unsigned long value, int count) {
    for (int i = count - 1; i >= 0; i--)
      bits.push_back(((value >> i) & 1) != 0);
  };
  append(0x4, 4);
  append(text.size(), version <= 9 ? 8 : 16);
  for (const char c : text)
    append(static_cast<uint8_t>(c), 8);
  const size_t capacity = static_cast<size_t>(DataCodewords(version, level)) * 8;
  append(0, static_cast<int>(std::min<size_t>(4, capacity - bits.size())));
  append(0, static_cast<int>((8 - bits.size() % 8) % 8));
  for (uint8_t pad = 0xEC; bits.size() < capacity; pad ^= 0xEC ^ 0x11)
    append(pad, 8);
  std::vector<uint8_t> data(bits.size() / 8);
  for (size_t i = 0; i < bits.size(); i++)
    data[i >> 3] |= static_cast<uint8_t>((bits[i] ? 1 : 0) << (7 - (i & 7)));

  // error correction per block, then interleaved
  const int numBlocks = NUM_BLOCKS[level][version];
  const int eccLen = ECC_PER_BLOCK[level][version];
  const int rawCodewords = RawDataModules(version) / 8;
  const int numShort = numBlocks - rawCodewords % numBlocks;
  const int shortLen = rawCodewords / numBlocks;
  const std::vector<uint8_t> divisor = RsDivisor(eccLen);
  std::vector<std::vector<uint8_t>> blocks;
  for (int i = 0, k = 0; i < numBlocks; i++)
  {
    const int len = shortLen - eccLen + (i < numShort ? 0 : 1);
    std::vector<uint8_t> block(data.begin() + k, data.begin() + k + len);
    k += len;
    const std::vector<uint8_t> ecc = RsRemainder(block, divisor);
    if (i < numShort)
      block.push_back(0);
    block.insert(block.end(), ecc.begin(), ecc.end());
    blocks.push_back(std::move(block));
  }
  std::vector<uint8_t> codewords;
  for (size_t i = 0; i < blocks[0].size(); i++)
  {
    for (size_t j = 0; j < blocks.size(); j++)
    {
      if (i != static_cast<size_t>(shortLen - eccLen) || j >= static_cast<size_t>(numShort))
        codewords.push_back(blocks[j][i]);
    }
  }

  CMatrix m(version, level);
  m.DrawFunctionPatterns();
  m.DrawCodewords(codewords);
  int mask = forcedMask;
  if (mask < 0 || mask > 7)
  {
    long best = std::numeric_limits<long>::max();
    for (int i = 0; i < 8; i++)
    {
      m.ApplyMask(i);
      m.DrawFormatBits(i);
      const long penalty = m.Penalty();
      if (penalty < best)
      {
        best = penalty;
        mask = i;
      }
      m.ApplyMask(i); // XOR again: undone
    }
  }
  m.ApplyMask(mask);
  m.DrawFormatBits(mask);

  out.version = version;
  out.size = m.Size();
  out.mask = mask;
  out.level = level == 1 ? 'M' : 'L';
  out.modules.assign(static_cast<size_t>(out.size * out.size), false);
  for (int y = 0; y < out.size; y++)
    for (int x = 0; x < out.size; x++)
      out.modules[static_cast<size_t>(y * out.size + x)] = m.Dark(x, y);
  return true;
}
