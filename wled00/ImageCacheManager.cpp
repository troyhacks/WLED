#include "ImageCacheManager.h"
#ifdef CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>


static const char* TAG = "ImageCache";

ImageCacheManager& ImageCacheManager::getInstance() {
  static ImageCacheManager instance;
  return instance;
}

ImageCacheManager::ImageCacheManager() :
  sync_task_handle(NULL),
  current_status(CacheStatus::IDLE),
  psram_limit(0),
  psram_used(0) {
  cache_mutex = xSemaphoreCreateMutex();
  status_events = xEventGroupCreate();
  xEventGroupSetBits(status_events, EVT_IDLE); // Start idle
#ifdef CONFIG_SPIRAM
  size_t total_psram = esp_psram_get_size();
#else
  size_t total_psram = 0;
#endif
  psram_limit = static_cast<size_t>(total_psram * 0.8);
  ESP_LOGI(TAG, "Total PSRAM: %u bytes, Cache Limit (80%%): %u bytes", total_psram, psram_limit);
}

ImageCacheManager::~ImageCacheManager() {
  clearCache();
  if (status_events) {
    vEventGroupDelete(status_events);
  }
  vSemaphoreDelete(cache_mutex);
}

// ============================================================================
// Helper: Detect if path is an MJPEG file
// ============================================================================

bool ImageCacheManager::_isMJPEGFile(const psram_string& path) {
  return path.size() > 6 && path.substr(path.size() - 6) == ".mjpeg";
}

// ============================================================================
// MJPEG Whole-File Caching
// ============================================================================

bool ImageCacheManager::_ensureMJPEGCached(const psram_string& file_path) {
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  if (mjpeg_cache.find(file_path) != mjpeg_cache.end()) {
    xSemaphoreGive(cache_mutex);
    return true;
  }
  xSemaphoreGive(cache_mutex);

  // Get file size
  struct stat st;
  if (stat(file_path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "Failed to stat MJPEG: %s", file_path.c_str());
    return false;
  }
  size_t file_size = st.st_size;

  // Check if we have room
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  bool has_room = (psram_used + file_size <= psram_limit);
  xSemaphoreGive(cache_mutex);

  if (!has_room) {
    ESP_LOGW(TAG, "Not enough PSRAM for MJPEG: %s (%u bytes needed, %u available)",
      file_path.c_str(), file_size, psram_limit - psram_used);
    return false;
  }

  // Allocate buffer
  uint8_t* buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes for MJPEG", file_size);
    return false;
  }

  // Load entire file
  FILE* fp = fopen(file_path.c_str(), "rb");
  if (!fp) {
    free(buffer);
    ESP_LOGE(TAG, "Failed to open MJPEG: %s", file_path.c_str());
    return false;
  }

  current_status = CacheStatus::PRELOADING_BG;

  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  current_loading_file = file_path;
  xSemaphoreGive(cache_mutex);

  ESP_LOGI(TAG, "Loading MJPEG: %s (%u bytes)", file_path.c_str(), file_size);

  size_t bytes_read = fread(buffer, 1, file_size, fp);
  fclose(fp);

  if (bytes_read != file_size) {
    free(buffer);
    ESP_LOGE(TAG, "Incomplete MJPEG read: %u/%u", bytes_read, file_size);
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    current_loading_file = "";
    xSemaphoreGive(cache_mutex);
    current_status = CacheStatus::IDLE;
    return false;
  }

  // Build frame index by scanning buffer (fast - it's in memory now)
  std::vector<MJPEGFrameInfo, PSRAM_Allocator<MJPEGFrameInfo>> frames;

  size_t frame_start = 0;
  bool in_frame = false;

  for (size_t i = 1; i < file_size; i++) {
    if (!in_frame) {
      // Look for SOI marker (0xFFD8)
      if (buffer[i - 1] == 0xFF && buffer[i] == 0xD8) {
        frame_start = i - 1;
        in_frame = true;
      }
    } else {
      // Look for EOI marker (0xFFD9)
      if (buffer[i - 1] == 0xFF && buffer[i] == 0xD9) {
        size_t frame_size = i - frame_start + 1;
        frames.push_back({ frame_start, frame_size });
        in_frame = false;
      }
    }
  }

  if (frames.empty()) {
    free(buffer);
    ESP_LOGE(TAG, "No frames found in MJPEG: %s", file_path.c_str());
    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    current_loading_file = "";
    xSemaphoreGive(cache_mutex);
    current_status = CacheStatus::IDLE;
    return false;
  }

  // Store in cache
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  mjpeg_cache[file_path] = { buffer, file_size, std::move(frames) };
  psram_used += file_size;
  current_loading_file = "";
  xSemaphoreGive(cache_mutex);

  current_status = CacheStatus::IDLE;

  ESP_LOGI(TAG, "Cached MJPEG %s: %u bytes, %u frames",
    file_path.c_str(), file_size, mjpeg_cache[file_path].frames.size());

  return true;
}

