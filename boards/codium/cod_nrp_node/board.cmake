# Copyright (c) 2025 Codium Electronique
# All rights reserved.

if(CONFIG_TFM_FLASH_MERGED_BINARY)
  set_property(TARGET runners_yaml_props_target PROPERTY hex_file tfm_merged.hex)
endif()

if (CONFIG_SOC_NRF9151_LACA)
  set(JLINKSCRIPTFILE ${CMAKE_CURRENT_LIST_DIR}/support/nrf9151_connect_under_reset.JLinkScript)
  board_runner_args(jlink "--device=nRF9151_xxCA" "--speed=4000"
    "--tool-opt=-jlinkscriptfile ${JLINKSCRIPTFILE}")
elseif(CONFIG_SOC_NRF54L15_CPUAPP)
  set(JLINKSCRIPTFILE ${CMAKE_CURRENT_LIST_DIR}/support/nrf54l15_connect_under_reset.JLinkScript)
  board_runner_args(jlink "--device=nRF54L15_M33" "--speed=4000"
    "--tool-opt=-jlinkscriptfile ${JLINKSCRIPTFILE}")
endif()

include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
if (CONFIG_SOC_NRF9151_LACA)
  include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
endif()
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
