#include "t_utils.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

void bytePrint(const void* const thing, const size_t byteCount)
{
    int mask;
    const uint8_t* base = (uint8_t*)thing;
    for (int i = byteCount - 1; i >= 0; i--) 
    {
        for (int j = 8 - 1; j >= 0; j--) 
        {
            mask = 1 << j;
            if (mask & *(base + i))
                putchar('1');
            else
                putchar('0');
        }
    }
    putchar('\n');
}

void bitprint(const void* const thing, const size_t bitcount)
{
    int mask;
    for (int i = bitcount - 1; i >= 0; i--) {
        mask = 1 << i;   
        if (mask & *(int*)thing)
            putchar('1');
        else
            putchar('0');
    }
    putchar('\n');
}

void obdn_TimerStart(Obdn_Timer* t)
{
    clock_gettime(t->clockId, &t->startTime);
}

void obdn_TimerStop(Obdn_Timer* t)
{
    clock_gettime(t->clockId, &t->endTime);
}

void obdn_TimerInit(Obdn_Timer* t)
{
    memset(t, 0, sizeof(Obdn_Timer));
    t->clockId = CLOCK_MONOTONIC;
}

void obdn_PrintTime(const Obdn_Timer* t)
{
    const uint32_t seconds = t->endTime.tv_sec - t->startTime.tv_sec;
    const uint32_t ns = t->endTime.tv_nsec - t->startTime.tv_nsec;
    printf("%d.%09d\n",seconds,ns);
}

void obdn_LoopStatsInit(Obdn_LoopStats* stats)
{
    memset(stats, 0, sizeof(Obdn_LoopStats));
    stats->longestFrame = UINT32_MAX;
}

void obdn_LoopStatsUpdate(const Obdn_Timer* t, Obdn_LoopStats* s)
{
    s->nsDelta  = (t->endTime.tv_sec * 1000000000 + t->endTime.tv_nsec) - (t->startTime.tv_sec * 1000000000 + t->startTime.tv_nsec);
    s->nsTotal += s->nsDelta;

    if (s->nsDelta > s->longestFrame) s->longestFrame = s->nsDelta;
    if (s->nsDelta < s->shortestFrame) s->shortestFrame = s->nsDelta;

    s->frameCount++;
}

void obdn_LoopSleep(const Obdn_LoopStats* s, const uint32_t nsTarget)
{
    struct timespec diffTime;
    diffTime.tv_nsec = nsTarget > s->nsDelta ? nsTarget - s->nsDelta : 0;
    diffTime.tv_sec  = 0;
    // we could use the second parameter to handle interrupts and signals
    nanosleep(&diffTime, NULL);
}

Obdn_LoopData obdn_CreateLoopData(const uint32_t targetNs, const bool printFps, const bool printNS)
{
    Obdn_LoopData data = {
        .targetNs = targetNs,
        .printFps = printFps,
        .printNs  = printNS
    };

    data.timer.clockId = CLOCK_MONOTONIC;
    data.loopStats.longestFrame = UINT32_MAX;

    return data;
}

void obdn_FrameStart(Obdn_LoopData *data)
{
    obdn_TimerStart(&data->timer);
}

void obdn_FrameEnd(Obdn_LoopData *data)
{
    obdn_TimerStop(&data->timer);
    obdn_LoopStatsUpdate(&data->timer, &data->loopStats);
    if (data->printFps)
        printf("FPS: %f\n", 1000000000.0 / data->loopStats.nsDelta );
    if (data->printNs)
        printf("Delta s: %09ld\n", data->loopStats.nsDelta);
    obdn_LoopSleep(&data->loopStats, data->targetNs);
}

uint64_t obdn_GetAligned(const uint64_t quantity, const uint32_t alignment)
{
    assert(alignment != 0);
    if (quantity % alignment != 0) // not aligned
        return (quantity / alignment + 1) * alignment;
    else
        return quantity;
}
