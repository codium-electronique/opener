# Copyright (c) 2026 Codium Electronique
# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(dectnrp_spinel)

# Vendored OpenThread codec, see lib/README.rst.
zephyr_library_include_directories(. lib)
zephyr_library_sources(lib/spinel.c)

zephyr_library_sources_ifdef(CONFIG_DECTNRP_SPINEL_TRANSPORT_UART hdlc.c)
zephyr_library_sources_ifdef(CONFIG_DECTNRP_SPINEL_LEVEL_DRIVER l2.c)

zephyr_library_sources(spinel_core.c spinel_ops.c)
zephyr_library_sources_ifdef(CONFIG_DECTNRP_SPINEL_TRANSPORT_UART transport/uart_hdlc.c)
zephyr_library_sources_ifdef(CONFIG_DECTNRP_SPINEL_TRANSPORT_NONE transport/none.c)

# NCP_VERSION reports the Opener version from the generated build_info.h.
if(TARGET opener_build_info_h)
  add_dependencies(dectnrp_spinel opener_build_info_h)
endif()
