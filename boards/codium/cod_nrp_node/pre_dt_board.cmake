# Copyright (c) 2025 Codium Electronique
# All rights reserved.

# Suppress "unique_unit_address_if_enabled" to handle the following overlaps:
# - flash-controller@39000 & kmu@39000
# - power@5000 & clock@5000
list(APPEND EXTRA_DTC_FLAGS "-Wno-unique_unit_address_if_enabled")