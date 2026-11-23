/*
 * Welche DirectFB-Ebenen gibt es am TV, wie liegen sie übereinander (Level) und wie sind sie
 * eingestellt? Nur lesen: keine Fenster, kein Bild, keine Änderung (Kooperationsstufe SHARED).
 * Hintergrund: ein Fenster auf DLID_PRIMARY war nachts sichtbar (Puffer-Modus 8), nach einem
 * Kaltstart nicht mehr (Puffer-Modus 2).
 */
#include <stdio.h>
#include <string.h>

#include <directfb.h>

static IDirectFB *dfb;

static DFBEnumerationResult layer_cb(DFBDisplayLayerID id, DFBDisplayLayerDescription desc, void *data)
{
    IDirectFBDisplayLayer *l = NULL;
    union { DFBDisplayLayerConfig c; unsigned char spare[256]; } lc; /* MStar-Strukturen sind größer */
    int level = -999;
    DFBResult r;

    (void)data;
    printf("Ebene %u: \"%s\" type 0x%x caps 0x%08x regions %d\n", (unsigned)id, desc.name,
           (unsigned)desc.type, (unsigned)desc.caps, desc.regions);
    r = dfb->GetDisplayLayer(dfb, id, &l);
    if (r != DFB_OK) {
        printf("   GetDisplayLayer: %s\n", DirectFBErrorString(r));
        return DFENUM_OK;
    }
    l->SetCooperativeLevel(l, DLSCL_SHARED);
    memset(&lc, 0, sizeof lc);
    r = l->GetConfiguration(l, &lc.c);
    if (r == DFB_OK)
        printf("   %dx%d format 0x%08x puffer %d optionen 0x%08x flags 0x%08x\n", lc.c.width, lc.c.height,
               (unsigned)lc.c.pixelformat, (int)lc.c.buffermode, (unsigned)lc.c.options, (unsigned)lc.c.flags);
    else
        printf("   GetConfiguration: %s\n", DirectFBErrorString(r));
    r = l->GetLevel(l, &level);
    printf("   level %d (%s)\n", level, r == DFB_OK ? "ok" : DirectFBErrorString(r));
    l->Release(l);
    return DFENUM_OK;
}

int main(int argc, char **argv)
{
    if (DirectFBInit(&argc, &argv) != DFB_OK || DirectFBCreate(&dfb) != DFB_OK) {
        printf("DirectFB-Start fehlgeschlagen\n");
        return 1;
    }
    dfb->EnumDisplayLayers(dfb, layer_cb, NULL);
    dfb->Release(dfb);
    return 0;
}
