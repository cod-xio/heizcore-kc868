idf_component_register(
    SRCS "http_server.c" "rest_api.c" "auth.c"
    INCLUDE_DIRS "include" "."
    REQUIRES core netsvc datalog board esp_http_server json app_update
             mbedtls
)
