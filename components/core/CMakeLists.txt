idf_component_register(
    SRCS "datamodel.c" "config_store.c" "alarms.c"
    INCLUDE_DIRS "include"
    REQUIRES json
)
