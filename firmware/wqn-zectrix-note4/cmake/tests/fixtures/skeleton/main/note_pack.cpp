// M8 gate fixture: declared writable-Load file #3, two entries. Also a
// legitimate owner of raw SPIFFS writes.
#include <cstdio>
#include <unistd.h>

// [load-repair] Writes: unlinks a corrupt image cache entry.
esp_err_t LoadNoteImageTransaction(void* opaque)
{
    (void)opaque;
    unlink("/cache/x");
    return 0;
}

// [load-repair] Writes through LoadNoteImageTransaction.
esp_err_t LoadCachedNoteImage(const char* image_id, int* wqni)
{
    (void)image_id;
    if (wqni == nullptr) return 1;
    *wqni = 0;
    return 0;
}
