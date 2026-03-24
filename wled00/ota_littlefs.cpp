/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wled.h" // Includes logging

#ifndef WLED_IDF_BUILD
// Includes for Arduino FS API
#include "FS.h"
#include "LittleFS.h"
#endif

#ifdef WLED_IDF_BUILD
#include <dirent.h>
#include <sys/stat.h>
#define LITTLEFS_BASE "/littlefs"
#endif

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

// ─── Parse image header ───────────────────────────────────────────────────────
// Two overloads: POSIX FILE* for IDF build, Arduino File& for Arduino build.
#ifdef WLED_IDF_BUILD
static esp_err_t parse_image_header_from_file_littlefs(FILE* file, size_t* firmware_size, char* app_version_str, size_t version_str_len) {
  esp_image_header_t image_header;
  esp_image_segment_header_t segment_header;
  esp_app_desc_t app_desc;
  size_t offset = 0;
  size_t total_size = 0;

  if (!file) { USER_PRINTLN("Invalid file passed to parser"); return ESP_ERR_INVALID_ARG; }
  fseek(file, 0, SEEK_SET);

  if (fread(&image_header, 1, sizeof(image_header), file) != sizeof(image_header)) {
    USER_PRINTLN("Failed to read image header"); return ESP_FAIL;
  }
  if (image_header.magic != ESP_IMAGE_HEADER_MAGIC) {
    USER_PRINTF("Invalid image magic: 0x%" PRIx8 "\n", image_header.magic); return ESP_ERR_INVALID_ARG;
  }
  USER_PRINTF("Image header: magic=0x%" PRIx8 ", segments=%" PRIu8 ", hash=%" PRIu8 "\n",
              image_header.magic, image_header.segment_count, image_header.hash_appended);

  offset = sizeof(image_header);
  total_size = sizeof(image_header);
  for (int i = 0; i < image_header.segment_count; i++) {
    if (fseek(file, (long)offset, SEEK_SET) != 0 ||
        fread(&segment_header, 1, sizeof(segment_header), file) != sizeof(segment_header)) {
      USER_PRINTF("Failed to read segment %d header\n", i); return ESP_FAIL;
    }
    USER_PRINTF("Segment %d: data_len=%" PRIu32 " load_addr=0x%" PRIx32 "\n",
                i, segment_header.data_len, segment_header.load_addr);
    total_size += sizeof(segment_header) + segment_header.data_len;
    offset     += sizeof(segment_header) + segment_header.data_len;
    if (i == 0) {
      size_t app_desc_offset = sizeof(image_header) + sizeof(segment_header);
      if (fseek(file, (long)app_desc_offset, SEEK_SET) == 0 &&
          fread(&app_desc, 1, sizeof(app_desc), file) == sizeof(app_desc)) {
        strncpy(app_version_str, app_desc.version, version_str_len - 1);
        app_version_str[version_str_len - 1] = '\0';
        USER_PRINTF("App version='%s' project='%s'\n", app_desc.version, app_desc.project_name);
      } else {
        strncpy(app_version_str, "unknown", version_str_len - 1);
        app_version_str[version_str_len - 1] = '\0';
      }
    }
  }
  size_t padding = (16 - (total_size % 16)) % 16;
  total_size += padding + 1 + ((image_header.hash_appended == 1) ? 32 : 0);
  *firmware_size = total_size;
  USER_PRINTF("Total image size: %u bytes\n", (unsigned int)*firmware_size);
  return ESP_OK;
}

static String find_latest_firmware_littlefs() {
  USER_PRINTLN("Opening " LITTLEFS_BASE " ...");
  DIR* dir = opendir(LITTLEFS_BASE);
  if (!dir) { USER_PRINTLN("Failed to open LittleFS root"); return ""; }
  String latest_file = "";
  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_type == DT_DIR) continue;
    String fileName = ent->d_name;
    if (fileName.startsWith("network_adapter") && fileName.endsWith(".bin")) {
      USER_PRINTF("Found update file: %s\n", fileName.c_str());
      latest_file = fileName;
      break;
    }
  }
  closedir(dir);
  if (latest_file.length() == 0) USER_PRINTLN("No network_adapter*.bin found in LittleFS root.");
  return latest_file;
}

