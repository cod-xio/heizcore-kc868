#pragma once
#include "esp_http_server.h"

void  datalog_start(void);
char *datalog_query_json(const char *range);   /* Aufrufer gibt frei */
void  datalog_export_csv(httpd_req_t *req);    /* streamt chunked    */
