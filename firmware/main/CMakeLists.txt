idf_component_register(
    SRCS "main.c" "proto.c" "transport.c" "audio.c" "ui.c" "orient.c" "dpower.c" "keydebounce.c" "motion.c" "imu.c" "font/clawd_font_48.c" "rotblit.c" "volknob.c" "volarc.c" "arcvis.c" "volstate.c" "volume.c" "nvstore.c" "brtmap.c" "bright.c" "apack.c" "aplay.c" "skin.c" "aglue.c" "acomp.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_usb_serial_jtag esp_driver_i2c esp_driver_gpio esp_timer esp_system nvs_flash esp_partition spi_flash
    PRIV_REQUIRES esp_hw_support
)
