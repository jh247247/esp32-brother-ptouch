# ESP-IDF component registration (this repo IS the component — consume it via
# the IDF component manager as a git dependency, EXTRA_COMPONENT_DIRS, or a
# components/ subdir; PlatformIO/Arduino users are served by library.json).
idf_component_register(
    SRCS
        "src/ptouch_raster.c"
        "src/ptouch_usb.c"
        "src/ptouch_print.c"
    INCLUDE_DIRS "include"
    REQUIRES usb esp_timer
)
