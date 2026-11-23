/*
 * flex_es_helper.c — Helfer-Prozess zwischen Kodi und dem TV-Mediendienst (ES-Push, echte Bildrate).
 *
 * Nur dieser Prozess laedt die Herstellerbibliotheken: Sie haengen zirkulaer voneinander ab und brauchen
 * LD_PRELOAD, und Kodi bleibt so frei von ihrer C++-Laufzeit. Stirbt Kodi, sieht der Helfer das Ende des
 * Steuer-Sockets und baut das Media-Item sauber ab. Der Dienst startet nach einem Absturz nicht neu.
 * Hintergrund: docs/kodi/MULTIDECODE-RECHERCHE.md §7g und §8. Protokoll: TVEsProtocol.h.
 *
 * Aufruf (Dateideskriptoren vom Elternprozess): flex_es_helper, 3 = Daten, 4 = Steuerung.
 * Umgebung: LD_LIBRARY_PATH und LD_PRELOAD wie tools/es_run.sh, dazu
 *   FLEX_ES_TARGET_MS=<ms>   so viel Film im Puffer des Dienstes halten (Standard 1500)
 *   FLEX_ES_PREROLL_MS=<ms>  Vorlauf vor MM_Play (Standard 1200)
 *
 * Regeln aus den Tests am TV:
 * - MM_Play nie vor E_EVENT_INIT_OK: mstplayer bricht sonst ab (CHECK), und der Dienst stirbt.
 * - MM_PushPayload ohne Zusatzdaten: Die Bilddaten laufen ueber info.addr (Shared Memory). Zusatzdaten
 *   gehen durch Binder und scheitern ab rund 1,04 MB still (rc bleibt 0).
 * - Ein Push kann im Dienst warten: MM_Stop kommt darum nie aus dem Push-Thread.
 */
#define _GNU_SOURCE
#include "TVEsProtocol.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef unsigned char (*event_cb)(void *, void *, void *);

static struct {
    void (*ctor)(void *);
    int (*init)(void *);
    int (*addcb)(void *, event_cb);
    int (*delcb)(void *, event_cb);
    int (*create)(void *, const void *, unsigned long *);
    int (*play)(void *, unsigned long);
    int (*stop)(void *, unsigned long);
    int (*del)(void *, unsigned long);
    int (*fin)(void *);
    int (*push)(void *, unsigned long, void *, void *, unsigned long);
    int (*trick)(void *, unsigned long, const void *);
    int (*flush)(void *, unsigned long, int);
    int (*dataend)(void *, unsigned long);
    int (*rect)(void *, unsigned long, const void *, unsigned char, unsigned char);
    unsigned long (*vpts)(void *);
    int (*bufinfo)(void *, unsigned long, int, unsigned long *);
    int (*aout)(void *);
    unsigned char (*getvol)(void *);  /* optional: nur fuers Log */
    unsigned char (*getmute)(void *);
    int (*setmute)(void *, unsigned char);
} mm;

