#include "luce/boot_state.h"

#include <cinttypes>
#include <cstdio>

#if LUCE_HAS_NVS

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string>

constexpr const char* kTag = "luce_boot";

const char* nvs_type_name(nvs_type_t type) {
  switch (type) {
    case NVS_TYPE_U8:
      return "U8";
    case NVS_TYPE_I8:
      return "I8";
    case NVS_TYPE_U16:
      return "U16";
    case NVS_TYPE_I16:
      return "I16";
    case NVS_TYPE_U32:
      return "U32";
    case NVS_TYPE_I32:
      return "I32";
    case NVS_TYPE_U64:
      return "U64";
    case NVS_TYPE_I64:
      return "I64";
    case NVS_TYPE_STR:
      return "STR";
    case NVS_TYPE_BLOB:
      return "BLOB";
    default:
      return "OTHER";
  }
}

void dump_nvs_value(nvs_handle_t handle, const nvs_entry_info_t& info) {
  switch (info.type) {
    case NVS_TYPE_U8: {
      uint8_t v = 0;
      if (nvs_get_u8(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u8): %u", v);
      }
      break;
    }
    case NVS_TYPE_I8: {
      int8_t v = 0;
      if (nvs_get_i8(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i8): %d", v);
      }
      break;
    }
    case NVS_TYPE_U16: {
      uint16_t v = 0;
      if (nvs_get_u16(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u16): %u", v);
      }
      break;
    }
    case NVS_TYPE_I16: {
      int16_t v = 0;
      if (nvs_get_i16(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i16): %d", v);
      }
      break;
    }
    case NVS_TYPE_U32: {
      uint32_t v = 0;
      if (nvs_get_u32(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u32): %lu", (unsigned long)v);
      }
      break;
    }
    case NVS_TYPE_I32: {
      int32_t v = 0;
      if (nvs_get_i32(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i32): %ld", (long)v);
      }
      break;
    }
    case NVS_TYPE_U64: {
      uint64_t v = 0;
      if (nvs_get_u64(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (u64): 0x%016" PRIx64, v);
      }
      break;
    }
    case NVS_TYPE_I64: {
      int64_t v = 0;
      if (nvs_get_i64(handle, info.key, &v) == ESP_OK) {
        ESP_LOGI(kTag, "    value (i64): %lld", (long long)v);
      }
      break;
    }
    case NVS_TYPE_STR: {
      size_t required = 0;
      if (nvs_get_str(handle, info.key, nullptr, &required) == ESP_OK && required > 0) {
        char str_val[129] = {0};
        size_t capacity = required < sizeof(str_val) ? required : sizeof(str_val);
        if (nvs_get_str(handle, info.key, str_val, &capacity) == ESP_OK) {
          ESP_LOGI(kTag, "    value (str): %s", str_val);
        }
      }
      break;
    }
    case NVS_TYPE_BLOB: {
      size_t required = 0;
      if (nvs_get_blob(handle, info.key, nullptr, &required) == ESP_OK && required > 0) {
        size_t copy_size = required < 32 ? required : 32;
        uint8_t data[32] = {0};
        if (nvs_get_blob(handle, info.key, data, &copy_size) == ESP_OK && copy_size > 0) {
          char blob_preview[33] = {0};
          for (size_t i = 0; i < copy_size; ++i) {
            std::snprintf(blob_preview + (i * 2), 3, "%02x", data[i]);
          }
          ESP_LOGI(kTag, "    value (blob, %u bytes): %s%s", (unsigned)required, blob_preview,
                   required > sizeof(data) ? "..." : "");
        }
      }
      break;
    }
    default:
      ESP_LOGW(kTag, "    value unsupported for type=%s", nvs_type_name(info.type));
      break;
  }
}

void dump_nvs_entries() {
  nvs_iterator_t iterator = nullptr;
  esp_err_t err = nvs_entry_find(nullptr, nullptr, NVS_TYPE_ANY, &iterator);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kTag, "NVS: no entries found");
    return;
  }
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS entry scan failed: %s", esp_err_to_name(err));
    return;
  }

  ESP_LOGI(kTag, "NVS entries (namespace/key/type/value):");
  nvs_handle_t current = 0;
  std::string current_ns;
  while (true) {
    nvs_entry_info_t entry;
    if (nvs_entry_info(iterator, &entry) != ESP_OK) {
      break;
    }

    if (current_ns != entry.namespace_name) {
      if (current != 0) {
        nvs_close(current);
        current = 0;
      }
      if (nvs_open(entry.namespace_name, NVS_READONLY, &current) != ESP_OK) {
        ESP_LOGW(kTag, "NVS: failed to open namespace '%s'", entry.namespace_name);
        current = 0;
      }
      current_ns.assign(entry.namespace_name);
    }

    ESP_LOGI(kTag, "  ns=%s key=%s type=%s", entry.namespace_name, entry.key,
             nvs_type_name(entry.type));
    if (current != 0) {
      dump_nvs_value(current, entry);
    }

    err = nvs_entry_next(&iterator);
    if (err != ESP_OK) {
      break;
    }
  }

  if (current != 0) {
    nvs_close(current);
  }
  nvs_release_iterator(iterator);
  if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_OK) {
    ESP_LOGW(kTag, "NVS entry scan ended with error: %s", esp_err_to_name(err));
  }
}

void update_boot_state_record() {
  ESP_LOGI(kTag, "NVS init: starting");
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(kTag, "NVS init: nvs_flash_init returned %s; erasing and retrying",
             esp_err_to_name(err));
    err = nvs_flash_erase();
    if (err == ESP_OK) {
      err = nvs_flash_init();
    }
  }

  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS init failed: %s", esp_err_to_name(err));
    return;
  }

  nvs_handle_t handle;
  err = nvs_open("boot", NVS_READWRITE, &handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS open failed: %s", esp_err_to_name(err));
    return;
  }

  uint32_t boot_count = 0;
  err = nvs_get_u32(handle, "boot_count", &boot_count);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    boot_count = 0;
  } else if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS get boot_count failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }

  boot_count += 1;
  err = nvs_set_u32(handle, "boot_count", boot_count);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS set boot_count failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }

  const uint32_t reset_reason = static_cast<uint32_t>(esp_reset_reason());
  err = nvs_set_u32(handle, "last_reset_reason", reset_reason);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS set last_reset_reason failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }

  err = nvs_commit(handle);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "NVS commit failed: %s", esp_err_to_name(err));
    nvs_close(handle);
    return;
  }
  nvs_close(handle);

  ESP_LOGI(kTag, "NVS state: boot_count=%lu last_reset_reason=%lu", (unsigned long)boot_count,
           (unsigned long)reset_reason);
  dump_nvs_entries();
}

#endif  // LUCE_HAS_NVS
