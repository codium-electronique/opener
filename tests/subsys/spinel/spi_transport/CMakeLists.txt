# Copyright (c) 2026 Codium Electronique
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(spinel_spi_transport)

target_sources(app PRIVATE src/main.c src/fake_spis.c)
include(${CMAKE_CURRENT_LIST_DIR}/../common/CMakeLists.inc)
# Core internals (spinel_reply_status()) are private to the Spinel subsystem.
target_include_directories(app PRIVATE
  ${ZEPHYR_OPENER_MODULE_DIR}/subsys/spinel
  ${ZEPHYR_OPENER_MODULE_DIR}/subsys/spinel/lib
)