static const struct { const char *name; void **slot; } syms[] = {
    {"_ZN23MSrv_MM_PlayerInterfaceC1Ev", (void **)&mm.ctor},
    {"_ZN23MSrv_MM_PlayerInterface13MM_InitializeEv", (void **)&mm.init},
    {"_ZN23MSrv_MM_PlayerInterface17AddEventRecipientEPFhPvS0_S0_E", (void **)&mm.addcb},
    {"_ZN23MSrv_MM_PlayerInterface20RemoveEventRecipientEPFhPvS0_S0_E", (void **)&mm.delcb},
    {"_ZN23MSrv_MM_PlayerInterface18MM_CreateMediaItemERK13ST_MEDIA_INFOPm", (void **)&mm.create},
    {"_ZN23MSrv_MM_PlayerInterface7MM_PlayEm", (void **)&mm.play},
    {"_ZN23MSrv_MM_PlayerInterface7MM_StopEm", (void **)&mm.stop},
    {"_ZN23MSrv_MM_PlayerInterface18MM_DeleteMediaItemEm", (void **)&mm.del},
    {"_ZN23MSrv_MM_PlayerInterface11MM_FinalizeEv", (void **)&mm.fin},
    {"_ZN23MSrv_MM_PlayerInterface14MM_PushPayloadEmP21ST_MMPLAYER_DATA_INFOPvm", (void **)&mm.push},
    {"_ZN23MSrv_MM_PlayerInterface15MM_SetTrickModeEmRK13ST_TRICK_INFO", (void **)&mm.trick},
    {"_ZN23MSrv_MM_PlayerInterface15MM_SetFlushDataEm12EN_DATA_TYPE", (void **)&mm.flush},
    {"_ZN23MSrv_MM_PlayerInterface13MM_SetDataEndEm", (void **)&mm.dataend},
    {"_ZN23MSrv_MM_PlayerInterface22MM_SetDisplayRectangleEmRK23ST_RECT_SIGNED_POSITIONhh", (void **)&mm.rect},
    {"_ZN23MSrv_MM_PlayerInterface14MM_GetVideoPTSEv", (void **)&mm.vpts},
    {"_ZN23MSrv_MM_PlayerInterface25MM_GetVideoDataBufferInfoEm27EN_PLAYER_VIDEO_BUFFER_INFOPm", (void **)&mm.bufinfo},
    {"MI_AOUT_Init", (void **)&mm.aout},
};

/* Lautstaerke und Stumm des Dienstes selbst (nicht des TVs): nur ins Log, falls vorhanden. */
static const struct { const char *name; void **slot; } optional_syms[] = {
    {"_ZN23MSrv_MM_PlayerInterface17MM_GetAudioVolumeEv", (void **)&mm.getvol},
    {"_ZN23MSrv_MM_PlayerInterface19MM_GetAudioMuteFlagEv", (void **)&mm.getmute},
    {"_ZN23MSrv_MM_PlayerInterface15MM_SetAudioMuteEh", (void **)&mm.setmute},
};

/* ST_MMPLAYER_DATA_INFO, wie libstplayer::pushVideoSamples/pushAudioSamples es fuellen. */
struct data_info {
    void *addr;
    uint32_t size;
    uint32_t pts;
    uint32_t type;
};

/* ST_RECT_SIGNED_POSITION, wie libstplayer::setDisplayRectangle es fuellt. */
struct rect_info {
    int32_t x, y;
    uint16_t w, h;
};

/* EN_NOTIFY_EVENT: Nummern aus der Namenstabelle von libstplayer. */
enum {
    EV_INIT_OK = 3,
    EV_INIT_FAIL = 4,
    EV_DECODE_ERROR = 8,
    EV_DECODE_DONE = 9,
    EV_AUDIO_UNSUPPORTED = 17,
    EV_NOT_SUPPORT = 18,
    EV_PLAYING_OK = 19,
    EV_ES_FLUSH_DATA_RDY = 24,
};

#define MEDIA_INFO_SIZE 0x1b0
#define INIT_TIMEOUT_S 10.0
#define FLUSH_TIMEOUT_S 3.0
#define END_STALL_S 3.0 /* Position steht und Puffer leer: Filmende, auch ohne DECODE_DONE */
#define PUSH_RETRY_S 2.0

static double t0;
static void *obj;
static volatile sig_atomic_t stop_flag;

static pthread_mutex_t svc_mu = PTHREAD_MUTEX_INITIALIZER; /* Abfragen gegen Anlegen/Loeschen des Items */
static pthread_mutex_t cfg_mu = PTHREAD_MUTEX_INITIALIZER;

static struct es_cmd cfg; /* letztes OPEN bzw. FLUSH mit neuem Item */
static volatile int have_open;
static volatile int flush_req;
static struct es_cmd flush_cmd;

static volatile unsigned long item; /* 0 = keines */
static volatile int item_stopped;
static volatile int ev_init_ok, ev_playing_ok, ev_flush_rdy, ev_done;
static volatile int play_thread_running, play_called, play_returned;
static volatile int want_pause, is_paused;
static volatile int have_rect;
static struct rect_info rect;

