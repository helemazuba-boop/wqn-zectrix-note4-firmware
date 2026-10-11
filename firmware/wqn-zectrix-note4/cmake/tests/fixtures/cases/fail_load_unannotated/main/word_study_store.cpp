// M8 gate fixture: both annotations stripped from a file with two declared
// writable Loads. Per-file counting is what catches the second one.
#include <cstdio>
esp_err_t LoadSessionSlotRaw(int mode, int* session)
{
    (void)mode;
    if (session == nullptr) return 1;
    *session = 0;
    return 0;
}
esp_err_t LoadSessionTransaction(void* opaque)
{
    (void)opaque;
    return 0;
}
