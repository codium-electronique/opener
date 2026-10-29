# Copyright (c) 2026 Codium Electronique
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(spinel_hdlc)

# HDLC-lite is only built for the UART transport: compile it into the test.
target_sources(app PRIVATE src/main.c ${ZEPHYR_OPENER_MODULE_DIR}/subsys/spinel/hdlc.c)
target_include_directories(app PRIVATE ${ZEPHYR_OPENER_MODULE_DIR}/subsys/spinel)
