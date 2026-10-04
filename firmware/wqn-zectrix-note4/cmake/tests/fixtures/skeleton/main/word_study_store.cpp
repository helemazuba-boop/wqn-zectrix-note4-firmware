// M8 gate fixture: declared writable-Load file #1, two entries.
#include <cstdio>

// [load-repair] Writes: promotes a torn primary file from its backup.
esp_err_t LoadSessionSlotRaw(int mode, int* session)
{
    (void)mode;
    if (session == nullptr) return 1;
    *session = 0;
    return 0;
}

// [load-repair] Writes: re-derives the cursor from the durable outbox.
esp_err_t LoadSessionTransaction(void* opaque)
{
    (void)opaque;
    return 0;
}
