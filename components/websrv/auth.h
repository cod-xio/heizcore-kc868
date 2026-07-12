#pragma once
#include <stdbool.h>
#include "esp_err.h"

void      auth_init(void);
esp_err_t auth_login(const char *user, const char *pw, char *out_token, int *out_role);
void      auth_logout(const char *token);
int       auth_check(const char *token);   /* Rolle (user_role_t) oder -1 */
esp_err_t auth_set_password(int user_idx, const char *newpw);
bool      auth_must_change_password(void);
