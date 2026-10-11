// M8 gate fixture: declared writable-Load file #2, two entries.
#include <cstdio>

// [load-repair] Writes: promotes a torn primary file from its backup.
esp_err_t LoadSessionRaw(int* session)
{
    if (session == nullptr) return 1;
    *session = 0;
    return 0;
}

// [load-repair] Writes: reconciles the session against the durable outbox.
esp_err_t LoadSessionTransaction(void* opaque)
{
    (void)opaque;
    return 0;
}
