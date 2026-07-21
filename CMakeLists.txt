# ESP-IDF component registration (this repo IS the component — consume it via
# the IDF component manager as a git dependency, EXTRA_COMPONENT_DIRS, or a
# components/ subdir. The public beta supports ESP-IDF 6 on ESP32-S3.
idf_component_register(
    SRCS
        "src/ptouch_model.c"
        "src/ptouch_usb_descriptor.c"
        "src/ptouch_raster.c"
        "src/ptouch_completion.c"
        "src/ptouch_usb.c"
        "src/ptouch_print.c"
    INCLUDE_DIRS "include"
    REQUIRES usb esp_timer
)
