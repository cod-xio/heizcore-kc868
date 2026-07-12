#pragma once
#include "esp_err.h"
#include "datamodel.h"

esp_err_t    cfg_load(void);
esp_err_t    cfg_save(void);
const dm_t  *cfg_get(void);            /* konsistente Kopie              */
char        *cfg_export_json(void);    /* Aufrufer gibt mit free() frei  */
esp_err_t    cfg_import_json(const char *json);