esp_err_t ota_littlefs_perform(bool delete_after_use) {
  String latest_filename = find_latest_firmware_littlefs();
  if (latest_filename.length() == 0) { USER_PRINTLN("No firmware file found"); return ESP_HOSTED_SLAVE_OTA_FAILED; }

  String firmware_path = String(LITTLEFS_BASE) + "/" + latest_filename;
  USER_PRINTF("Firmware path: %s\n", firmware_path.c_str());

  uint8_t* chunk = (uint8_t*)heap_caps_malloc_prefer(CHUNK_SIZE, 3,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_DMA,
      MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
  if (!chunk) { USER_PRINTLN("Failed to allocate chunk buffer"); return ESP_ERR_NO_MEM; }

  FILE* f = fopen(firmware_path.c_str(), "rb");
  if (!f) { free(chunk); USER_PRINTF("Failed to open %s\n", firmware_path.c_str()); return ESP_FAIL; }

  size_t firmware_size;
  char new_app_version[32];
  esp_err_t ret = parse_image_header_from_file_littlefs(f, &firmware_size, new_app_version, sizeof(new_app_version));
  if (ret != ESP_OK) {
    fclose(f); free(chunk);
    USER_PRINTF("Failed to parse image header: %s\n", esp_err_to_name(ret));
    return ESP_HOSTED_SLAVE_OTA_FAILED;
  }
  USER_PRINTF("Firmware verified - Size: %u bytes, Version: %s\n", (unsigned int)firmware_size, new_app_version);

  #ifndef CONFIG_OTA_VERSION_FORCE_SLAVEFW
  esp_hosted_coprocessor_fwver_t current_slave_version = { 0 };
  esp_err_t version_ret = esp_hosted_get_coprocessor_fwversion(&current_slave_version);
  if (version_ret == ESP_OK) {
    char current_version_str[32];
    snprintf(current_version_str, sizeof(current_version_str), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
             current_slave_version.major1, current_slave_version.minor1, current_slave_version.patch1);
    if (strcmp(new_app_version, current_version_str) == 0) {
      USER_PRINTF("Already at version %s, skipping OTA.\n", current_version_str);
      fclose(f); free(chunk); return ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED;
    }
    USER_PRINTF("OTA: %s -> %s\n", current_version_str, new_app_version);
  }
  #endif

  ret = esp_hosted_slave_ota_begin();
  if (ret != ESP_OK) {
    USER_PRINTF("Failed to begin OTA: %s\n", esp_err_to_name(ret));
    fclose(f); free(chunk); return ESP_HOSTED_SLAVE_OTA_FAILED;
  }

  rewind(f);
  size_t bytes_read;
  uint32_t chunk_num = 1;
  while ((bytes_read = fread(chunk, 1, CHUNK_SIZE, f)) > 0) {
    ret = esp_hosted_slave_ota_write(chunk, bytes_read);
    if (ret != ESP_OK) {
      USER_PRINTF("OTA write failed at chunk %lu: %s\n", chunk_num, esp_err_to_name(ret));
      fclose(f); free(chunk); return ESP_HOSTED_SLAVE_OTA_FAILED;
    }
    chunk_num++;
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  fclose(f);
  free(chunk);

  ret = esp_hosted_slave_ota_end();
  if (ret != ESP_OK) {
    USER_PRINTF("OTA end failed: %s\n", esp_err_to_name(ret)); return ESP_HOSTED_SLAVE_OTA_FAILED;
  }
  USER_PRINTLN("LittleFS OTA completed successfully");

  if (delete_after_use) {
    char del_path[256];
    snprintf(del_path, sizeof(del_path), "%s/%s", LITTLEFS_BASE, latest_filename.c_str());
    if (remove(del_path) == 0) { USER_PRINTF("Deleted %s\n", del_path); }
    else                       { USER_PRINTF("Failed to delete %s\n", del_path); }
  }
  return ESP_HOSTED_SLAVE_OTA_COMPLETED;
}

#else  // ── Arduino build: original LittleFS/File versions ───────────────────

static esp_err_t parse_image_header_from_file_littlefs(File& file, size_t* firmware_size, char* app_version_str, size_t version_str_len) {
  esp_image_header_t image_header;
  esp_image_segment_header_t segment_header;
  esp_app_desc_t app_desc;
  size_t offset = 0;
  size_t total_size = 0;

  if (!file) { USER_PRINTLN("Invalid file object passed to parser"); return ESP_ERR_INVALID_ARG; }
  file.seek(0, SeekSet);

  if (file.read((uint8_t*)&image_header, sizeof(image_header)) != sizeof(image_header)) {
    USER_PRINTLN("Failed to read image header from file"); return ESP_FAIL;
  }
  if (image_header.magic != ESP_IMAGE_HEADER_MAGIC) {
    USER_PRINTF("Invalid image magic: 0x%" PRIx8 "\n", image_header.magic); return ESP_ERR_INVALID_ARG;
  }
  USER_PRINTF("Image header: magic=0x%" PRIx8 ", segment_count=%" PRIu8 ", hash_appended=%" PRIu8 "\n",
              image_header.magic, image_header.segment_count, image_header.hash_appended);

  offset = sizeof(image_header);
  total_size = sizeof(image_header);
  for (int i = 0; i < image_header.segment_count; i++) {
    if (!file.seek(offset, SeekSet) ||
        file.read((uint8_t*)&segment_header, sizeof(segment_header)) != sizeof(segment_header)) {
      USER_PRINTF("Failed to read segment %d header\n", i); return ESP_FAIL;
    }
    USER_PRINTF("Segment %d: data_len=%" PRIu32 ", load_addr=0x%" PRIx32 "\n",
                i, segment_header.data_len, segment_header.load_addr);
    total_size += sizeof(segment_header) + segment_header.data_len;
    offset     += sizeof(segment_header) + segment_header.data_len;
    if (i == 0) {
      size_t app_desc_offset = sizeof(image_header) + sizeof(segment_header);
      if (file.seek(app_desc_offset, SeekSet) &&
          file.read((uint8_t*)&app_desc, sizeof(app_desc)) == sizeof(app_desc)) {
        strncpy(app_version_str, app_desc.version, version_str_len - 1);
        app_version_str[version_str_len - 1] = '\0';
        USER_PRINTF("Found app description: version='%s', project_name='%s'\n", app_desc.version, app_desc.project_name);
      } else {
        USER_PRINTLN("Failed to read app description");
        strncpy(app_version_str, "unknown", version_str_len - 1);
        app_version_str[version_str_len - 1] = '\0';
      }
    }
  }
  size_t padding = (16 - (total_size % 16)) % 16;
  if (padding > 0) { USER_PRINTF("Adding %u bytes padding\n", (unsigned int)padding); total_size += padding; }
  total_size += 1;
  if (image_header.hash_appended == 1) total_size += 32;
  *firmware_size = total_size;
  USER_PRINTF("Total image size: %u bytes\n", (unsigned int)*firmware_size);
  return ESP_OK;
}

static String find_latest_firmware_littlefs() {
  USER_PRINTLN("Opening root directory / ...");
  File root = LittleFS.open("/");
  if (!root) { USER_PRINTLN("Failed to open / directory"); return ""; }
  String latest_file = "";
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String fileName = file.name();
      if (fileName.startsWith("network_adapter") && fileName.endsWith(".bin")) {
        USER_PRINTF("Found update file: %s, size: %lu\n", fileName.c_str(), file.size());
        latest_file = fileName;
        break;
      }
    }
    file.close();
    file = root.openNextFile();
  }
  if (latest_file.length() == 0) USER_PRINTLN("No .bin files found in / directory.");
  if (file) file.close();
  root.close();
  return latest_file;
}

