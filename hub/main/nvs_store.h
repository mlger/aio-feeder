/*
 * AIO Feeder — NVS-backed IHubStore for the ESP-IDF hub (hub_store.h binding).
 * CRC'd blobs live under the "aio-hub" namespace; every put commits.
 */
#ifndef AIO_NVS_STORE_H
#define AIO_NVS_STORE_H

#include <nvs.h>
#include <nvs_flash.h>

#include <cstring>

#include "../../hub/src/hub_store.h"

namespace aio {

class NvsHubStore : public IHubStore {
 public:
  bool begin() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      err = nvs_flash_init();
    }
    if (err != ESP_OK) return false;
    return nvs_open("aio-hub", NVS_READWRITE, &h_) == ESP_OK;
  }

  bool put(const char *key, const uint8_t *data, size_t len) override {
    if (nvs_set_blob(h_, key, data, len) != ESP_OK) return false;
    return nvs_commit(h_) == ESP_OK;
  }

  size_t get(const char *key, uint8_t *out, size_t max_len) override {
    size_t sz = 0;
    if (nvs_get_blob(h_, key, nullptr, &sz) != ESP_OK || !sz || sz > max_len) return 0;
    if (nvs_get_blob(h_, key, out, &sz) != ESP_OK) return 0;
    return sz;
  }

  bool del(const char *key) override {
    if (nvs_erase_key(h_, key) != ESP_OK) return false; /* missing key: not an error for callers */
    return nvs_commit(h_) == ESP_OK;
  }

 private:
  nvs_handle_t h_ = 0;
};

}  // namespace aio

#endif /* AIO_NVS_STORE_H */
