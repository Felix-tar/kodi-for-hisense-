/*
 * flex_app_close.c — beendet die im Hintergrund pausierten Apps Netflix und YouTube vor dem Kodi-Start.
 *
 * Wunsch des Besitzers (05.11.2026): bei jedem Kodi-Start alle Hintergrund-Apps wie YouTube und Netflix
 * schließen. Beide liegen nach der Benutzung (und ab dem Einschalten als "preload") pausiert im Speicher
 * (zusammen rund 230 MB RSS bei ~300 MB frei), halten Verbindungen ins Internet offen
 * (docs/tv-analyse/VERBINDUNGEN.md) und sperren nach Launcher-Logik den TV-Player (E1).
 * Hintergrund: docs/kodi/TVPLAYER-DESIGN.md E1, docs/kodi/BUILD.md "Hintergrund-Apps beim Start".
 *
 * Dass die Apps wirklich nur im Hintergrund liegen, garantiert der Aufrufer: kodi.sh ruft den Helfer
 * automatisch nur beim Start über die Kodi-Kachel (flex.js startet Kodi nur von der freien Startseite,
 * homeIdle/noAppInStack) und gibt dann --bg-confirmed mit. Ohne diese Bestätigung beendet der Helfer
 * nichts, wenn der App-Manager (APM) nicht erreichbar ist. Die APM-Abfrage (Fokus, Ressourcen 1/2/8)
 * ist nur eine zusätzliche Bremse: YouTube (Client ist app_wrapper) fordert dort nie Fokus an, sie
 * erkennt also nicht jede App, die vorne ist.
 *
 * Weg: SIGKILL auf den App-Prozess. Netflix ist selbst APM-Client; APM räumt über den Binder-Tod
 * auf (removeClient). Für YouTube ist app_wrapper der Client; er kehrt aus waitpid zurück und meldet
 * sich selbst ab (APM_Leave). Der Launcher bekommt dabei kein AppStatus, also kein Modulwechsel.
 * Nicht APM_RequestToStopService: appmgr hält dabei seine globale Sperre über ein synchrones onDestroy
 * ohne Zeitlimit (statisch belegt, Recherche 06.11.2026); hinge Netflix dort, stünde jede App-Steuerung.
 * Nicht SIGTERM: Cobalt/Netflix könnten "stopped" melden, und der Launcher baut dann je nach Startweg
 * die Startseite um oder öffnet Live-TV (flex.js beendete Kodi).
 * Nie angefasst: eine App, die APM als Fokus- oder Ressourcen-Besitzer führt, ein angehaltener Prozess
 * (Zustand T), alles andere (hisense_browser, Messagestore, nfxdevsplash).
 *
 * Aufruf (root): LD_LIBRARY_PATH=/mslib:/mslib/utopia:/lib flex_app_close [--dry] [--bg-confirmed]
 *   [netflix] [youtube]; ohne App-Namen beide. --dry: nur prüfen und melden (APM-Sicht ausgeben).
 * Ausgabe: je App "<app> <closed|none|skip|fail|probe> <grund>", zum Schluss
 * "result <ok|none|partial|fail> <geschlossene>". Beendet sich spätestens nach 12 s (flex.js wartet ab
 * der Kodi-Anforderung 25 s auf das erste Lebenszeichen, KODI_START_MS).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Prototypen aus den __PRETTY_FUNCTION__-Texten in libAPM.so */
