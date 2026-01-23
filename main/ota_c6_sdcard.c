/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_hosted_ota.h"
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"

static const char* TAG = "ota_c6_sd";

#ifndef CHUNK_SIZE
#define CHUNK_SIZE 1500
#endif

static int compare_self_version_with_slave_version(uint32_t slave_version)
{
    uint32_t host_version = ESP_HOSTED_VERSION_VAL(ESP_HOSTED_VERSION_MAJOR_1,
            ESP_HOSTED_VERSION_MINOR_1,
            ESP_HOSTED_VERSION_PATCH_1);

    // mask out patch level
    // compare major.minor only
    slave_version &= 0xFFFFFF00;
    host_version &= 0xFFFFFF00;

    if (host_version == slave_version) {
        // versions match
        return 0;
    } else if (host_version > slave_version) {
        // host version > slave version
        ESP_LOGW(TAG, "Version mismatch: Host [%u.%u.%u] > Co-proc [%u.%u.%u] ==> Upgrade co-proc to avoid RPC timeouts",
            ESP_HOSTED_VERSION_PRINTF_ARGS(host_version), ESP_HOSTED_VERSION_PRINTF_ARGS(slave_version));
        return -1;
    } else {
        // host version < slave version
        ESP_LOGW(TAG, "Version mismatch: Host [%u.%u.%u] < Co-proc [%u.%u.%u] ==> Upgrade host to avoid compatibility issues",
            ESP_HOSTED_VERSION_PRINTF_ARGS(host_version), ESP_HOSTED_VERSION_PRINTF_ARGS(slave_version));
        return 1;
    }
}

/* Check host-slave version compatibility */
static int compare_host_slave_version(void)
{
    /* Get slave version via RPC */
    esp_hosted_coprocessor_fwver_t slave_version_struct = {0};
    esp_err_t ret = esp_hosted_get_coprocessor_fwversion(&slave_version_struct);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not get slave firmware version (error: %s)", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Proceeding without version compatibility check");
        return ESP_OK;
    }

    /* Convert slave version to 32-bit value for comparison */
    uint32_t slave_version = ESP_HOSTED_VERSION_VAL(slave_version_struct.major1,
            slave_version_struct.minor1,
            slave_version_struct.patch1);

    /* Log versions */
    ESP_LOGI(TAG, "Host firmware version: %d.%d.%d", ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1, ESP_HOSTED_VERSION_PATCH_1);
    ESP_LOGI(TAG, "Slave firmware version: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
             slave_version_struct.major1, slave_version_struct.minor1, slave_version_struct.patch1);

    return compare_self_version_with_slave_version(slave_version);
}

/* Function to parse ESP32 image header and get firmware info from file */
static esp_err_t parse_image_header_from_file(const char* file_path, size_t* firmware_size, char* app_version_str, size_t version_str_len)
{
	FILE* file;
	esp_image_header_t image_header;
	esp_image_segment_header_t segment_header;
	esp_app_desc_t app_desc;
	size_t offset = 0;
	size_t total_size = 0;

	file = fopen(file_path, "rb");
	if (file == NULL) {
		ESP_LOGE(TAG, "Failed to open firmware file for header verification: %s", file_path);
		return ESP_FAIL;
	}

	/* Read image header */
	if (fread(&image_header, sizeof(image_header), 1, file) != 1) {
		ESP_LOGE(TAG, "Failed to read image header from file");
		fclose(file);
		return ESP_FAIL;
	}

	/* Validate magic number */
	if (image_header.magic != ESP_IMAGE_HEADER_MAGIC) {
		ESP_LOGE(TAG, "Invalid image magic: 0x%" PRIx8, image_header.magic);
		fclose(file);
		return ESP_ERR_INVALID_ARG;
	}

	ESP_LOGI(TAG, "Image header: magic=0x%" PRIx8 ", segment_count=%" PRIu8 ", hash_appended=%" PRIu8,
			image_header.magic, image_header.segment_count, image_header.hash_appended);

	/* Calculate total size by reading all segments */
	offset = sizeof(image_header);
	total_size = sizeof(image_header);

	for (int i = 0; i < image_header.segment_count; i++) {
		/* Read segment header */
		if (fseek(file, offset, SEEK_SET) != 0 ||
				fread(&segment_header, sizeof(segment_header), 1, file) != 1) {
			ESP_LOGE(TAG, "Failed to read segment %d header", i);
			fclose(file);
			return ESP_FAIL;
		}

		ESP_LOGI(TAG, "Segment %d: data_len=%" PRIu32 ", load_addr=0x%" PRIx32, i, segment_header.data_len, segment_header.load_addr);

		/* Add segment header size + data size */
		total_size += sizeof(segment_header) + segment_header.data_len;
		offset += sizeof(segment_header) + segment_header.data_len;

		/* Read app description from the first segment */
		if (i == 0) {
			size_t app_desc_offset = sizeof(image_header) + sizeof(segment_header);
			if (fseek(file, app_desc_offset, SEEK_SET) == 0 &&
					fread(&app_desc, sizeof(app_desc), 1, file) == 1) {
				strncpy(app_version_str, app_desc.version, version_str_len - 1);
				app_version_str[version_str_len - 1] = '\0';
				ESP_LOGI(TAG, "Found app description: version='%s', project_name='%s'",
						app_desc.version, app_desc.project_name);
			} else {
				ESP_LOGW(TAG, "Failed to read app description");
				strncpy(app_version_str, "unknown", version_str_len - 1);
				app_version_str[version_str_len - 1] = '\0';
			}
		}
	}

	/* Add padding to align to 16 bytes */
	size_t padding = (16 - (total_size % 16)) % 16;
	if (padding > 0) {
		ESP_LOGD(TAG, "Adding %u bytes of padding for alignment", (unsigned int)padding);
		total_size += padding;
	}

	/* Add the checksum byte (always present) */
	total_size += 1;
	ESP_LOGD(TAG, "Added 1 byte for checksum");

	/* Add SHA256 hash if appended */
	bool has_hash = (image_header.hash_appended == 1);
	if (has_hash) {
		total_size += 32;  // SHA256 hash is 32 bytes
		ESP_LOGD(TAG, "Added 32 bytes for SHA256 hash (hash_appended=1)");
	} else {
		ESP_LOGD(TAG, "No SHA256 hash appended (hash_appended=0)");
	}

	*firmware_size = total_size;
	ESP_LOGI(TAG, "Total image size: %u bytes", (unsigned int)*firmware_size);

	fclose(file);
	return ESP_OK;
}

