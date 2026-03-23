/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

 // Includes for Arduino FS API
#include "FS.h"
#include "LittleFS.h"
#include "wled.h" // Includes FS and logging

// Includes for ESP-IDF Types and Functions
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_hosted_ota.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#endif
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"

static const char* TAG = "ota_littlefs";

#ifndef CHUNK_SIZE
#define CHUNK_SIZE 1500
#endif

/**
 * @brief Parse ESP32 image header from an Arduino File object
 *
 * @param file Arduino File object (must be opened for reading)
 * @param firmware_size Pointer to store the calculated total firmware size
 * @param app_version_str Buffer to store the read app version
 * @param version_str_len Length of the app_version_str buffer
 * @return ESP_OK on success, ESP_FAIL or ESP_ERR_INVALID_ARG on failure
 */
static esp_err_t parse_image_header_from_file_littlefs(File& file, size_t* firmware_size, char* app_version_str, size_t version_str_len) {
  esp_image_header_t image_header;
  esp_image_segment_header_t segment_header;
  esp_app_desc_t app_desc;
  size_t offset = 0;
  size_t total_size = 0;

  // Ensure the file is valid and at the beginning
  if (!file) {
    USER_PRINTLN("Invalid file object passed to parser");
    return ESP_ERR_INVALID_ARG;
  }
  file.seek(0, SeekSet); // Rewind file to the beginning

  /* Read image header */
  if (file.read((uint8_t*)&image_header, sizeof(image_header)) != sizeof(image_header)) {
    USER_PRINTLN("Failed to read image header from file");
    return ESP_FAIL;
  }

  /* Validate magic number */
  if (image_header.magic != ESP_IMAGE_HEADER_MAGIC) {
    USER_PRINTF("Invalid image magic: 0x%\n" PRIx8, image_header.magic);
    return ESP_ERR_INVALID_ARG;
  }

  USER_PRINTF("Image header: magic=0x%" PRIx8 ", segment_count=%" PRIu8 ", hash_appended=%" PRIu8 "\n", image_header.magic, image_header.segment_count, image_header.hash_appended);

  /* Calculate total size by reading all segments */
  offset = sizeof(image_header);
  total_size = sizeof(image_header);

  for (int i = 0; i < image_header.segment_count; i++) {
    /* Read segment header */
    if (!file.seek(offset, SeekSet) ||
      file.read((uint8_t*)&segment_header, sizeof(segment_header)) != sizeof(segment_header)) {
      USER_PRINTF("Failed to read segment %d header\n", i);
      return ESP_FAIL;
    }

    USER_PRINTF("Segment %d: data_len=%" PRIu32 ", load_addr=0x%" PRIx32 "\n", i, segment_header.data_len, segment_header.load_addr);

    /* Add segment header size + data size */
    total_size += sizeof(segment_header) + segment_header.data_len;
    offset += sizeof(segment_header) + segment_header.data_len;

    /* Read app description from the first segment */
    if (i == 0) {
      size_t app_desc_offset = sizeof(image_header) + sizeof(segment_header);
      if (file.seek(app_desc_offset, SeekSet) &&
        file.read((uint8_t*)&app_desc, sizeof(app_desc)) == sizeof(app_desc)) {

        strncpy(app_version_str, app_desc.version, version_str_len - 1);
        app_version_str[version_str_len - 1] = '\0'; // Ensure null termination
        USER_PRINTF("Found app description: version='%s', project_name='%s'\n", app_desc.version, app_desc.project_name);
      } else {
        USER_PRINTLN("Failed to read app description");
        strncpy(app_version_str, "unknown", version_str_len - 1);
        app_version_str[version_str_len - 1] = '\0';
      }
    }
  }

  /* Add padding to align to 16 bytes */
  size_t padding = (16 - (total_size % 16)) % 16;
  if (padding > 0) {
    USER_PRINTF("Adding %u bytes of padding for alignment\n", (unsigned int)padding);
    total_size += padding;
  }

  /* Add the checksum byte (always present) */
  total_size += 1;
  USER_PRINTLN("Added 1 byte for checksum");

  /* Add SHA256 hash if appended */
  bool has_hash = (image_header.hash_appended == 1);
  if (has_hash) {
    total_size += 32;  // SHA256 hash is 32 bytes
    USER_PRINTLN("Added 32 bytes for SHA256 hash (hash_appended=1)");
  } else {
    USER_PRINTLN("No SHA256 hash appended (hash_appended=0)");
  }

  *firmware_size = total_size;
  USER_PRINTF("Total image size: %u bytes\n", (unsigned int)*firmware_size);

  return ESP_OK;
}

