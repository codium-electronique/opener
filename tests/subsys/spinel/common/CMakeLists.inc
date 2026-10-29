# Copyright (c) 2026 Codium Electronique
# SPDX-License-Identifier: Apache-2.0

# Mock DECT NR+ driver, included by the Spinel tests that need one.
target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/mock_driver.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR})