size_t ImageCacheManager::_getMJPEGFrameCount(const psram_string& file_path) {
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  auto it = mjpeg_cache.find(file_path);
  size_t count = (it != mjpeg_cache.end()) ? it->second.frames.size() : 0;
  xSemaphoreGive(cache_mutex);
  return count;
}

ImageResult ImageCacheManager::_getImageFromMJPEG(const psram_string& file_path, size_t index) {
  ImageResult result = { nullptr, 0, false, false };

  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  auto it = mjpeg_cache.find(file_path);
  if (it == mjpeg_cache.end()) {
    xSemaphoreGive(cache_mutex);
    return result;
  }

  if (index >= it->second.frames.size()) {
    xSemaphoreGive(cache_mutex);
    ESP_LOGW(TAG, "Frame index %d out of range for %s", index, file_path.c_str());
    return result;
  }

  const MJPEGFrameInfo& frame = it->second.frames[index];

  // Return pointer directly into cached buffer - no copy, no allocation!
  result.buffer = it->second.buffer + frame.offset;
  result.size = frame.size;
  result.from_cache = true;
  result.needs_free = false;

  xSemaphoreGive(cache_mutex);
  return result;
}

// ============================================================================
// File List Management
// ============================================================================

bool ImageCacheManager::_ensureFileListCached(const psram_string& folder_path) {
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  if (folder_file_lists.find(folder_path) != folder_file_lists.end()) {
    xSemaphoreGive(cache_mutex);
    return true;
  }
  xSemaphoreGive(cache_mutex);

  DIR* dir = opendir(folder_path.c_str());
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open directory: %s", folder_path.c_str());
    return false;
  }

  psram_string_vector filenames;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    psram_string name = entry->d_name;
    if (name.find(".jpg") != psram_string::npos || name.find(".jpeg") != psram_string::npos) {
      filenames.push_back(name);
    }
  }
  closedir(dir);

  std::sort(filenames.begin(), filenames.end());

  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  folder_file_lists[folder_path] = std::move(filenames);
  ESP_LOGI(TAG, "Cached file list for %s: %d files", folder_path.c_str(), folder_file_lists[folder_path].size());
  xSemaphoreGive(cache_mutex);

  return true;
}

psram_string ImageCacheManager::_getFilenameByIndex(const psram_string& folder_path, size_t index) {
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  auto it = folder_file_lists.find(folder_path);
  if (it != folder_file_lists.end() && index < it->second.size()) {
    psram_string result = it->second[index];
    xSemaphoreGive(cache_mutex);
    return result;
  }
  xSemaphoreGive(cache_mutex);
  return "";
}

size_t ImageCacheManager::getFolderSizeFromDisk(const std::string& folder_path) {
  psram_string ps_path = folder_path.c_str();

  // Handle MJPEG files
  if (_isMJPEGFile(ps_path)) {
    _ensureMJPEGCached(ps_path);
    return _getMJPEGFrameCount(ps_path);
  }

  // Handle JPEG folders
  _ensureFileListCached(ps_path);

  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  size_t size = 0;
  auto it = folder_file_lists.find(ps_path);
  if (it != folder_file_lists.end()) {
    size = it->second.size();
  }
  xSemaphoreGive(cache_mutex);
  return size;
}