static struct {
    int (*connect)(void);
    int (*status)(const char *);
    int (*readapp)(void *, const char *);
    int (*focus)(char *, uint32_t);
    int (*resource)(int, char *, uint32_t);
} apm;
static int apm_ok, dry, confirmed;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void apm_load(void)
{
    void *h = dlopen("/mslib/libAPM.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        say("# libAPM: %s", dlerror());
        return;
    }
    apm.connect = (int (*)(void))dlsym(h, "APM_Connect");
    apm.status = (int (*)(const char *))dlsym(h, "APM_RequestStatusOfApplication");
    apm.readapp = (int (*)(void *, const char *))dlsym(h, "APM_RequestToReadAppByName");
    apm.focus = (int (*)(char *, uint32_t))dlsym(h, "APM_GetFocusApp");
    apm.resource = (int (*)(int, char *, uint32_t))dlsym(h, "APM_GetResourceApp");
    if (!apm.connect || !apm.status || !apm.readapp || !apm.focus || !apm.resource) {
        say("# libAPM: Symbole fehlen");
        return;
    }
    int rc = apm.connect();
    if (rc != 0) {
        say("# APM_Connect rc=%d", rc);
        return;
    }
    apm_ok = 1;
}

/* ---- /proc ---- */
static int read_file(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}

/* Zustandsbuchstabe aus /proc/<pid>/stat, 0 = Prozess weg */
static char proc_state(pid_t pid)
{
    char p[64], b[512];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    if (read_file(p, b, sizeof b) <= 0) return 0;
    char *e = strrchr(b, ')');
    return (e && e[1] == ' ') ? e[2] : 0;
}

/* lebt noch (ein Zombie zählt als beendet: ihn räumt der Elternprozess ab) */
static int alive(pid_t pid)
{
    char st = proc_state(pid);
    return st && st != 'Z' && st != 'X';
}

/* argv[0] von pid enthält want (read_file liefert die NUL-getrennte Zeile, strstr sieht nur argv[0]) */
static int arg0_has(pid_t pid, const char *want)
{
    char p[64], b[512];
    snprintf(p, sizeof p, "/proc/%d/cmdline", (int)pid);
    if (read_file(p, b, sizeof b) <= 0) return 0;
    return strstr(b, want) != NULL;
}

static int find_procs(const char *want, pid_t *out, int max)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid != getpid() && arg0_has(pid, want) && alive(pid)) out[n++] = pid;
    }
    closedir(d);
    return n;
}

/* ---- APM ---- */
/* Führt APM die App als Fokus- oder Ressourcen-Besitzer? APM_RESOURCE_e ist eine Bitmaske; 1, 2 und 8
 * sind die einzeln abfragbaren (0, 4 und -1 lehnt appmgr ab). */
static const char *apm_front(const char *name)
{
    static const int res[] = {1, 2, 8};
    char b[64];
    memset(b, 0, sizeof b);
    if (apm.focus(b, sizeof b - 1) == 0 && b[0] && strcmp(b, name) == 0) return "hat den Fokus";
    for (unsigned i = 0; i < sizeof res / sizeof res[0]; i++) {
        memset(b, 0, sizeof b);
        if (apm.resource(res[i], b, sizeof b - 1) == 0 && b[0] && strcmp(b, name) == 0) return "hat eine Ressource";
    }
    return NULL;
}

/* pid laut APM: APM_RequestToReadAppByName liefert die Zahl der kopierten Bytes (>= 0x70, Datensatz
 * mit pid bei +0x1c), bei Fehlern <= 0 und lässt den Puffer dann unberührt */
static pid_t apm_pid(const char *name)
{
    static unsigned char buf[8192];
    memset(buf, 0, sizeof buf);
    int rc = apm.readapp(buf, name);
    if (rc < 0x70 || rc > (int)sizeof buf) return 0;
    int32_t pid;
    memcpy(&pid, buf + 0x1c, sizeof pid);
    return pid > 1 ? (pid_t)pid : 0;
}

/* Darf die App beendet werden? NULL = ja, sonst der Grund */
static const char *blocked(const char *const *names, int n)
{
    if (!apm_ok) return confirmed ? NULL : "APM nicht erreichbar, Hintergrund nicht bestätigt";
    for (int i = 0; i < n; i++) {
        const char *why = apm_front(names[i]);
        if (why) return why;
    }
    return NULL;
}

