#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

void websrv_start(void);
void rest_api_register(httpd_handle_t server);

/* Rolle des angemeldeten Benutzers (user_role_t) oder -1 */
int       websrv_get_role(httpd_req_t *req);
/* sendet selbst 401/403 und liefert ESP_FAIL, wenn Rolle > max_role */
esp_err_t websrv_require_role(httpd_req_t *req, int max_role);
char     *websrv_build_state_json(void);   /* Aufrufer gibt frei */