// ============================================================================
// Streaming API
// ============================================================================

ImageResult ImageCacheManager::getImageStreaming(const std::string& folder_path, size_t index) {
  psram_string ps_path = folder_path.c_str();

  // Handle MJPEG files
  if (_isMJPEGFile(ps_path)) {
    if (!_ensureMJPEGCached(ps_path)) {
      return { nullptr, 0, false, false };
    }
    return _getImageFromMJPEG(ps_path, index);
  }

  // Handle JPEG folders
  ImageResult result = { nullptr, 0, false, false };

  if (!_ensureFileListCached(ps_path)) {
    return result;
  }

  // Fast path: check cache
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  ImageData* cached = _getImageByIndex(ps_path, index);
  if (cached) {
    result.buffer = cached->buffer;
    result.size = cached->size;
    result.from_cache = true;
    result.needs_free = false;
    xSemaphoreGive(cache_mutex);
    return result;
  }
  xSemaphoreGive(cache_mutex);

  // Load from disk
  psram_string filename = _getFilenameByIndex(ps_path, index);
  if (filename.empty()) {
    ESP_LOGW(TAG, "No filename at index %d for %s", index, folder_path.c_str());
    return result;
  }

  psram_string full_path = ps_path + "/" + filename;

  struct stat st;
  if (stat(full_path.c_str(), &st) != 0) {
    ESP_LOGE(TAG, "Failed to stat: %s", full_path.c_str());
    return result;
  }

  size_t file_size = st.st_size;

  uint8_t* buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
  if (!buffer && file_size <= 8192) {
    buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_DEFAULT);
  }
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes for %s", file_size, filename.c_str());
    return result;
  }

  FILE* file = fopen(full_path.c_str(), "rb");
  if (!file) {
    free(buffer);
    ESP_LOGE(TAG, "Failed to open: %s", full_path.c_str());
    return result;
  }

  size_t bytes_read = fread(buffer, 1, file_size, file);
  fclose(file);

  if (bytes_read != file_size) {
    free(buffer);
    ESP_LOGE(TAG, "Incomplete read: %s (%u/%u)", filename.c_str(), bytes_read, file_size);
    return result;
  }

  result.buffer = buffer;
  result.size = file_size;
  result.from_cache = false;
  result.needs_free = true;

  // Cache for next time
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  if (psram_used + file_size <= psram_limit) {
    psram_file_map& cached_files = image_cache[ps_path];
    if (cached_files.find(filename) == cached_files.end()) {
      uint8_t* cache_buffer = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
      if (cache_buffer) {
        memcpy(cache_buffer, buffer, file_size);
        cached_files[filename] = { cache_buffer, file_size, st.st_mtime };
        psram_used += file_size;
      }
    }
  }
  xSemaphoreGive(cache_mutex);

  _queueBackgroundSync(ps_path);

  return result;
}

// ============================================================================
// Background Sync
// ============================================================================

void ImageCacheManager::_queueBackgroundSync(const psram_string& path) {
  // Skip MJPEG files - they're loaded in full on first access
  if (_isMJPEGFile(path)) {
    return;
  }

  xSemaphoreTake(cache_mutex, portMAX_DELAY);

  bool already_queued = std::find(pending_sync_folders.begin(),
    pending_sync_folders.end(), path) != pending_sync_folders.end();

  if (!already_queued) {
    auto file_list_it = folder_file_lists.find(path);
    auto cache_it = image_cache.find(path);

    bool fully_cached = false;
    if (file_list_it != folder_file_lists.end() && cache_it != image_cache.end()) {
      fully_cached = (cache_it->second.size() >= file_list_it->second.size());
    }

    if (!fully_cached) {
      pending_sync_folders.push_back(path);
    }
  }
  xSemaphoreGive(cache_mutex);

  if (sync_task_handle == NULL) {
    xTaskCreatePinnedToCore(_backgroundSyncTask, "cache_sync", 4096, this,
      IMAGECACHE_BG_PRIORITY, &sync_task_handle, 0);
  }
}