/**
 * @brief Find the first .bin firmware file in the root directory.
 *
 * @return String object containing the *filename* (e.g., "firmware.bin")
 * or an empty String if no .bin file is found.
 */
static String find_latest_firmware_littlefs() {
  USER_PRINTLN("Opening root directory / ...");
  File root = LittleFS.open("/");
  if (!root) {
    USER_PRINTLN("Failed to open / directory");
    return "";
  }

  String latest_file = "";
  File file = root.openNextFile();

  while (file) {
    if (!file.isDirectory()) {
      String fileName = file.name();
      // USER_PRINTF("Found file: %s\n", fileName.c_str());

      if (fileName.startsWith("network_adapter") && fileName.endsWith(".bin")) {
        USER_PRINTF("Found update file: %s, size: %lu\n", fileName.c_str(), file.size());
        latest_file = fileName;
        break; // Use the first .bin file found
      }
    }
    file.close(); // Close this file
    file = root.openNextFile(); // Open the next
  }

  if (latest_file.length() == 0) {
    USER_PRINTLN("No .bin files found in / directory.");
  }

  if (file) file.close();
  root.close();
  return latest_file;
}

/**
 * @brief Main OTA function using WLED/Arduino LittleFS API
 */
esp_err_t ota_littlefs_perform(bool delete_after_use) {
  String latest_filename;
  String firmware_path;
  File firmware_file;
  uint8_t* chunk = (uint8_t*)heap_caps_malloc_prefer(CHUNK_SIZE, 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_DMA, MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL); // Use heap for chunk buffer
  size_t bytes_read;
  esp_err_t ret = ESP_OK;

  if (!chunk) {
    USER_PRINTLN("Failed to allocate chunk buffer");
    return ESP_ERR_NO_MEM;
  }

  USER_PRINTLN("Starting WiFi CoProcessor OTA process...");

  /* Find the latest firmware file */
  USER_PRINTLN("Searching for firmware files in LittleFS");
  latest_filename = find_latest_firmware_littlefs();
  if (latest_filename.length() == 0) {
    USER_PRINTLN("Failed to find firmware file");
    free(chunk);
    return ESP_HOSTED_SLAVE_OTA_FAILED;
  }

  // Create the absolute path
  firmware_path = "/" + latest_filename;
  USER_PRINTF("Firmware file found: %s\n", firmware_path.c_str());

  /* Open the file ONCE for parsing */
  firmware_file = LittleFS.open(firmware_path, "r");
  if (!firmware_file) {
    USER_PRINTF("Failed to open firmware file: %s\n", firmware_path.c_str());
    free(chunk);
    return ESP_FAIL;
  }

  /* Verify image header and get firmware info */
  size_t firmware_size;
  char new_app_version[32];
  ret = parse_image_header_from_file_littlefs(firmware_file, &firmware_size, new_app_version, sizeof(new_app_version));
  firmware_file.close();
  if (ret != ESP_OK) {
    USER_PRINTF("Failed to parse image header: %s\n", esp_err_to_name(ret));
    free(chunk);
    return ESP_HOSTED_SLAVE_OTA_FAILED;
  }

  USER_PRINTF("Firmware verified - Size: %u bytes, Version: %s\n", (unsigned int)firmware_size, new_app_version);

  #ifndef CONFIG_OTA_VERSION_FORCE_SLAVEFW
  /* Get current running slave firmware version */
  esp_hosted_coprocessor_fwver_t current_slave_version = { 0 };
  esp_err_t version_ret = esp_hosted_get_coprocessor_fwversion(&current_slave_version);

  if (version_ret == ESP_OK) {
    char current_version_str[32];
    snprintf(current_version_str, sizeof(current_version_str), "%" PRIu32 ".%" PRIu32 ".%" PRIu32, current_slave_version.major1, current_slave_version.minor1, current_slave_version.patch1);

    USER_PRINTF("Current slave firmware version: %s\n", current_version_str);
    USER_PRINTF("New slave firmware version: %s\n", new_app_version);

    if (strcmp(new_app_version, current_version_str) == 0) {
      USER_PRINTF("Current slave firmware version (%s) is the same as new version (%s). Skipping OTA.\n", current_version_str, new_app_version);
      firmware_file.close();
      free(chunk);
      return ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED;
    }

    USER_PRINTF("Version differs - proceeding with OTA from %s to %s\n", current_version_str, new_app_version);
  } else {
    USER_PRINTF("Could not get current slave firmware version (error: %s), proceeding with OTA\n", esp_err_to_name(version_ret));
  }
  #else
  USER_PRINTF("Version check disabled - proceeding with OTA (new firmware version: %s)\n", new_app_version);
  #endif

  USER_PRINTF("Starting OTA from LittleFS: %s\n", firmware_path.c_str());

  /* Begin OTA */
  ret = esp_hosted_slave_ota_begin();
  if (ret != ESP_OK) {
    USER_PRINTF("Failed to begin OTA: %s\n", esp_err_to_name(ret));
    firmware_file.close();
    free(chunk);
    return ESP_HOSTED_SLAVE_OTA_FAILED;
  }

  firmware_file = LittleFS.open(firmware_path, "r");
  if (!firmware_file) {
    USER_PRINTF("Failed to re-open firmware file for writing: %s\n", firmware_path.c_str());
    free(chunk);
    return ESP_FAIL;
  }

  /* Write firmware in chunks */
  uint32_t chunk_num = 1;
  while ((bytes_read = firmware_file.read(chunk, CHUNK_SIZE)) > 0) {
    ret = esp_hosted_slave_ota_write(chunk, bytes_read);
    if (ret != ESP_OK) {
      USER_PRINTF("Failed to write OTA chunk %d: %s\n", chunk_num, esp_err_to_name(ret));
      firmware_file.close();
      free(chunk);
      return ESP_HOSTED_SLAVE_OTA_FAILED;
    } else {
      USER_PRINTF("Wrote OTA chunk %d (%d bytes)\n", chunk_num, bytes_read);
    }
    chunk_num++;
    vTaskDelay(100/portTICK_PERIOD_MS);
  }

  // File is now fully read, close it
  firmware_file.close();

  /* End OTA */
  ret = esp_hosted_slave_ota_end();
  if (ret != ESP_OK) {
    USER_PRINTF("Failed to end OTA: %s\n", esp_err_to_name(ret));
    free(chunk);
    return ESP_HOSTED_SLAVE_OTA_FAILED;
  }

  USER_PRINTLN("LittleFS OTA completed successfully");

  /* Delete firmware file if requested */
  if (delete_after_use) {
    if (LittleFS.remove(firmware_path)) {
      USER_PRINTF("Deleted firmware file: %s\n", firmware_path.c_str());
    } else {
      USER_PRINTF("Failed to delete firmware file: %s\n", firmware_path.c_str());
    }
  }

  /* Clean up allocated memory */
  free(chunk);

  return ESP_HOSTED_SLAVE_OTA_COMPLETED;
}

