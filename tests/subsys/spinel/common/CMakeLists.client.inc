# Copyright (c) 2026 Codium Electronique
# SPDX-License-Identifier: Apache-2.0

# Host side of the tests that talk to the Spinel core. Core internals and the
# vendored codec are private to the Spinel subsystem.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/spinel_client.c)
target_include_directories(app PRIVATE
  ${ZEPHYR_OPENER_MODULE_DIR}/subsys/spinel
  ${ZEPHYR_OPENER_MODULE_DIR}/subsys/spinel/lib
)
