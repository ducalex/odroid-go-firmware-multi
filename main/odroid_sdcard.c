#include "odroid_sdcard.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <sdmmc_cmd.h>
#include <diskio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>


#define SD_PIN_NUM_MISO 19
#define SD_PIN_NUM_MOSI 23
#define SD_PIN_NUM_CLK  18
#define SD_PIN_NUM_CS 22


static bool isOpen = false;


inline static void swap(char** a, char** b)
{
    char* t = *a;
    *a = *b;
    *b = t;
}

static int strcicmp(char const *a, char const *b)
{
    for (;; a++, b++)
    {
        int d = tolower((int)*a) - tolower((int)*b);
        if (d != 0 || !*a) return d;
    }
}

static int partition (char* arr[], int low, int high)
{
    char* pivot = arr[high];
    int i = (low - 1);

    for (int j = low; j <= high- 1; j++)
    {
        if (strcicmp(arr[j], pivot) < 0)
        {
            i++;
            swap(&arr[i], &arr[j]);
        }
    }
    swap(&arr[i + 1], &arr[high]);
    return (i + 1);
}

static void quick_sort(char* arr[], int low, int high)
{
    if (low < high)
    {
        int pi = partition(arr, low, high);

        quick_sort(arr, low, pi - 1);
        quick_sort(arr, pi + 1, high);
    }
}

static void sort_files(char** files, int count)
{
    if (count > 1)
    {
        quick_sort(files, 0, count - 1);
    }
}


int odroid_sdcard_files_get(const char* path, const char* extension, char*** filesOut)
{
    const int MAX_FILES = 1024;

    int count = 0;
    char** result = (char**)malloc(MAX_FILES * sizeof(void*));
    if (!result) abort();


    DIR *dir = opendir(path);
    if( dir == NULL )
    {
        ESP_LOGE(__func__, "opendir failed.");
        return 0;
    }

    int extensionLength = strlen(extension);
    if (extensionLength < 1) abort();


    char* temp = (char*)malloc(extensionLength + 1);
    if (!temp) abort();

    memset(temp, 0, extensionLength + 1);


    struct dirent *entry;
    while((entry=readdir(dir)) != NULL)
    {
        size_t len = strlen(entry->d_name);


        // ignore 'hidden' files (MAC)
        bool skip = false;
        if (entry->d_name[0] == '.') skip = true;


        memset(temp, 0, extensionLength + 1);
        if (!skip)
        {
            for (int i = 0; i < extensionLength; ++i)
            {
                temp[i] = tolower((int)entry->d_name[len - extensionLength + i]);
            }

            if (len > extensionLength)
            {
                if (strcmp(temp, extension) == 0)
                {
                    result[count] = (char*)malloc(len + 1);

                    if (!result[count])
                    {
                        abort();
                    }

                    strcpy(result[count], entry->d_name);
                    ++count;

                    if (count >= MAX_FILES) break;
                }
            }
        }
    }

    closedir(dir);
    free(temp);

    sort_files(result, count);

    *filesOut = result;
    return count;
}

void odroid_sdcard_files_free(char** files, int count)
{
    for (int i = 0; i < count; ++i)
    {
        free(files[i]);
    }

    free(files);
}

esp_err_t odroid_sdcard_open(const char* base_path)
{
    esp_err_t ret;

    if (isOpen)
    {
        ESP_LOGE(__func__, "already open.");
        ret = ESP_FAIL;
    }
    else
    {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    	host.slot = HSPI_HOST; // HSPI_HOST;
    	//host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; //10000000;
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    	sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();
    	slot_config.gpio_miso = (gpio_num_t)SD_PIN_NUM_MISO;
    	slot_config.gpio_mosi = (gpio_num_t)SD_PIN_NUM_MOSI;
    	slot_config.gpio_sck  = (gpio_num_t)SD_PIN_NUM_CLK;
    	slot_config.gpio_cs = (gpio_num_t)SD_PIN_NUM_CS;
    	//slot_config.dma_channel = 2;

    	// Options for mounting the filesystem.
    	// If format_if_mount_failed is set to true, SD card will be partitioned and
    	// formatted in case when mounting fails.
    	esp_vfs_fat_sdmmc_mount_config_t mount_config;
        memset(&mount_config, 0, sizeof(mount_config));

    	mount_config.format_if_mount_failed = false;
    	mount_config.max_files = 5;


    	// Use settings defined above to initialize SD card and mount FAT filesystem.
    	// Note: esp_vfs_fat_sdmmc_mount is an all-in-one convenience function.
    	// Please check its source code and implement error recovery when developing
    	// production applications.
    	sdmmc_card_t* card;
    	ret = esp_vfs_fat_sdmmc_mount(base_path, &host, &slot_config, &mount_config, &card);

    	if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE)
        {
            ret = ESP_OK;
            isOpen = true;
        }
        else
        {
            ESP_LOGE(__func__, "esp_vfs_fat_sdmmc_mount failed (%d)", ret);
        }
    }

	return ret;
}

