/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  Protocol between Kodi (CTVEsEngine) and the helper process native/flex_es_helper.c, which alone
 *  talks to the TV's media service. Plain C: the helper does not use Kodi's C++ runtime.
 *  Same process, same CPU: structures go over the sockets as they are.
 */

#pragma once

#include <stdint.h>

/* Inherited file descriptors of the helper */
#define ES_FD_DATA 3 /* SOCK_STREAM, Kodi -> helper: es_rec + payload */
#define ES_FD_CTL 4 /* SOCK_SEQPACKET: Kodi -> helper es_cmd, helper -> Kodi es_status */

#define ES_MAGIC 0x31534546u

enum es_rec_type
{
  ES_REC_AUDIO = 0, /* values as ST_MMPLAYER_DATA_INFO.type */
  ES_REC_VIDEO = 1,
  ES_REC_EOS = 2 /* no payload: no more data in this generation */
};

/*! Packet on the data socket. Records of an older generation are dropped by the helper. */
struct es_rec
{
  uint32_t type;
  uint32_t size;
  uint32_t gen;
  uint32_t pts_ms;
};

enum es_op
{
  ES_OP_OPEN = 1, /* vcodec, acodec, fps_mhz, gen: start the first media item */
  ES_OP_PAUSE, /* flags: 1 pause, 0 play */
  ES_OP_FLUSH, /* gen: new generation; flags: see ES_FLUSH_* */
  ES_OP_RECT, /* x, y, w, h on the 3840x2160 video plane */
  ES_OP_STOP /* tear down and exit; EOF on the control socket means the same */
};

#define ES_FLUSH_RECREATE 1u /* new media item (with vcodec, acodec, fps_mhz) instead of a flush */

struct es_cmd
{
  uint32_t magic;
  uint32_t op;
  uint32_t gen;
  uint32_t flags;
  uint32_t vcodec; /* ST_MEDIA_INFO +0x50 */
  uint32_t acodec; /* ST_MEDIA_INFO +0xa0, 0 = no audio */
  uint32_t fps_mhz; /* ST_MEDIA_INFO +0x5c */
  int32_t x;
  int32_t y;
  uint32_t w;
  uint32_t h;
};

enum es_state
{
  ES_ST_CONNECT = 0, /* service not reached yet, or no item */
  ES_ST_PRELOAD, /* item exists, data before MM_Play or after a seek */
  ES_ST_PLAYING,
  ES_ST_PAUSED,
  ES_ST_ENDED, /* all data shown */
  ES_ST_ERROR /* err: es_err or the event number of the service */
};

enum es_err
{
  ES_ERR_NONE = 0,
  ES_ERR_LIBS = 1000, /* vendor libraries or symbols missing */
  ES_ERR_ITEM = 1001, /* MM_CreateMediaItem refused: another player holds the service */
  ES_ERR_INIT_TIMEOUT = 1002 /* no E_EVENT_INIT_OK in time */
};

struct es_status
{
  uint32_t magic;
  uint32_t state;
  uint32_t gen; /* generation the helper is pushing */
  uint32_t pts_ms; /* MM_GetVideoPTS */
  uint32_t buf_ms; /* MM_GetVideoDataBufferInfo kind 3 */
  uint32_t err;
  uint32_t n_video;
  uint32_t n_audio;
  uint32_t n_retry; /* pushes repeated because the service refused them */
  uint32_t n_fail; /* packets given up after all retries */
  uint32_t n_decerr; /* E_EVENT_DECODE_ERROR */
};
