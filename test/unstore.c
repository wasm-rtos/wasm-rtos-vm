/* A host empties a store: a frame's slot is read, forgotten, and the frame goes to a new one with its bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../source/wm.h"

static int g_failures;
static uint8_t g_slots[64][65536];
static uint8_t g_used[64];
static uint32_t g_writes, g_drops;

static void check(int ok, const char* what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += !ok;
}

static int reserve(void* context, int32_t frames)
{
    (void)context, (void)frames;
    return 1;
}

static int room(void* context, int again)
{
    (void)context, (void)again;
    return 1;
}

/* A new slot for every write: the first free one. */
static WmResult write(void* context, uint32_t* slot, const uint8_t* frame)
{
    (void)context;
    if (*slot != 0xFFFFFFFFu)
        g_used[*slot] = 0;
    for (uint32_t i = 0; i < 64; ++i)
        if (!g_used[i])
        {
            g_used[i] = 1;
            memcpy(g_slots[i], frame, wm_frame_size());
            *slot = i;
            ++g_writes;
            return NULL;
        }
    return "exhaustion: no slot";
}

static WmResult read(void* context, uint32_t slot, uint8_t* frame)
{
    (void)context;
    memcpy(frame, g_slots[slot], wm_frame_size());
    return NULL;
}

static void drop(void* context, uint32_t slot)
{
    (void)context;
    g_used[slot] = 0;
    ++g_drops;
}

int main(void)
{
    static const WmPager pager = { NULL, reserve, room, write, read, drop, NULL, NULL, NULL, NULL };
    WmMemory* memory;
    uint8_t byte = 0;
    uint32_t first;
    wm_set_pager(&pager);
    memory = wm_memory_new(1, 4);
    check(memory != NULL && wm_memory_frames(memory) > 1, "a paged memory");
    if (!memory)
        return 1;
    check(wm_memory_frame_slot(memory, 0) == 0xFFFFFFFFu, "a frame never stored has no slot");
    check(!wm_memory_write(memory, 5, "x", 1), "write to frame 0");
    check(!wm_memory_frame_take(memory, 0) && (first = wm_memory_frame_slot(memory, 0)) != 0xFFFFFFFFu,
          "frame 0 leaves RAM into a slot");
    check(wm_memory_stored(memory) == wm_frame_size(), "it is only in the store");
    check(!wm_memory_frame_unstore(memory, 0), "forget its slot");
    check(wm_memory_frame_slot(memory, 0) == 0xFFFFFFFFu && g_drops == 1 && !g_used[first],
          "the slot is dropped and free");
    check(wm_memory_stored(memory) == 0 && !wm_memory_read(memory, 5, &byte, 1) && byte == 'x',
          "the frame is back in RAM with its bytes");
    g_used[first] = 1; /* the old slot is taken by someone else now */
    check(!wm_memory_frame_take(memory, 0) && wm_memory_frame_slot(memory, 0) != first && g_writes == 2,
          "it counts as dirty: leaving again, it is written to a new slot");
    check(!wm_memory_read(memory, 5, &byte, 1) && byte == 'x', "and reads back the same");
    check(!wm_memory_frame_unstore(memory, 1), "a frame without a slot: nothing to do");
    wm_memory_free(memory);
    wm_set_pager(NULL);
    printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures != 0;
}