esp_err_t odroid_sdcard_close()
{
    esp_err_t ret;

    if (!isOpen)
    {
        ESP_LOGE(__func__, "not open.");
        ret = ESP_FAIL;
    }
    else
    {
        ret = esp_vfs_fat_sdmmc_unmount();

        if (ret == ESP_OK)
        {
            isOpen = false;
    	}
        else
        {
            ESP_LOGE(__func__, "esp_vfs_fat_sdmmc_unmount failed (%d)", ret);
        }
    }

    return ret;
}

size_t odroid_sdcard_get_filesize(const char* path)
{
    size_t ret = 0;

    if (!isOpen)
    {
        ESP_LOGE(__func__, "not open.");
    }
    else
    {
        FILE* f = fopen(path, "rb");
        if (f == NULL)
        {
            ESP_LOGE(__func__, "fopen failed.");
        }
        else
        {
            // get the file size
            fseek(f, 0, SEEK_END);
            ret = ftell(f);
            fseek(f, 0, SEEK_SET);
        }
    }

    return ret;
}

size_t odroid_sdcard_copy_file_to_memory(const char* path, void* ptr)
{
    size_t ret = 0;

    if (!isOpen)
    {
        ESP_LOGE(__func__, "not open.");
    }
    else
    {
        if (!ptr)
        {
            ESP_LOGE(__func__, "ptr is null.");
        }
        else
        {
            FILE* f = fopen(path, "rb");
            if (f == NULL)
            {
                ESP_LOGE(__func__, "fopen failed.");
            }
            else
            {
                // copy
                const size_t BLOCK_SIZE = 512;
                while(true)
                {
                    __asm__("memw");
                    size_t count = fread((uint8_t*)ptr + ret, 1, BLOCK_SIZE, f);
                    __asm__("memw");

                    ret += count;

                    if (count < BLOCK_SIZE) break;
                }
            }
        }
    }

    return ret;
}

esp_err_t odroid_sdcard_format(int fs_type)
{
    esp_err_t err = ESP_FAIL;
    const char *errmsg = "success!";
    sdmmc_card_t card;
    void *buffer = malloc(4096);
    DWORD partitions[] = {100, 0, 0, 0};
    BYTE drive = 0xFF;

    sdmmc_host_t host_config = SDSPI_HOST_DEFAULT();
    host_config.slot = HSPI_HOST;

    sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();
    slot_config.gpio_miso = (gpio_num_t)SD_PIN_NUM_MISO;
    slot_config.gpio_mosi = (gpio_num_t)SD_PIN_NUM_MOSI;
    slot_config.gpio_sck  = (gpio_num_t)SD_PIN_NUM_CLK;
    slot_config.gpio_cs = (gpio_num_t)SD_PIN_NUM_CS;

    if (buffer == NULL) {
        return false;
    }

    if (isOpen) {
        odroid_sdcard_close();
    }

    err = ff_diskio_get_drive(&drive);
    if (drive == 0xFF) {
        errmsg = "ff_diskio_get_drive() failed";
        goto _cleanup;
    }

    err = (*host_config.init)();
    if (err != ESP_OK) {
        errmsg = "host_config.init() failed";
        goto _cleanup;
    }

    err = sdspi_host_init_slot(host_config.slot, &slot_config);
    if (err != ESP_OK) {
        errmsg = "sdspi_host_init_slot() failed";
        goto _cleanup;
    }

    err = sdmmc_card_init(&host_config, &card);
    if (err != ESP_OK) {
        errmsg = "sdmmc_card_init() failed";
        goto _cleanup;
    }

    ff_diskio_register_sdmmc(drive, &card);

    ESP_LOGI(__func__, "partitioning card %d", drive);
    if (f_fdisk(drive, partitions, buffer) != FR_OK) {
        errmsg = "f_fdisk() failed";
        err = ESP_FAIL;
        goto _cleanup;
    }

    ESP_LOGI(__func__, "formatting card %d", drive);
    char path[3] = {(char)('0' + drive), ':', 0};
    if (f_mkfs(path, fs_type ? FM_EXFAT : FM_FAT32, 0, buffer, 4096) != FR_OK) {
        errmsg = "f_mkfs() failed";
        err = ESP_FAIL;
        goto _cleanup;
    }

    err = ESP_OK;

_cleanup:

    if (err == ESP_OK) {
        ESP_LOGI(__func__, "%s", errmsg);
    } else {
        ESP_LOGE(__func__, "%s (%d)", errmsg, err);
    }

    free(buffer);
    host_config.deinit();
    ff_diskio_unregister(drive);

    return err;
}
