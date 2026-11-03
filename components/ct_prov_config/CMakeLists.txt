idf_component_register(SRCS "src/ct_prov_caps.c" "src/ct_prov_set.c"
                           "src/ct_prov_scan.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ct_prov json)
