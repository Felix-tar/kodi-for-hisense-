/*
 * Sichttest: auf welcher DirectFB-Ebene ist ein Fenster über dem Launcher zu sehen? Nacheinander
 * je 4 s ein Vollbild-Fenster, nur mit DirectFB gefüllt (ohne EGL):
 *   Ebene 0 = BLAU, Ebene 1 = GRÜN, Ebene 2 = ROT
 * Danach wird jedes Fenster wieder entfernt; an den Ebenen selbst wird nichts eingestellt.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <directfb.h>

int main(int argc, char **argv)
{
    static const unsigned char col[3][3] = {{0x20, 0x40, 0xe0}, {0x20, 0xc0, 0x40}, {0xe0, 0x20, 0x20}};
    static const char *name[3] = {"BLAU", "GRUEN", "ROT"};
    IDirectFB *dfb = NULL;
    int id;

    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(40);
    if (DirectFBInit(&argc, &argv) != DFB_OK || DirectFBCreate(&dfb) != DFB_OK) return 1;
    for (id = 0; id < 3; id++) {
        IDirectFBDisplayLayer *l = NULL;
        IDirectFBWindow *w = NULL;
        IDirectFBSurface *s = NULL;
        union { DFBWindowDescription d; unsigned char spare[256]; } wd; /* MStar-Strukturen sind größer */
        DFBResult r = dfb->GetDisplayLayer(dfb, id, &l);
        if (r != DFB_OK) { printf("Ebene %d: %s\n", id, DirectFBErrorString(r)); continue; }
        l->SetCooperativeLevel(l, DLSCL_SHARED);
        memset(&wd, 0, sizeof wd);
        wd.d.flags = DWDESC_POSX | DWDESC_POSY | DWDESC_WIDTH | DWDESC_HEIGHT | DWDESC_CAPS | DWDESC_PIXELFORMAT;
        wd.d.width = 1920;
        wd.d.height = 1080;
        wd.d.caps = DWCAPS_ALPHACHANNEL;
        wd.d.pixelformat = DSPF_ARGB;
        r = l->CreateWindow(l, &wd.d, &w);
        printf("Ebene %d (%s): CreateWindow %s\n", id, name[id], DirectFBErrorString(r));
        if (r == DFB_OK && w->GetSurface(w, &s) == DFB_OK) {
            s->Clear(s, col[id][0], col[id][1], col[id][2], 0xff);
            s->Flip(s, NULL, DSFLIP_NONE);
            w->SetOpacity(w, 0xff);
            w->RaiseToTop(w);
            sleep(4);
            w->SetOpacity(w, 0x00);
            s->Release(s);
        }
        if (w) { w->Destroy(w); w->Release(w); }
        l->Release(l);
        sleep(1);
    }
    dfb->Release(dfb);
    printf("fertig\n");
    return 0;
}
