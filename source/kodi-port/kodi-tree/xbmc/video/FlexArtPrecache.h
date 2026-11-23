/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

/*!
 * \brief Fill the texture cache with the library's posters and fanart (FLEX-TV, 18.11.2026).
 *
 * Kodi caches an image only when it is shown first. The library's art comes as full-size images
 * from the internet (TMDB) or the network share, so paging through "Movies A-Z" waited for every
 * uncached poster. Once per Kodi start, this queues the posters and fanart of movies, sets, TV
 * shows and seasons with CTextureCache::BackgroundCacheImage: images already cached are skipped
 * there, the rest are loaded one after the other at low priority, paused while a video plays.
 */
namespace FLEXART
{
//! the home window opens (first time per run); later calls do nothing
void StartPrecache();
} // namespace FLEXART
