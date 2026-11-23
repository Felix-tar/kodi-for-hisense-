/*
 * Welche Fenster (auch anderer Prozesse, z. B. des Launchers) liegen auf welcher DirectFB-Ebene?
 * Nur lesen: IDirectFBDisplayLayer::GetWindow(id) für id 1..200, je Fenster Größe, Position,
 * Deckkraft. Ziel: Kodi soll die Ebene des Launchers erkennen und sich dort obenauf legen.
 */
#include <stdio.h>
#include <string.h>

#include <directfb.h>

int main(int argc, char **argv)
{
    IDirectFB *dfb = NULL;
    int id;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (DirectFBInit(&argc, &argv) != DFB_OK || DirectFBCreate(&dfb) != DFB_OK) return 1;
    for (id = 0; id < 3; id++) {
        IDirectFBDisplayLayer *l = NULL;
        DFBWindowID wid;
        int found = 0;
        if (dfb->GetDisplayLayer(dfb, id, &l) != DFB_OK) continue;
        l->SetCooperativeLevel(l, DLSCL_SHARED);
        for (wid = 1; wid <= 200; wid++) {
            IDirectFBWindow *w = NULL;
            int x = 0, y = 0, ww = 0, hh = 0;
            unsigned char op = 0;
            if (l->GetWindow(l, wid, &w) != DFB_OK || !w) continue;
            w->GetPosition(w, &x, &y);
            w->GetSize(w, &ww, &hh);
            w->GetOpacity(w, &op);
            printf("Ebene %d: Fenster %u %dx%d bei %d,%d Deckkraft %u\n", id, (unsigned)wid, ww, hh, x, y,
                   (unsigned)op);
            w->Release(w);
            found++;
        }
        printf("Ebene %d: %d Fenster\n", id, found);
        l->Release(l);
    }
    dfb->Release(dfb);
    return 0;
}
