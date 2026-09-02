# Copyright (c) 2026 Codium Electronique
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_TFM_FLASH_MERGED_BINARY)
  set_property(TARGET runners_yaml_props_target PROPERTY hex_file tfm_merged.hex)
endif()

set(JLINKSCRIPTFILE ${CMAKE_CURRENT_LIST_DIR}/support/nrf9151_connect_under_reset.JLinkScript)
board_runner_args(jlink "--device=nRF9151_xxCA" "--speed=4000" "--tool-opt=-jlinkscriptfile ${JLINKSCRIPTFILE}")

include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)