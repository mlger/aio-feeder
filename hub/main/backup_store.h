/*
 * AIO Feeder — backup store binding (PLAN.md §9.12, §7.7; M4b).
 *
 * Hardware: the Waveshare 4.3B microSD slot on SDMMC 1-bit (CLK/CMD/D0 =
 * GPIO 11/12/13, Kconfig-overridable), FAT via the IDF VFS. Mount is
 * best-effort at boot — per §7.5 feeding and the hub core never depend on
 * the card; the Settings > Storage pane shows the outcome.
 *
 * QEMU shot build: SD is not emulated (§7.7), so the same IBackupStore
 * interface is backed by a heap-allocated RAM store ("virtual SD") — heap,
 * not statics, because internal DRAM is the binding budget there (see
 * tools/qemu/README.md).
 */
#ifndef AIO_BACKUP_STORE_H
#define AIO_BACKUP_STORE_H

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "../../hub/src/backup.h"
#include "sdkconfig.h"

namespace aio {

#if defined(CONFIG_AIO_UI_SHOT) && CONFIG_AIO_UI_SHOT

/* virtual SD — allocated by main.cpp at boot (heap, not static DRAM) */
static inline IBackupStore *backup_store_create() {
  return new RamBackupStore(2, 2800); /* 2 files × 2.8 KB covers the sim scene */
}
static inline const char *backup_store_kind() { return "virtual SD"; }

#else  /* hardware: SDMMC 1-bit + FAT */

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

class SdBackupStore : public IBackupStore {
 public:
  bool mount() override {
    if (mounted_) return true;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT; /* 1-bit: only D0 is wired (§4.3) */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)SD_CLK;
    slot.cmd = (gpio_num_t)SD_CMD;
    slot.d0 = (gpio_num_t)SD_D0;
    slot.width = 1;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {}; /* assign: field order varies */
    cfg.format_if_mount_failed = false;
    cfg.max_files = 4;
    mounted_ = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &cfg, &card_) == ESP_OK;
    return mounted_;
  }

  bool present() const override { return mounted_; }
  const char *kind() const override { return mounted_ ? "SD" : "no card"; }

  bool write(const char *name, const char *data, size_t len) override {
    if (!mounted_) return false;
    char path[80];
    snprintf(path, sizeof(path), MOUNT "/%s", name);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    const size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
  }

  int list(char out[][BACKUP_NAME_LEN], uint8_t max_names) override {
    if (!mounted_) return 0;
    DIR *d = opendir(MOUNT);
    if (!d) return 0;
    char all[BACKUP_SLOTS_MAX][BACKUP_NAME_LEN] = {};
    uint8_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) && n < BACKUP_SLOTS_MAX) {
      if (strncmp(ent->d_name, "aio-backup-", 11) != 0 &&
          strncmp(ent->d_name, "aio-history-", 12) != 0)
        continue;
      strncpy(all[n], ent->d_name, BACKUP_NAME_LEN - 1);
      all[n][BACKUP_NAME_LEN - 1] = '\0';
      n++;
    }
    closedir(d);
    /* newest first (timestamp embedded in the name) */
    bool taken[BACKUP_SLOTS_MAX] = {false};
    int outn = 0;
    for (uint8_t round = 0; round < n && outn < (int)max_names; round++) {
      int best = -1;
      for (uint8_t i = 0; i < n; i++)
        if (!taken[i] && (best < 0 || strcmp(all[i], all[best]) > 0)) best = i;
      if (best < 0) break;
      taken[best] = true;
      strncpy(out[outn], all[best], BACKUP_NAME_LEN);
      out[outn][BACKUP_NAME_LEN - 1] = '\0';
      outn++;
    }
    return outn;
  }

  int read(const char *name, char *out, size_t cap) override {
    if (!mounted_) return -1;
    char path[80];
    snprintf(path, sizeof(path), MOUNT "/%s", name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    const size_t n = fread(out, 1, cap, f);
    fclose(f);
    return (int)n;
  }

 private:
  static constexpr char MOUNT[] = "/sdcard";
#if defined(CONFIG_AIO_SD_CLK) && CONFIG_AIO_SD_CLK
  static constexpr gpio_num_t SD_CLK = (gpio_num_t)CONFIG_AIO_SD_CLK;
  static constexpr gpio_num_t SD_CMD = (gpio_num_t)CONFIG_AIO_SD_CMD;
  static constexpr gpio_num_t SD_D0 = (gpio_num_t)CONFIG_AIO_SD_D0;
#else
  static constexpr gpio_num_t SD_CLK = GPIO_NUM_11;
  static constexpr gpio_num_t SD_CMD = GPIO_NUM_12;
  static constexpr gpio_num_t SD_D0 = GPIO_NUM_13;
#endif
  bool mounted_ = false;
  sdmmc_card_t *card_ = nullptr;
};

static inline IBackupStore *backup_store_create() { return new SdBackupStore(); }
static inline const char *backup_store_kind() { return "SD"; }

#endif /* CONFIG_AIO_UI_SHOT */

}  // namespace aio

#endif /* AIO_BACKUP_STORE_H */