static volatile uint32_t st_state = ES_ST_CONNECT, st_gen, st_pts, st_buf, st_err;
static volatile uint32_t st_video, st_audio, st_retry, st_fail, st_decerr;

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define LOG(...)                                                          \
    do {                                                                  \
        char lb_[512];                                                    \
        int ln_ = snprintf(lb_, sizeof lb_, "[%8.3f] ", now() - t0);      \
        snprintf(lb_ + ln_, sizeof lb_ - ln_, __VA_ARGS__);               \
        fprintf(stderr, "%s\n", lb_);                                     \
    } while (0)

static void on_signal(int sig) {
    (void)sig;
    stop_flag = 1;
}

static void send_status(void) {
    struct es_status s = {ES_MAGIC, st_state, st_gen, st_pts, st_buf, st_err,
                          st_video, st_audio, st_retry, st_fail, st_decerr};
    /* Kodi weg: Das sieht der Steuer-Thread am Socket-Ende. */
    (void)!send(ES_FD_CTL, &s, sizeof s, MSG_NOSIGNAL | MSG_DONTWAIT);
}

static void fail(uint32_t err, const char *why) {
    LOG("Fehler %u: %s", err, why);
    st_err = err;
    st_state = ES_ST_ERROR;
    send_status();
}

static unsigned char on_event(void *a, void *ev, void *data) {
    const unsigned long *d = data;
    unsigned long e = (unsigned long)ev;
    (void)a;
    /* Meldungen eines schon geloeschten Items zaehlen nicht. */
    if (d && item && d[0] != item) {
        LOG("Ereignis %lu fuer fremdes Item", e);
        return 1;
    }
    switch (e) {
    case EV_INIT_OK: ev_init_ok = 1; break;
    case EV_PLAYING_OK: ev_playing_ok = 1; break;
    case EV_ES_FLUSH_DATA_RDY: ev_flush_rdy = 1; break;
    case EV_DECODE_DONE: ev_done = 1; break;
    case EV_DECODE_ERROR: st_decerr++; break;
    case EV_INIT_FAIL:
    case EV_NOT_SUPPORT:
    case EV_AUDIO_UNSUPPORTED:
        fail((uint32_t)e, "Dienst lehnt den Strom ab");
        break;
    default: break;
    }
    if (e != 0 && e != EV_DECODE_ERROR)
        LOG("Ereignis %lu", e);
    return 1;
}

static void put32(unsigned char *p, unsigned off, uint32_t v) {
    memcpy(p + off, &v, 4);
}

/* Wie libstplayer::PlayerImp::createMedia (0x8bcc); Bedeutungen siehe MULTIDECODE-RECHERCHE §7e. */
static void fill_media_info(unsigned char *mi, const struct es_cmd *c) {
    memset(mi, 0, MEDIA_INFO_SIZE);
    put32(mi, 0x00, 0x00010012);
    put32(mi, 0x04, 0x55aa5a5a);
    put32(mi, 0x08, MEDIA_INFO_SIZE);
    put32(mi, 0x10, 1);
    put32(mi, 0x20, 0xffffffff);
    put32(mi, 0x24, 0xffffffff);
    put32(mi, 0x28, 2);
    put32(mi, 0x2c, 3);
    put32(mi, 0x48, 0x802);
    put32(mi, 0x50, c->vcodec);
    put32(mi, 0x5c, c->fps_mhz);
    mi[0x62] = 1;
    mi[0x64] = 4;
    mi[0x65] = 1;
    put32(mi, 0xa0, c->acodec);
    put32(mi, 0xa4, 1);
    put32(mi, 0xa8, 9);
}

static void apply_rect(void) {
    if (!have_rect || !item || !play_returned)
        return;
    pthread_mutex_lock(&cfg_mu);
    struct rect_info r = rect;
    pthread_mutex_unlock(&cfg_mu);
    LOG("MM_SetDisplayRectangle %d,%d %ux%u -> %d", r.x, r.y, r.w, r.h, mm.rect(obj, item, &r, 0, 0));
}