void ImageCacheManager::_backgroundSyncTask(void* params) {
  ImageCacheManager* manager = static_cast<ImageCacheManager*>(params);
  manager->current_status = CacheStatus::PRELOADING_BG;

  while (true) {
    psram_string folder_to_sync;

    xSemaphoreTake(manager->cache_mutex, portMAX_DELAY);
    if (manager->pending_sync_folders.empty()) {
      xSemaphoreGive(manager->cache_mutex);
      break;
    }
    folder_to_sync = manager->pending_sync_folders.front();
    manager->pending_sync_folders.erase(manager->pending_sync_folders.begin());
    xSemaphoreGive(manager->cache_mutex);

    manager->_synchronizeFolder(folder_to_sync);
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  xEventGroupSetBits(manager->status_events, EVT_IDLE);
  
  manager->current_status = CacheStatus::IDLE;
  xSemaphoreTake(manager->cache_mutex, portMAX_DELAY);
  manager->current_loading_file = "";
  xSemaphoreGive(manager->cache_mutex);

  manager->sync_task_handle = NULL;
  vTaskDelete(NULL);
}

void ImageCacheManager::_synchronizeFolder(const psram_string& folder_path) {
  DIR* dir = opendir(folder_path.c_str());
  if (!dir) return;

  std::map<psram_string, time_t, std::less<psram_string>,
    PSRAM_Allocator<std::pair<const psram_string, time_t>>> files_on_disk;
  struct dirent* entry;
  struct stat st;

  while ((entry = readdir(dir)) != nullptr) {
    psram_string name = entry->d_name;
    if (name.find(".jpg") != psram_string::npos || name.find(".jpeg") != psram_string::npos) {
      psram_string full_path = folder_path + "/" + name;
      if (stat(full_path.c_str(), &st) == 0) {
        files_on_disk[name] = st.st_mtime;
      }
    }
  }
  closedir(dir);

  for (const auto& disk_file_pair : files_on_disk) {
    const psram_string& filename = disk_file_pair.first;
    const time_t& disk_mtime = disk_file_pair.second;

    xSemaphoreTake(cache_mutex, portMAX_DELAY);
    current_loading_file = folder_path + "/" + filename;
    psram_file_map& cached_files = image_cache[folder_path];
    auto cache_it = cached_files.find(filename);
    bool needs_load = (cache_it == cached_files.end()) || (disk_mtime > cache_it->second.mtime);

    if (needs_load && cache_it != cached_files.end()) {
      psram_used -= cache_it->second.size;
      free(cache_it->second.buffer);
      cached_files.erase(cache_it);
    }
    xSemaphoreGive(cache_mutex);

    if (needs_load) {
      psram_string full_path = folder_path + "/" + filename;
      stat(full_path.c_str(), &st);
      size_t size = st.st_size;

      if (psram_used + size > psram_limit) continue;

      FILE* file = fopen(full_path.c_str(), "rb");
      uint8_t* buffer = nullptr;
      if (file) {
        buffer = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (buffer) fread(buffer, 1, size, file);
        fclose(file);
      }

      if (buffer) {
        xSemaphoreTake(cache_mutex, portMAX_DELAY);
        cached_files[filename] = { buffer, size, disk_mtime };
        psram_used += size;
        xSemaphoreGive(cache_mutex);
      }

      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  current_loading_file = "";
  xSemaphoreGive(cache_mutex);
}

ImageData* ImageCacheManager::_getImageByIndex(const psram_string& folder_path, size_t index) {
  auto list_it = folder_file_lists.find(folder_path);
  auto cache_it = image_cache.find(folder_path);

  if (list_it != folder_file_lists.end() && cache_it != image_cache.end()) {
    if (index < list_it->second.size()) {
      const psram_string& filename = list_it->second[index];
      auto file_it = cache_it->second.find(filename);
      if (file_it != cache_it->second.end()) {
        return &file_it->second;
      }
    }
  }
  return nullptr;
}

void ImageCacheManager::startPreload(const std::string& root_path) {
  xEventGroupClearBits(status_events, EVT_IDLE);
  DIR* dir = opendir(root_path.c_str());
  if (!dir) {
    ESP_LOGE(TAG, "Failed to open root directory: %s", root_path.c_str());
    return;
  }

  psram_string_vector items;
  struct dirent* entry;
  struct stat st;

  while ((entry = readdir(dir)) != nullptr) {
    psram_string name = entry->d_name;
    psram_string full_path = psram_string(root_path.c_str()) + "/" + name;

    if (stat(full_path.c_str(), &st) != 0) continue;

    // Check for MJPEG files
    if (S_ISREG(st.st_mode) &&
      name.rfind("sequence", 0) == 0 &&
      name.size() > 6 &&
      name.substr(name.size() - 6) == ".mjpeg") {
      items.push_back(name);
    }
    // Check for JPEG sequence folders
    else if (S_ISDIR(st.st_mode) && name.rfind("sequence", 0) == 0) {
      items.push_back(name);
    }
  }
  closedir(dir);

  // Sort with _hot items first
  std::sort(items.begin(), items.end(), [](const psram_string& a, const psram_string& b) {
    auto is_hot = [](const psram_string& s) {
      if (s.size() >= 11 && s.substr(s.size() - 11) == "_hot.mjpeg") return true;
      if (s.size() >= 4 && s.substr(s.size() - 4) == "_hot") return true;
      return false;
      };
    bool a_hot = is_hot(a);
    bool b_hot = is_hot(b);
    if (a_hot != b_hot) return a_hot;
    return a < b;
    });

  // Queue all items for preload
  psram_string ps_root = root_path.c_str();
  for (const auto& item : items) {
    psram_string full_path = ps_root + "/" + item;

    if (_isMJPEGFile(full_path)) {
      // Load MJPEG files immediately (they load as a single operation)
      _ensureMJPEGCached(full_path);
    } else {
      // Queue folders for background sync
      _ensureFileListCached(full_path);
      _queueBackgroundSync(full_path);
    }
  }

  ESP_LOGI(TAG, "Queued %d items for preload", items.size());
}

CacheStatus ImageCacheManager::getStatus() {
  return current_status;
}

psram_string ImageCacheManager::getCurrentFile() {
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  psram_string file = current_loading_file;
  xSemaphoreGive(cache_mutex);
  return file;
}

size_t ImageCacheManager::getCacheUsedBytes() {
  xSemaphoreTake(cache_mutex, portMAX_DELAY);
  size_t used = psram_used;
  xSemaphoreGive(cache_mutex);
  return used;
}

void ImageCacheManager::clearCache() {
  xSemaphoreTake(cache_mutex, portMAX_DELAY);

  // Clear JPEG folder cache
  for (auto& pair : image_cache) {
    for (auto& file_pair : pair.second) {
      free(file_pair.second.buffer);
    }
  }
  image_cache.clear();
  folder_file_lists.clear();

  // Clear MJPEG cache
  for (auto& pair : mjpeg_cache) {
    free(pair.second.buffer);
  }
  mjpeg_cache.clear();

  pending_sync_folders.clear();
  psram_used = 0;
  xSemaphoreGive(cache_mutex);
}

bool ImageCacheManager::waitUntilIdle(TickType_t timeout_ticks) {
  EventBits_t bits = xEventGroupWaitBits(
    status_events,
    EVT_IDLE,
    pdFALSE,  // Don't clear on exit
    pdTRUE,   // Wait for all bits
    timeout_ticks
  );
  return (bits & EVT_IDLE) != 0;
}