esp_err_t ota_littlefs_perform(bool delete_after_use) {
  String latest_filename;
  String firmware_path;
  File firmware_file;
  uint8_t* chunk = (uint8_t*)heap_caps_malloc_prefer(CHUNK_SIZE, 3,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_DMA,
      MALLOC_CAP_SPIRAM, MALLOC_CAP_INTERNAL);
  size_t bytes_read;
  esp_err_t ret = ESP_OK;

  if (!chunk) { USER_PRINTLN("Failed to allocate chunk buffer"); return ESP_ERR_NO_MEM; }
  USER_PRINTLN("Starting WiFi CoProcessor OTA process...");

  latest_filename = find_latest_firmware_littlefs();
  if (latest_filename.length() == 0) { free(chunk); return ESP_HOSTED_SLAVE_OTA_FAILED; }

  firmware_path = "/" + latest_filename;
  USER_PRINTF("Firmware file found: %s\n", firmware_path.c_str());

  firmware_file = LittleFS.open(firmware_path, "r");
  if (!firmware_file) { free(chunk); USER_PRINTF("Failed to open: %s\n", firmware_path.c_str()); return ESP_FAIL; }

  size_t firmware_size;
  char new_app_version[32];
  ret = parse_image_header_from_file_littlefs(firmware_file, &firmware_size, new_app_version, sizeof(new_app_version));
  firmware_file.close();
  if (ret != ESP_OK) { free(chunk); return ESP_HOSTED_SLAVE_OTA_FAILED; }
  USER_PRINTF("Firmware verified - Size: %u bytes, Version: %s\n", (unsigned int)firmware_size, new_app_version);

  #ifndef CONFIG_OTA_VERSION_FORCE_SLAVEFW
  esp_hosted_coprocessor_fwver_t current_slave_version = { 0 };
  esp_err_t version_ret = esp_hosted_get_coprocessor_fwversion(&current_slave_version);
  if (version_ret == ESP_OK) {
    char current_version_str[32];
    snprintf(current_version_str, sizeof(current_version_str), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
             current_slave_version.major1, current_slave_version.minor1, current_slave_version.patch1);
    if (strcmp(new_app_version, current_version_str) == 0) {
      firmware_file.close(); free(chunk); return ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED;
    }
    USER_PRINTF("OTA: %s -> %s\n", current_version_str, new_app_version);
  }
  #endif

  ret = esp_hosted_slave_ota_begin();
  if (ret != ESP_OK) { firmware_file.close(); free(chunk); return ESP_HOSTED_SLAVE_OTA_FAILED; }

  firmware_file = LittleFS.open(firmware_path, "r");
  if (!firmware_file) { free(chunk); return ESP_FAIL; }

  uint32_t chunk_num = 1;
  while ((bytes_read = firmware_file.read(chunk, CHUNK_SIZE)) > 0) {
    ret = esp_hosted_slave_ota_write(chunk, bytes_read);
    if (ret != ESP_OK) { firmware_file.close(); free(chunk); return ESP_HOSTED_SLAVE_OTA_FAILED; }
    chunk_num++;
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  firmware_file.close();

  ret = esp_hosted_slave_ota_end();
  if (ret != ESP_OK) { free(chunk); return ESP_HOSTED_SLAVE_OTA_FAILED; }
  USER_PRINTLN("LittleFS OTA completed successfully");

  if (delete_after_use) {
    if (LittleFS.remove(firmware_path)) USER_PRINTF("Deleted: %s\n", firmware_path.c_str());
    else                                USER_PRINTF("Failed to delete: %s\n", firmware_path.c_str());
  }
  free(chunk);
  return ESP_HOSTED_SLAVE_OTA_COMPLETED;
}
#endif  // WLED_IDF_BUILD

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