static void apply_pause(void) {
    if (!item || !ev_playing_ok || want_pause == is_paused)
        return;
    static const uint32_t normal[2] = {0, 1}, pause[2] = {1, 1}; /* wie libstplayer */
    int p = want_pause;
    LOG("MM_SetTrickMode(%s) -> %d", p ? "PAUSE" : "NORMAL", mm.trick(obj, item, p ? pause : normal));
    is_paused = p;
    st_state = p ? ES_ST_PAUSED : ES_ST_PLAYING;
    send_status();
}

/* MM_Stop genau einmal je Item, nie waehrend MM_Play im Dienst liegt. Aus jedem Thread ausser dem Push-Thread
 * erlaubt, solange dieser pusht; er selbst ruft es nur, wenn er nicht pusht. */
static void stop_item_once(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (item && !item_stopped) {
        for (int i = 0; i < 200 && play_thread_running; i++)
            usleep(10000);
        item_stopped = 1;
        LOG("MM_Stop -> %d", mm.stop(obj, item));
    }
    pthread_mutex_unlock(&mu);
}

static void *play_thread(void *arg) {
    (void)arg;
    LOG("MM_Play -> %d", mm.play(obj, item));
    play_returned = 1;
    play_thread_running = 0;
    return NULL;
}

static void start_play(void) {
    pthread_t t;
    play_called = 1;
    play_thread_running = 1;
    if (pthread_create(&t, NULL, play_thread, NULL) == 0) {
        pthread_detach(t);
    } else {
        play_thread_running = 0;
        fail(ES_ERR_INIT_TIMEOUT, "kein Thread fuer MM_Play");
    }
}

/* Steuerung von Kodi. Kommt das Socket-Ende, ist Kodi weg: abbauen. */
static void *ctl_thread(void *arg) {
    (void)arg;
    while (!stop_flag) {
        struct pollfd p = {ES_FD_CTL, POLLIN, 0};
        int r = poll(&p, 1, 100);
        if (r == 0 || (r < 0 && errno == EINTR))
            continue;
        struct es_cmd c;
        ssize_t n = recv(ES_FD_CTL, &c, sizeof c, 0);
        if (n <= 0) {
            LOG("Steuer-Socket zu: Kodi weg");
            stop_flag = 1;
            break;
        }
        if (n != sizeof c || c.magic != ES_MAGIC)
            continue;
        switch (c.op) {
        case ES_OP_OPEN:
            pthread_mutex_lock(&cfg_mu);
            cfg = c;
            pthread_mutex_unlock(&cfg_mu);
            have_open = 1;
            break;
        case ES_OP_PAUSE:
            want_pause = c.flags != 0;
            apply_pause();
            break;
        case ES_OP_RECT:
            pthread_mutex_lock(&cfg_mu);
            rect.x = c.x;
            rect.y = c.y;
            rect.w = (uint16_t)c.w;
            rect.h = (uint16_t)c.h;
            pthread_mutex_unlock(&cfg_mu);
            have_rect = 1;
            apply_rect();
            break;
        case ES_OP_FLUSH:
            pthread_mutex_lock(&cfg_mu);
            flush_cmd = c;
            pthread_mutex_unlock(&cfg_mu);
            /* neues Item: Ein Push, der im Dienst wartet, kommt erst nach MM_Stop zurueck. */
            if (c.flags & ES_FLUSH_RECREATE)
                stop_item_once();
            flush_req = 1;
            break;
        case ES_OP_STOP:
            stop_flag = 1;
            break;
        default:
            break;
        }
    }
    stop_item_once();
    return NULL;
}

