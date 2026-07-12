idf_component_register(
    SRCS "control_task.c" "boiler.c" "buffer.c" "heating_circuit.c"
         "dhw.c" "solar.c" "heat_curve.c" "pid.c"
    INCLUDE_DIRS "include"
    REQUIRES core board esp_timer
)
