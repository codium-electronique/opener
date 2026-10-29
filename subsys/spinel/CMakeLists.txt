# Copyright (c) 2026 Codium Electronique
# SPDX-License-Identifier: Apache-2.0

zephyr_library_named(dectnrp_spinel)

# Vendored OpenThread codec, see lib/README.rst.
zephyr_library_include_directories(lib)
zephyr_library_sources(lib/spinel.c)

zephyr_library_sources_ifdef(CONFIG_DECTNRP_SPINEL_TRANSPORT_UART hdlc.c)
