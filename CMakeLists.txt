zephyr_library()

if (CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  zephyr_library_sources(src/input_relay_central.c)
endif()

if (CONFIG_ZMK_SPLIT AND (NOT CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
  zephyr_library_sources(src/input_relay_peripheral.c)
endif()

zephyr_library_sources_ifdef(CONFIG_ZMK_VIRTUAL_INPUT src/virtual_input.c)
zephyr_include_directories(include)

zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
