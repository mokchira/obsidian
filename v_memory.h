#ifndef TANTO_V_MEMORY_H
#define TANTO_V_MEMORY_H

#include "v_def.h"
#include <stdbool.h>

typedef uint32_t Tanto_V_BlockId;

typedef struct {
    size_t       size;
    uint8_t*     hostData;
    VkDeviceSize vOffset;
    VkBuffer*    vBuffer;
    bool         isMapped;
} Tanto_V_BlockHostBuffer;

typedef struct {
} Tanto_V_BlockDevBuffer;

typedef struct {
    VkImage           handle;
    VkImageView       view;
    VkSampler         sampler;
    Tanto_V_BlockId   memBlockId;
} Tanto_V_Image;

void tanto_v_InitMemory(void);

Tanto_V_BlockHostBuffer* tanto_v_RequestBlockHost(size_t size, const VkBufferUsageFlags);

Tanto_V_BlockHostBuffer* tanto_v_RequestBlockHostAligned(const size_t size, const uint32_t alignment);

uint32_t tanto_v_GetMemoryType(uint32_t typeBits, const VkMemoryPropertyFlags properties);

Tanto_V_Image tanto_v_CreateImage(
        const uint32_t width, 
        const uint32_t height,
        const VkFormat format,
        const VkImageUsageFlags usageFlags,
        const VkImageAspectFlags aspectMask);

void tanto_v_DestroyImage(Tanto_V_Image image);

void tanto_v_BindImageToMemory(const VkImage, const uint32_t size);

void tanto_v_CleanUpMemory(void);

#endif /* end of include guard: V_MEMORY_H */
