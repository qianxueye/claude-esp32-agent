idf_component_register(
    SRCS "main.c" "proto.c" "transport.c" "audio.c" "ui.c" "orient.c" "dpower.c" "motion.c" "imu.c" "font/clawd_font_48.c" "rotblit.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_usb_serial_jtag esp_driver_i2c esp_driver_gpio esp_timer esp_system
    PRIV_REQUIRES esp_hw_support
)
