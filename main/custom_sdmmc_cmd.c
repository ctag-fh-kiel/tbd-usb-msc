#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"
#include "esp_memory_utils.h"
#include <string.h>

static const char* TAG = "custom_sdmmc_cmd";

// Declare sdmmc_read_sectors_dma which is in the SDK
extern esp_err_t sdmmc_read_sectors_dma(
    sdmmc_card_t* card,
    void* dst,
    size_t start_block,
    size_t block_count,
    size_t buffer_len
);

extern esp_err_t sdmmc_write_sectors_dma(
    sdmmc_card_t* card,
    const void* src,
    size_t start_block,
    size_t block_count,
    size_t buffer_len
);

//DMA_ATTR static uint8_t sector_buffer[512]; // -> runtime error
//__attribute__((section(".dram0"), aligned(4))) // -> linker warning
//uint8_t sector_buffer[512];

// Static pointer for DMA buffer - allocated once on first use
#define DMA_BUFFER_SIZE 512  // Single block buffer
static uint8_t* sector_buffer = NULL;
static size_t sector_buffer_actual_size = 0; // actual allocated size (may be larger due to heap alignment)

// Ensure buffer is allocated (called before first use)
static esp_err_t ensure_buffer_allocated()
{
    if (sector_buffer == NULL) {
        sector_buffer = (uint8_t*)heap_caps_malloc(DMA_BUFFER_SIZE, MALLOC_CAP_DMA);
        if (sector_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate DMA buffer");
            return ESP_ERR_NO_MEM;
        }
        sector_buffer_actual_size = heap_caps_get_allocated_size(sector_buffer);
        ESP_LOGI(TAG, "DMA buffer allocated at %p (requested: %d bytes, actual: %zu bytes)",
                 sector_buffer, DMA_BUFFER_SIZE, sector_buffer_actual_size);
    }
    return ESP_OK;
}

// Your wrapped implementation
esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_block, size_t block_count)
{
    if (block_count == 0) {
        return ESP_OK;
    }

    size_t block_size = card->csd.sector_size;
    // only works for block size 512
    assert(block_size == 512);

    // Fast path: if buffer is already DMA-capable and aligned, use it directly
    bool is_aligned = card->host.check_buffer_alignment(card->host.slot, dst, block_size * block_count);
    if (is_aligned
        #if !SOC_SDMMC_PSRAM_DMA_CAPABLE
            && !esp_ptr_external_ram(dst)
        #endif
    ) {
        // Buffer is suitable for direct DMA - bypass wrapper overhead
        return sdmmc_read_sectors_dma(card, dst, start_block, block_count, block_size * block_count);
    }

    // Slow path: buffer not DMA-capable, use single-block reads like original SDK
    esp_err_t err = ensure_buffer_allocated();
    if (err != ESP_OK) {
        return err;
    }

    uint8_t* cur_dst = (uint8_t*)dst;

    // Use single-block reads to match original SDK behavior
    for (size_t i = 0; i < block_count; ++i) {
        err = sdmmc_read_sectors_dma(card, sector_buffer, start_block + i, 1, sector_buffer_actual_size);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "Error 0x%x reading block %zu+%zu", err, start_block, i);
            break;
        }
        memcpy(cur_dst, sector_buffer, block_size);
        cur_dst += block_size;
    }

    return err;
}



esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* src,
        size_t start_block, size_t block_count)
{
    if (block_count == 0) {
        return ESP_OK;
    }

    size_t block_size = card->csd.sector_size;
    // only works for block size 512
    assert(block_size == 512);

    // Fast path: if buffer is already DMA-capable and aligned, use it directly
    bool is_aligned = card->host.check_buffer_alignment(card->host.slot, src, block_size * block_count);
    if (is_aligned
        #if !SOC_SDMMC_PSRAM_DMA_CAPABLE
            && !esp_ptr_external_ram(src)
        #endif
    ) {
        // Buffer is suitable for direct DMA - bypass wrapper overhead
        return sdmmc_write_sectors_dma(card, src, start_block, block_count, block_size * block_count);
    }

    // Slow path: buffer not DMA-capable, use single-block writes like original SDK
    esp_err_t err = ensure_buffer_allocated();
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t* cur_src = (const uint8_t*)src;

    // Use single-block writes to match original SDK behavior
    for (size_t i = 0; i < block_count; ++i) {
        memcpy(sector_buffer, cur_src, block_size);
        cur_src += block_size;
        err = sdmmc_write_sectors_dma(card, sector_buffer, start_block + i, 1, sector_buffer_actual_size);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "%s: error 0x%x writing block %zu+%zu",
                    __func__, err, start_block, i);
            break;
        }
    }

    return err;
}