/* Find latest firmware file in sd card */
static esp_err_t find_latest_firmware(char* firmware_path, size_t max_len, const char *mount_point)
{
	DIR *dir;
	struct dirent *entry;
	struct stat file_stat;
	char *latest_file = malloc(256); // Use heap instead of stack
	char *full_path = malloc(512);   // Use heap for full path

	if (!latest_file || !full_path) {
		ESP_LOGE(TAG, "Failed to allocate memory for file search");
		if (latest_file) free(latest_file);
		if (full_path) free(full_path);
		return ESP_ERR_NO_MEM;
	}

	memset(latest_file, 0, 256);

	dir = opendir(mount_point);
	if (dir == NULL) {
		ESP_LOGE(TAG, "Failed to open %s directory", mount_point);
		free(latest_file);
		free(full_path);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Successfully opened %s directory", mount_point);

	/* Find the first .bin file (since timestamps might not be reliable in sd card) */
	while ((entry = readdir(dir)) != NULL) {
		ESP_LOGI(TAG, "Found file: %s", entry->d_name);
		if (strstr(entry->d_name, ".bin") != NULL) {
			ESP_LOGI(TAG, "Found .bin file: %s", entry->d_name);
			snprintf(full_path, 512, "%s/%s", mount_point, entry->d_name);

			if (stat(full_path, &file_stat) == 0) {
				ESP_LOGI(TAG, "File stat successful for %s, size: %ld", entry->d_name, file_stat.st_size);
				/* Use the first .bin file found */
				strncpy(latest_file, entry->d_name, 255);
				latest_file[255] = '\0'; // Ensure null termination
				ESP_LOGI(TAG, "Using firmware file: %s", latest_file);
				break; // Use the first .bin file found
			} else {
				ESP_LOGW(TAG, "Failed to stat file: %s", full_path);
			}
		}
	}
	closedir(dir);

	ESP_LOGI(TAG, "Final latest_file: '%s', length: %d", latest_file, strlen(latest_file));

	if (strlen(latest_file) == 0) {
		ESP_LOGE(TAG, "No .bin files found in %s directory. Please refer doc to know how partition is created with slave firmware at correct path.", mount_point);
		free(latest_file);
		free(full_path);
		return ESP_FAIL;
	}

	// Ensure we don't overflow the destination buffer
	if (snprintf(firmware_path, max_len, "%s/%s", mount_point, latest_file) >= max_len) {
		ESP_LOGE(TAG, "Firmware path too long");
		free(latest_file);
		free(full_path);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Found latest firmware: %s", firmware_path);

	// Clean up allocated memory
	free(latest_file);
	free(full_path);

	return ESP_OK;
}

// internal function to perform ota from sd card
static esp_err_t ota_c6_sd_perform_(bool delete_after_use, const char* mount_point)
{
	char *firmware_path = malloc(256); // Use heap instead of stack
	FILE *firmware_file;
	uint8_t *chunk = malloc(CHUNK_SIZE); // Use heap for chunk buffer
	size_t bytes_read;
	esp_err_t ret = ESP_OK;

	if (!firmware_path || !chunk) {
		ESP_LOGE(TAG, "Failed to allocate memory");
		if (firmware_path) free(firmware_path);
		if (chunk) free(chunk);
		return ESP_ERR_NO_MEM;
	}

	ESP_LOGI(TAG, "Starting C6 SD OTA process");


	/* Find the latest firmware file */
	ESP_LOGI(TAG, "Searching for firmware files in on sd-card");
	ret = find_latest_firmware(firmware_path, 256, mount_point);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to find firmware file");
		free(firmware_path);
		free(chunk);
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	}
	ESP_LOGI(TAG, "Firmware file found: %s", firmware_path);

	/* Verify image header and get firmware info */
	size_t firmware_size;
	char new_app_version[32];
	ret = parse_image_header_from_file(firmware_path, &firmware_size, new_app_version, sizeof(new_app_version));
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to parse image header: %s", esp_err_to_name(ret));
		free(firmware_path);
		free(chunk);
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	}

	ESP_LOGI(TAG, "Firmware verified - Size: %u bytes, Version: %s", (unsigned int)firmware_size, new_app_version);

	/* Get current running slave firmware version */
	esp_hosted_coprocessor_fwver_t current_slave_version = {0};
	esp_err_t version_ret = esp_hosted_get_coprocessor_fwversion(&current_slave_version);

	if (version_ret == ESP_OK) {
		char current_version_str[32];
		snprintf(current_version_str, sizeof(current_version_str), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
				current_slave_version.major1, current_slave_version.minor1, current_slave_version.patch1);

		ESP_LOGI(TAG, "Current slave firmware version: %s", current_version_str);
		ESP_LOGI(TAG, "New slave firmware version: %s", new_app_version);

		if (strcmp(new_app_version, current_version_str) == 0) {
			ESP_LOGW(TAG, "Current slave firmware version (%s) is the same as new version (%s). Skipping OTA.",
					current_version_str, new_app_version);
			free(firmware_path);
			free(chunk);
			return ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED;
		}

		ESP_LOGI(TAG, "Version differs - proceeding with OTA from %s to %s", current_version_str, new_app_version);
	} else {
		ESP_LOGW(TAG, "Could not get current slave firmware version (error: %s), proceeding with OTA",
				esp_err_to_name(version_ret));
	}

	/* Open firmware file */
	firmware_file = fopen(firmware_path, "rb");
	if (firmware_file == NULL) {
		ESP_LOGE(TAG, "Failed to open firmware file: %s", firmware_path);
		free(firmware_path);
		free(chunk);
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Starting OTA from sd card: %s", firmware_path);

	/* Begin OTA */
	ret = esp_hosted_slave_ota_begin();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(ret));
		fclose(firmware_file);
		free(firmware_path);
		free(chunk);
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	}

	/* Write firmware in chunks */
	while ((bytes_read = fread(chunk, 1, CHUNK_SIZE, firmware_file)) > 0) {
		ret = esp_hosted_slave_ota_write(chunk, bytes_read);
		if (ret != ESP_OK) {
			ESP_LOGE(TAG, "Failed to write OTA chunk: %s", esp_err_to_name(ret));
			fclose(firmware_file);
			free(firmware_path);
			free(chunk);
			return ESP_HOSTED_SLAVE_OTA_FAILED;
		}
	}

	fclose(firmware_file);

	/* End OTA */
	ret = esp_hosted_slave_ota_end();
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(ret));
		free(firmware_path);
		free(chunk);
		return ESP_HOSTED_SLAVE_OTA_FAILED;
	}

	ESP_LOGI(TAG, "sd card OTA completed successfully");

	/* Delete firmware file if requested */
	if (delete_after_use) {
		if (unlink(firmware_path) == 0) {
			ESP_LOGI(TAG, "Deleted firmware file: %s", firmware_path);
		} else {
			ESP_LOGW(TAG, "Failed to delete firmware file: %s", firmware_path);
		}
	}


	/* Clean up allocated memory */
	free(firmware_path);
	free(chunk);

	return ESP_HOSTED_SLAVE_OTA_COMPLETED;
}

