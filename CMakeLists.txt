# ─────────────────────────────────────────────────────────────
# HeizCore – Heizungsregler-Firmware für KinCony KC868-A6 / A8
# ESP-IDF >= 5.2   (joltwallet/littlefs über Component Manager)
# ─────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.16)

# joltwallet__littlefs stellt das Makro littlefs_create_partition_image
# bereit. Nach "idf.py reconfigure" liegt es in managed_components/.
# EXTRA_COMPONENT_DIRS wird nur gesetzt wenn der Ordner schon existiert
# (lokaler Build nach erstem reconfigure / CI nach reconfigure-Step).
set(_LFS_COMP_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/joltwallet__littlefs")
if(EXISTS "${_LFS_COMP_DIR}")
    set(EXTRA_COMPONENT_DIRS "${_LFS_COMP_DIR}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(heizcore)

# LittleFS-Image aus dem web/-Ordner erzeugen und mitflashen.
if(EXISTS "${_LFS_COMP_DIR}")
    littlefs_create_partition_image(storage web FLASH_IN_PROJECT)
else()
    message(WARNING
        "joltwallet__littlefs noch nicht aufgelöst. "
        "Bitte zuerst 'idf.py reconfigure' ausführen, dann 'idf.py build'.")
endif()
