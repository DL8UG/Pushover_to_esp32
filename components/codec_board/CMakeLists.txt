idf_component_register(SRCS "cfg_parse.c" "codec_board.c" "codec_init.c" "dummy_codec.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_codec_dev esp_driver_i2c fatfs sdmmc
                       EMBED_TXTFILES "board_cfg.txt")