// public interface
esp_err_t ota_c6_sd_perform(bool delete_after_use, const char* mount_point){
	int ret;
	int host_slave_version_not_compatible = 1;

	//ESP_ERROR_CHECK(nvs_flash_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	ESP_ERROR_CHECK(esp_hosted_init());
	ESP_ERROR_CHECK(esp_hosted_connect_to_slave());

	ESP_LOGI(TAG, "ESP-Hosted initialized successfully");

	/* Check host-slave version compatibility */
	host_slave_version_not_compatible = compare_host_slave_version();

	if (!host_slave_version_not_compatible) {
		ESP_LOGW(TAG, "Slave OTA not required, so nothing to do!");
		return -1;
	}

	/* Perform OTA from sd card */
	ESP_LOGI(TAG, "Using sd card OTA method");
	ret = ota_c6_sd_perform_(delete_after_use, mount_point);

	if (ret == ESP_HOSTED_SLAVE_OTA_COMPLETED) {
		ESP_LOGI(TAG, "OTA completed successfully");

		/* Activate the new firmware */
		ret = esp_hosted_slave_ota_activate();
		if (ret == ESP_OK) {
			ESP_LOGI(TAG, "Slave will reboot with new firmware");
			vTaskDelay(pdMS_TO_TICKS(2000));
		} else {
			ESP_LOGE(TAG, "Failed to activate OTA: %s", esp_err_to_name(ret));
		}
	} else if (ret == ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED) {
		ESP_LOGI(TAG, "OTA not required");
	} else {
		ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
	}
	return ret;
}