/**
 * Flash C6 firmware from any POSIX path (SD card, USB, etc.) using fopen() — no LittleFS copy.
 * Returns ESP_OK if flashed and activated (caller must esp_restart()).
 * Returns ESP_ERR_NOT_FOUND if version already matches.
 */
esp_err_t ota_from_path(const char* firmware_path) {
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
  return ESP_ERR_NOT_SUPPORTED;
#else
  USER_PRINTF("C6 OTA: opening %s\n", firmware_path);
  FILE* f = fopen(firmware_path, "rb");
  if (!f) { USER_PRINTF("C6 OTA: cannot open %s\n", firmware_path); return ESP_FAIL; }

  // Validate magic and read version from app description in first segment
  esp_image_header_t        img_hdr  = {};
  esp_image_segment_header_t seg_hdr = {};
  esp_app_desc_t             app_desc = {};
  char new_version[32] = "unknown";

  if (fread(&img_hdr, 1, sizeof(img_hdr), f) != sizeof(img_hdr) ||
      img_hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
    USER_PRINTLN("C6 OTA: invalid image magic");
    fclose(f); return ESP_ERR_INVALID_ARG;
  }
  if (fread(&seg_hdr,  1, sizeof(seg_hdr),  f) == sizeof(seg_hdr) &&
      fread(&app_desc, 1, sizeof(app_desc),  f) == sizeof(app_desc)) {
    strncpy(new_version, app_desc.version, sizeof(new_version) - 1);
  }
  USER_PRINTF("C6 OTA: firmware version in file: %s\n", new_version);

  // Version check — skip if already running this version
#ifndef CONFIG_OTA_VERSION_FORCE_SLAVEFW
  esp_hosted_coprocessor_fwver_t cur = {};
  if (esp_hosted_get_coprocessor_fwversion(&cur) == ESP_OK) {
    char cur_str[32];
    snprintf(cur_str, sizeof(cur_str), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
             cur.major1, cur.minor1, cur.patch1);
    if (strcmp(new_version, cur_str) == 0) {
      USER_PRINTF("C6 OTA: already at %s, skipping\n", cur_str);
      fclose(f); return ESP_ERR_NOT_FOUND;
    }
    USER_PRINTF("C6 OTA: upgrading %s → %s\n", cur_str, new_version);
  }
#endif

  esp_err_t ret = esp_hosted_slave_ota_begin();
  if (ret != ESP_OK) {
    USER_PRINTF("C6 OTA: begin failed: %s\n", esp_err_to_name(ret));
    fclose(f); return ESP_HOSTED_SLAVE_OTA_FAILED;
  }

  uint8_t* chunk = (uint8_t*)heap_caps_malloc_prefer(CHUNK_SIZE, 3,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_DMA,
      MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
  if (!chunk) { fclose(f); return ESP_ERR_NO_MEM; }

  rewind(f);
  size_t n;
  bool ok = true;
  uint32_t chunk_num = 1;
  while ((n = fread(chunk, 1, CHUNK_SIZE, f)) > 0) {
    if (esp_hosted_slave_ota_write(chunk, n) != ESP_OK) {
      USER_PRINTF("C6 OTA: write failed at chunk %lu\n", chunk_num);
      ok = false; break;
    }
    chunk_num++;
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  free(chunk);
  fclose(f);

  if (!ok || esp_hosted_slave_ota_end() != ESP_OK) {
    USER_PRINTLN("C6 OTA: failed"); return ESP_HOSTED_SLAVE_OTA_FAILED;
  }

  USER_PRINTLN("C6 OTA: complete, activating...");
  ret = esp_hosted_slave_ota_activate();
  if (ret != ESP_OK) {
    USER_PRINTF("C6 OTA: activate failed: %s\n", esp_err_to_name(ret));
    return ret;
  }
  return ESP_OK;  // caller must esp_restart()
#endif
}