idf_component_register(
    SRCS "datalog.c"
    INCLUDE_DIRS "include"
    REQUIRES core netsvc json esp_http_server
)
