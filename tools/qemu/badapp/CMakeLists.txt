# tools/qemu/badapp — deliberately non-confirming ESP32-S3 image for the
# M8a OTA selftest (ci.yml ota-selftest): the bootloader boots it in
# PENDING_VERIFY, it reboots immediately without calling
# esp_ota_mark_app_valid_cancel_rollback(), and the next boot attempt gets
# marked INVALID → fallback to the previous slot. That walk exercises the
# rollback path of CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE for real.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(badapp)