/* Position und Pufferstand. Keine Dienst-Aufrufe, solange MM_Play im Dienst liegt: sie wuerden blockieren. */
static void *status_thread(void *arg) {
    (void)arg;
    unsigned n = 0;
    while (!stop_flag) {
        usleep(50000);
        if (n % 200 == 199) /* alle 10 s: kommt Ton an, verliert der Dienst Pakete? */
            LOG("Stand %u: Position %u ms, Puffer %u ms, Video %u, Ton %u, wiederholt %u, verloren %u, Dekodierfehler %u",
                st_state, st_pts, st_buf, st_video, st_audio, st_retry, st_fail, st_decerr);
        pthread_mutex_lock(&svc_mu);
        if (item && !item_stopped && play_returned) {
            unsigned long ms = 0;
            if (mm.bufinfo(obj, item, 3, &ms) == 0)
                st_buf = (uint32_t)ms;
            st_pts = (uint32_t)mm.vpts(obj);
        }
        pthread_mutex_unlock(&svc_mu);
        if (n++ % 4 == 0)
            send_status();
    }
    return NULL;
}

/* Liest genau len Bytes. 1 = da, 0 = Abbruch, -1 = Socket zu. */
static int read_all(void *buf, size_t len) {
    unsigned char *p = buf;
    while (len && !stop_flag) {
        ssize_t n = read(ES_FD_DATA, p, len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
        } else if (n == 0) {
            return -1;
        } else if (errno == EAGAIN || errno == EINTR) {
            struct pollfd pf = {ES_FD_DATA, POLLIN, 0};
            poll(&pf, 1, 100);
        } else {
            return -1;
        }
    }
    return len == 0;
}

static int create_item(const struct es_cmd *c) {
    static unsigned char mi[MEDIA_INFO_SIZE];
    unsigned long it = 0;
    fill_media_info(mi, c);
    int rc = mm.create(obj, mi, &it);
    LOG("MM_CreateMediaItem v=%#x a=%u fps=%u -> %d, item=%#lx", c->vcodec, c->acodec, c->fps_mhz, rc, it);
    if (!it)
        return 0;
    pthread_mutex_lock(&svc_mu);
    ev_init_ok = ev_playing_ok = ev_flush_rdy = ev_done = 0;
    play_called = play_returned = 0;
    item_stopped = 0;
    is_paused = 0;
    item = it;
    pthread_mutex_unlock(&svc_mu);
    return 1;
}

static void destroy_item(void) {
    if (!item)
        return;
    stop_item_once();
    pthread_mutex_lock(&svc_mu);
    unsigned long it = item;
    LOG("MM_DeleteMediaItem -> %d", mm.del(obj, it));
    item = 0;
    pthread_mutex_unlock(&svc_mu);
}

/* Ein Paket schieben. Abgelehnt, etwa bei vollem ES-Puffer: wie libstplayer ("sample retry") wiederholen. */
static void push_packet(void *data, const struct es_rec *r) {
    struct data_info di = {data, r->size, r->pts_ms, r->type};
    double until = now() + PUSH_RETRY_S;
    int rc = mm.push(obj, item, &di, NULL, 0);
    while (rc != 0 && !stop_flag && !flush_req && now() < until) {
        st_retry++;
        usleep(5000);
        rc = mm.push(obj, item, &di, NULL, 0);
    }
    if (rc != 0)
        st_fail++;
    else if (r->type == ES_REC_VIDEO)
        st_video++;
    else
        st_audio++;
}

enum session_end { SESSION_STOP, SESSION_RECREATE };

/* Die Daten einer Sitzung schieben; bleibt bis Stopp oder neues Item. */
static enum session_end run_session(unsigned char **data, size_t *cap, long target_ms, long preroll_ms) {
    struct es_rec r;
    int have = 0, eos = 0;
    long first_pts = -1, last_video_pts = -1;
    double since = now(), end_since = 0;
    uint32_t end_pts = 0;
    uint32_t gen = st_gen;
    st_state = ES_ST_PRELOAD;
    send_status();

