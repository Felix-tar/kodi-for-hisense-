/*
 * Wie viele Bytes schreibt die DirectFB des TVs (MStar-Fassung von 1.4.2) wirklich, wenn sie eine
 * Struktur ausfüllt? Kodi stürzte ab, weil GetConfiguration() mehr als
 * sizeof(DFBDisplayLayerConfig) aus den 1.4.2-Headern auf den Stack schrieb (Vermutung).
 * Füllt einen Puffer mit einem Muster, lässt DirectFB hineinschreiben und meldet das letzte
 * veränderte Byte. Öffnet kein Fenster.
 */
#include <stdio.h>
#include <string.h>

#include <directfb.h>

#define PAT 0xA5
#define BUF 2048

static int last_written(const unsigned char *b)
{
    int i;
    for (i = BUF - 1; i >= 0; i--)
        if (b[i] != PAT)
            return i + 1;
    return 0;
}

int main(int argc, char **argv)
{
    IDirectFB *dfb = NULL;
    IDirectFBDisplayLayer *layer = NULL;
    static unsigned char buf[BUF];
    DFBResult r;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (DirectFBInit(&argc, &argv) != DFB_OK || DirectFBCreate(&dfb) != DFB_OK) {
        printf("DirectFB-Start fehlgeschlagen\n");
        return 1;
    }
    if (dfb->GetDisplayLayer(dfb, DLID_PRIMARY, &layer) != DFB_OK ||
        layer->SetCooperativeLevel(layer, DLSCL_SHARED) != DFB_OK) {
        printf("Ebene nicht verfügbar\n");
        dfb->Release(dfb);
        return 1;
    }

    memset(buf, PAT, sizeof buf);
    r = layer->GetConfiguration(layer, (DFBDisplayLayerConfig *)buf);
    printf("GetConfiguration: %s, sizeof(DFBDisplayLayerConfig) = %u, geschrieben bis Byte %d\n",
           DirectFBErrorString(r), (unsigned)sizeof(DFBDisplayLayerConfig), last_written(buf));

    memset(buf, PAT, sizeof buf);
    r = layer->GetLevel(layer, (int *)buf);
    printf("GetLevel: %s, sizeof(int) = %u, geschrieben bis Byte %d\n", DirectFBErrorString(r),
           (unsigned)sizeof(int), last_written(buf));

    memset(buf, PAT, sizeof buf);
    r = dfb->GetDeviceDescription(dfb, (DFBGraphicsDeviceDescription *)buf);
    printf("GetDeviceDescription: %s, sizeof = %u, geschrieben bis Byte %d\n", DirectFBErrorString(r),
           (unsigned)sizeof(DFBGraphicsDeviceDescription), last_written(buf));

    memset(buf, PAT, sizeof buf);
    r = layer->GetDescription(layer, (DFBDisplayLayerDescription *)buf);
    printf("Layer GetDescription: %s, sizeof = %u, geschrieben bis Byte %d\n", DirectFBErrorString(r),
           (unsigned)sizeof(DFBDisplayLayerDescription), last_written(buf));

    layer->Release(layer);
    dfb->Release(dfb);
    return 0;
}