static void apm_dump(const char *const *names, int n)
{
    if (!apm_ok) return;
    char f[64];
    memset(f, 0, sizeof f);
    int rc = apm.focus(f, sizeof f - 1);
    say("# APM Fokus rc=%d '%s'", rc, f);
    for (int i = 0; i < n; i++)
        say("# APM %s: Status %d, pid %d", names[i], apm.status(names[i]), (int)apm_pid(names[i]));
}

static int wait_gone(const pid_t *pids, int n, int ms)
{
    for (int t = 0; t <= ms; t += 50) {
        int left = 0;
        for (int i = 0; i < n; i++)
            if (alive(pids[i])) left++;
        if (!left) return 1;
        usleep(50000);
    }
    return 0;
}

/* gemeinsamer Ablauf: Prozesse suchen, prüfen, SIGKILL, warten */
static int close_app(const char *app, const pid_t *p, int m, const char *const *names, int nn)
{
    if (!m) {
        say("%s none läuft nicht", app);
        return 0;
    }
    for (int i = 0; i < m; i++) {
        char st = proc_state(p[i]);
        if (st == 'T' || st == 't') {
            say("%s skip angehalten (Zustand %c)", app, st);
            return -1;
        }
    }
    const char *why = blocked(names, nn);
    if (dry) {
        apm_dump(names, nn);
        say("%s probe %d Prozess(e), %s", app, m, why ? why : "würde beendet");
        return 0;
    }
    if (why) {
        say("%s skip %s", app, why);
        return -1;
    }
    for (int i = 0; i < m; i++) kill(p[i], SIGKILL);
    if (!wait_gone(p, m, 2000)) {
        say("%s fail Prozess lebt noch", app);
        return -1;
    }
    say("%s closed SIGKILL (%d Prozess(e))", app, m);
    return 1;
}

static int close_netflix(void)
{
    static const char *const names[] = {"Netflix_Daemon", "Netflix"};
    pid_t p[4];
    /* argv[0] /applications/Netflix/bin/netflix (nicht nfxdevsplash, nicht NetflixLauncher) */
    int m = find_procs("/applications/Netflix/bin/netflix", p, 4);
    return close_app("netflix", p, m, names, 2);
}

static int close_youtube(void)
{
    static const char *const names[] = {"youtube_preload", "youtube", "youtube_kids"};
    pid_t p[8], q[8];
    /* Cobalt selbst (argv[0] "cobalt" bzw. .../cobalt/bin/cobalt, auch cobalt_kids), nicht app_wrapper */
    int n = find_procs("cobalt", p, 8), m = 0;
    for (int i = 0; i < n; i++)
        if (!arg0_has(p[i], "app_wrapper")) q[m++] = p[i];
    return close_app("youtube", q, m, names, 3);
}

int main(int argc, char **argv)
{
    int want_n = 0, want_y = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dry")) dry = 1;
        else if (!strcmp(argv[i], "--bg-confirmed")) confirmed = 1;
        else if (!strcmp(argv[i], "netflix")) want_n = 1;
        else if (!strcmp(argv[i], "youtube")) want_y = 1;
        else {
            fprintf(stderr, "Aufruf: %s [--dry] [--bg-confirmed] [netflix] [youtube]\n", argv[0]);
            return 2;
        }
    }
    if (!want_n && !want_y) want_n = want_y = 1;
    if (getuid() != 0) {
        say("result fail nicht root");
        return 0;
    }
    alarm(12);   /* nie den Kodi-Start aufhalten */
    apm_load();

    int closed = 0, failed = 0;
    char list[32] = "";
    /* YouTube zuerst: rein lokal (SIGKILL), kann nicht an APM hängen */
    if (want_y) {
        int r = close_youtube();
        if (r > 0) { closed++; strcat(list, "youtube"); }
        if (r < 0) failed++;
    }
    if (want_n) {
        int r = close_netflix();
        if (r > 0) { closed++; strcat(list, list[0] ? ",netflix" : "netflix"); }
        if (r < 0) failed++;
    }
    say("result %s %s", failed ? (closed ? "partial" : "fail") : (closed ? "ok" : "none"), list[0] ? list : "-");
    return 0;
}
