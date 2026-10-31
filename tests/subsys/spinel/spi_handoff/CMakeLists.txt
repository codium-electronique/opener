# Copyright (c) 2026 Codium Electronique
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(spinel_spi_handoff)

# The SPI link is only built for the SPI transport: compile them into the test.
target_sources(app PRIVATE src/main.c ${ZEPHYR_OPENER_MODULE_DIR}/subsys/spinel/spi_link.c ${ZEPHYR_OPENER_MODULE_DIR}/subsys/spinel/spi_handoff.c)
target_include_directories(app PRIVATE ${ZEPHYR_OPENER_MODULE_DIR}/subsys/spinel)
# Let the stress tests run other threads inside the copy window of spinel_spi_handoff_arm().
target_compile_definitions(app PRIVATE SPINEL_SPI_HANDOFF_TEST_HOOK)
include(${CMAKE_CURRENT_LIST_DIR}/../common/CMakeLists.inc)