    while (!stop_flag) {
        if (flush_req) {
            pthread_mutex_lock(&cfg_mu);
            struct es_cmd c = flush_cmd;
            flush_req = 0;
            pthread_mutex_unlock(&cfg_mu);
            gen = c.gen;
            st_gen = gen;
            st_pts = 0;
            have = 0;
            eos = 0;
            if (c.flags & ES_FLUSH_RECREATE) {
                pthread_mutex_lock(&cfg_mu);
                cfg = c;
                pthread_mutex_unlock(&cfg_mu);
                return SESSION_RECREATE;
            }
            /* Daten im Dienst verwerfen, das Item bleibt (libstplayer: Art 2 = Ton und Video). */
            ev_flush_rdy = 0;
            LOG("MM_SetFlushData -> %d", mm.flush(obj, item, 2));
            for (double u = now() + FLUSH_TIMEOUT_S; !ev_flush_rdy && !stop_flag && now() < u;)
                usleep(5000);
            LOG("Flush %s", ev_flush_rdy ? "bestaetigt" : "ohne Bestaetigung");
            first_pts = -1;
            last_video_pts = -1;
        }

        if (st_state == ES_ST_ERROR) {
            usleep(50000);
            continue; /* Kodi entscheidet */
        }

        if (!ev_init_ok && !play_called && now() - since > INIT_TIMEOUT_S) {
            fail(ES_ERR_INIT_TIMEOUT, "kein INIT_OK");
            continue;
        }
        if (ev_init_ok && !play_called) {
            LOG("INIT_OK nach %.3f s", now() - since);
            start_play();
        }
        if (ev_playing_ok && st_state == ES_ST_PRELOAD) {
            if (mm.getvol && mm.getmute)
                LOG("Dienst: Lautstaerke %u, stumm %u", mm.getvol(obj), mm.getmute(obj));
            if (getenv("FLEX_ES_UNMUTE") && mm.setmute)
                LOG("Probe: MM_SetAudioMute(0) -> %d", mm.setmute(obj, 0));
            st_state = ES_ST_PLAYING;
            send_status();
            apply_rect();
            apply_pause();
        }

        if (eos) {
            /* Filmende: DECODE_DONE, oder Position steht bei leerem Puffer. */
            if (st_pts != end_pts) {
                end_pts = st_pts;
                end_since = now();
            }
            if (st_state != ES_ST_ENDED && (ev_done || (!is_paused && now() - end_since > END_STALL_S))) {
                LOG("Filmende (%s)", ev_done ? "DECODE_DONE" : "Position steht");
                st_state = ES_ST_ENDED;
                send_status();
            }
            usleep(50000);
            continue;
        }

        if (!have) {
            struct pollfd pf = {ES_FD_DATA, POLLIN, 0};
            if (poll(&pf, 1, 50) <= 0)
                continue;
            int rd = read_all(&r, sizeof r);
            if (rd <= 0) {
                if (rd < 0) {
                    LOG("Daten-Socket zu");
                    stop_flag = 1;
                }
                continue;
            }
            if (r.size > *cap) {
                unsigned char *grown = realloc(*data, r.size);
                if (!grown) {
                    LOG("kein Speicher fuer Paket mit %u Bytes", r.size);
                    stop_flag = 1;
                    continue;
                }
                *data = grown;
                *cap = r.size;
            }
            if (r.size && read_all(*data, r.size) <= 0)
                continue;
            have = 1;
        }
        if (r.gen != gen) { /* Rest vor einem Sprung */
            have = 0;
            continue;
        }
        if (r.type == ES_REC_EOS) {
            LOG("Daten zu Ende, MM_SetDataEnd -> %d", mm.dataend(obj, item));
            have = 0;
            eos = 1;
            end_since = now();
            end_pts = st_pts;
            continue;
        }
        if (first_pts < 0 && r.type == ES_REC_VIDEO)
            first_pts = r.pts_ms;

        if (!play_called) {
            /* vor MM_Play nur der Vorlauf */
            if (first_pts >= 0 && (long)r.pts_ms - first_pts > preroll_ms) {
                usleep(5000);
                continue;
            }
        } else if (r.type == ES_REC_VIDEO && first_pts >= 0) {
            /* Abstand letztes Bild zur Position. kind 3 springt zu grob (am TV 1,5 bis 3 s bei 1,5 s Ziel) und zaehlt
             * nur als Rueckfall, solange die Position noch aus der Zeit vor einem Sprung stammt. */
            long pos = (long)st_pts;
            long lead = pos >= first_pts ? last_video_pts - pos : (long)st_buf;
            if (lead >= target_ms) {
                usleep(5000);
                continue;
            }
        }

        push_packet(*data, &r);
        if (r.type == ES_REC_VIDEO && (long)r.pts_ms > last_video_pts)
            last_video_pts = r.pts_ms;
        have = 0;
    }
    return SESSION_STOP;
}

