idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES board core control netsvc websrv datalog nvs_flash joltwallet__littlefs
    PRIV_REQUIRES vfs
)
