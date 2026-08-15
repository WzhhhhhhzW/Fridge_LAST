#include "edge-impulse-sdk/porting/ei_classifier_porting.h"

extern "C"
{
#include "deepseek_bridge.h"
#include "system_tick.h"
}

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

namespace
{
constexpr size_t kHeapBytes = 48U * 1024U;
constexpr size_t kAlignment = 8U;

struct HeapBlock
{
    size_t size;
    HeapBlock * next;
    bool free;
};

alignas(8) uint8_t g_heap[kHeapBytes];
HeapBlock * g_heap_head = nullptr;

size_t align_size(size_t size)
{
    return (size + (kAlignment - 1U)) & ~(kAlignment - 1U);
}

void heap_initialize(void)
{
    if (nullptr == g_heap_head)
    {
        g_heap_head = reinterpret_cast<HeapBlock *>(g_heap);
        g_heap_head->size = kHeapBytes - sizeof(HeapBlock);
        g_heap_head->next = nullptr;
        g_heap_head->free = true;
    }
}

void heap_coalesce(void)
{
    HeapBlock * block = g_heap_head;

    while ((nullptr != block) && (nullptr != block->next))
    {
        if (block->free && block->next->free)
        {
            block->size += sizeof(HeapBlock) + block->next->size;
            block->next = block->next->next;
        }
        else
        {
            block = block->next;
        }
    }
}
} // namespace

EI_IMPULSE_ERROR ei_run_impulse_check_canceled(void)
{
    return EI_IMPULSE_OK;
}

EI_IMPULSE_ERROR ei_sleep(int32_t time_ms)
{
    uint32_t start = system_tick_now();
    while (!system_tick_elapsed(start, static_cast<uint32_t>(time_ms)))
    {
        __NOP();
    }
    return EI_IMPULSE_OK;
}

uint64_t ei_read_timer_ms(void)
{
    return system_tick_now();
}

uint64_t ei_read_timer_us(void)
{
    return static_cast<uint64_t>(system_tick_now()) * 1000ULL;
}

void ei_serial_set_baudrate(int baudrate)
{
    (void) baudrate;
}

char ei_getchar(void)
{
    return 0;
}

void ei_putchar(char c)
{
    char text[2] = {c, '\0'};
    deepseek_bridge_pc_write(text);
}

void ei_printf(const char * format, ...)
{
    char text[192];
    va_list args;

    va_start(args, format);
    (void) std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    text[sizeof(text) - 1U] = '\0';
    deepseek_bridge_pc_write(text);
}

void ei_printf_float(float value)
{
    int32_t scaled = static_cast<int32_t>(value * 1000.0f);
    char text[24];
    (void) std::snprintf(text,
                         sizeof(text),
                         "%ld.%03ld",
                         static_cast<long>(scaled / 1000),
                         static_cast<long>((scaled < 0 ? -scaled : scaled) % 1000));
    deepseek_bridge_pc_write(text);
}

void * ei_malloc(size_t size)
{
    HeapBlock * block;
    size_t wanted;

    if (0U == size)
    {
        return nullptr;
    }

    heap_initialize();
    wanted = align_size(size);
    block = g_heap_head;
    while (nullptr != block)
    {
        if (block->free && (block->size >= wanted))
        {
            if (block->size >= (wanted + sizeof(HeapBlock) + kAlignment))
            {
                uint8_t * split_address = reinterpret_cast<uint8_t *>(block + 1) + wanted;
                HeapBlock * split = reinterpret_cast<HeapBlock *>(split_address);
                split->size = block->size - wanted - sizeof(HeapBlock);
                split->next = block->next;
                split->free = true;
                block->next = split;
                block->size = wanted;
            }
            block->free = false;
            return block + 1;
        }
        block = block->next;
    }
    return nullptr;
}

void * ei_calloc(size_t count, size_t size)
{
    if ((0U != size) && (count > (SIZE_MAX / size)))
    {
        return nullptr;
    }

    size_t bytes = count * size;
    void * memory = ei_malloc(bytes);
    if (nullptr != memory)
    {
        std::memset(memory, 0, bytes);
    }
    return memory;
}

void ei_free(void * pointer)
{
    if (nullptr == pointer)
    {
        return;
    }

    uint8_t * address = static_cast<uint8_t *>(pointer);
    if ((address <= g_heap) || (address >= (g_heap + kHeapBytes)))
    {
        return;
    }

    HeapBlock * block = reinterpret_cast<HeapBlock *>(pointer) - 1;
    block->free = true;
    heap_coalesce();
}

/*
 * Some Edge Impulse continuous-classifier helpers use ordinary C++ new/new[]
 * instead of ei_malloc().  The FSP bare-metal heap is intentionally zero, so
 * route all C++ allocations in this application through the same bounded
 * 48 KiB model heap.  On exhaustion std::bad_alloc is caught by the KWS
 * wrapper and the listener is disabled without taking down the firmware.
 */
void * operator new(std::size_t size)
{
    void * pointer = ei_malloc(size);
    if (nullptr == pointer)
    {
        throw std::bad_alloc();
    }
    return pointer;
}

void * operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void * pointer) noexcept
{
    ei_free(pointer);
}

void operator delete[](void * pointer) noexcept
{
    ei_free(pointer);
}

void operator delete(void * pointer, std::size_t) noexcept
{
    ei_free(pointer);
}

void operator delete[](void * pointer, std::size_t) noexcept
{
    ei_free(pointer);
}

void DebugLog(const char * text)
{
    if (nullptr != text)
    {
        deepseek_bridge_pc_write(text);
    }
}
