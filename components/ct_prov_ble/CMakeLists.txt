idf_component_register(SRCS "src/ct_prov_gatt.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ct_prov
                       PRIV_REQUIRES bt esp_timer)
