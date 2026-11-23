idf_component_register(
    SRCS "impo_cloud_route.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client esp_timer mbedtls)