int main(void) {
    t0 = now();
    setvbuf(stderr, NULL, _IOLBF, 0);
    /* Kodi vererbt Sockets, Dateien und den Griff auf die Fernbedienung: Nichts davon darf hier ueberleben. */
    for (int fd = ES_FD_CTL + 1; fd < 1024; fd++)
        close(fd);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    long target_ms = getenv("FLEX_ES_TARGET_MS") ? atol(getenv("FLEX_ES_TARGET_MS")) : 1500;
    long preroll_ms = getenv("FLEX_ES_PREROLL_MS") ? atol(getenv("FLEX_ES_PREROLL_MS")) : 1200;

    pthread_t ctl, st;
    pthread_create(&ctl, NULL, ctl_thread, NULL);
    send_status();

    void *lib = NULL;
    static const char *const libs[] = {"libdrvIPAUTH.so", "libmsrv_mmplayer_ipc.so"};
    for (size_t i = 0; i < sizeof libs / sizeof libs[0]; i++) {
        lib = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
        if (!lib) {
            LOG("dlopen: %s", dlerror());
            fail(ES_ERR_LIBS, "Bibliothek fehlt");
            goto wait_stop;
        }
    }
    for (size_t i = 0; i < sizeof optional_syms / sizeof optional_syms[0]; i++)
        *optional_syms[i].slot = dlsym(lib, optional_syms[i].name);
    for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++) {
        *syms[i].slot = dlsym(lib, syms[i].name);
        if (!*syms[i].slot) {
            LOG("Symbol fehlt: %s", syms[i].name);
            fail(ES_ERR_LIBS, "Symbol fehlt");
            goto wait_stop;
        }
    }

    {
        /* MI_AOUT_InitParams: u16SizeofStructure an +0, sonst "unknown error". */
        static unsigned char aout_params[256];
        aout_params[0] = sizeof aout_params & 0xff;
        aout_params[1] = sizeof aout_params >> 8;
        LOG("MI_AOUT_Init -> %d", mm.aout(aout_params));

        obj = calloc(1, 4096); /* mm_test: operator new(0x40); grosszuegig */
        mm.ctor(obj);
        LOG("MM_Initialize -> %d", mm.init(obj));
        LOG("AddEventRecipient -> %d", mm.addcb(obj, on_event));
        pthread_create(&st, NULL, status_thread, NULL);

        unsigned char *data = NULL;
        size_t cap = 0;
        while (!stop_flag && !have_open)
            usleep(10000);
        while (!stop_flag) {
            pthread_mutex_lock(&cfg_mu);
            struct es_cmd c = cfg;
            pthread_mutex_unlock(&cfg_mu);
            st_gen = c.gen;
            if (!create_item(&c)) {
                fail(ES_ERR_ITEM, "kein Media-Item");
                while (!stop_flag)
                    usleep(50000);
                break;
            }
            enum session_end end = run_session(&data, &cap, target_ms, preroll_ms);
            destroy_item();
            if (end == SESSION_STOP)
                break;
        }
        stop_flag = 1;
        pthread_join(st, NULL);
        pthread_join(ctl, NULL);
        destroy_item();
        LOG("RemoveEventRecipient -> %d", mm.delcb(obj, on_event));
        LOG("MM_Finalize -> %d", mm.fin(obj));
        return 0;
    }

wait_stop:
    pthread_join(ctl, NULL);
    return 1;
}
