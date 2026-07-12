/* auth.c – Benutzer, Sitzungen, Rollen
 *
 * Passwörter: SHA-256(salt || passwort), Salt 8 Byte zufällig.
 * Sitzungen: 16-Byte-Token (Cookie "hcsid"), Timeout 24 h gleitend.
 * Erster Start: admin erhält Passwort "admin" und muss es beim ersten
 * Login über die Weboberfläche ändern (Flag must_change).
 */
#include <string.h>
#include <time.h>
#include "auth.h"
#include "datamodel.h"
#include "config_store.h"
#include "esp_random.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

static const char *TAG = "auth";
#define SESSION_MAX      8
#define SESSION_TTL_S    (24 * 3600)

typedef struct {
    char     token[33];
    int      user_idx;
    time_t   last_seen;
    bool     used;
} session_t;

static session_t s_sess[SESSION_MAX];
static bool s_must_change_admin;

static void hex(const uint8_t *in, int n, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2*i] = h[in[i] >> 4]; out[2*i+1] = h[in[i] & 15]; }
    out[2*n] = 0;
}

static void hash_pw(const char *salt, const char *pw, char out[65])
{
    uint8_t dig[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, (const uint8_t *)salt, strlen(salt));
    mbedtls_sha256_update(&c, (const uint8_t *)pw, strlen(pw));
    mbedtls_sha256_finish(&c, dig);
    mbedtls_sha256_free(&c);
    hex(dig, 32, out);
}

void auth_init(void)
{
    dm_t *d = dm_lock();
    if (!d->user[0].pw_hash[0]) {
        uint8_t s[8]; esp_fill_random(s, 8);
        hex(s, 8, d->user[0].salt);
        hash_pw(d->user[0].salt, "admin", d->user[0].pw_hash);
        s_must_change_admin = true;
        ESP_LOGW(TAG, "Erststart: admin/admin – Passwortänderung wird erzwungen");
        dm_unlock();
        cfg_save();
        return;
    }
    dm_unlock();
}

bool auth_must_change_password(void) { return s_must_change_admin; }

esp_err_t auth_set_password(int user_idx, const char *newpw)
{
    if (strlen(newpw) < 4) return ESP_ERR_INVALID_ARG;
    dm_t *d = dm_lock();
    if (user_idx < 0 || user_idx >= DM_MAX_USERS || !d->user[user_idx].enabled) {
        dm_unlock(); return ESP_ERR_INVALID_ARG;
    }
    uint8_t s[8]; esp_fill_random(s, 8);
    hex(s, 8, d->user[user_idx].salt);
    hash_pw(d->user[user_idx].salt, newpw, d->user[user_idx].pw_hash);
    dm_unlock();
    if (user_idx == 0) s_must_change_admin = false;
    return cfg_save();
}

/* Login: bei Erfolg Token in out_token (33 Byte) */
esp_err_t auth_login(const char *user, const char *pw, char *out_token, int *out_role)
{
    dm_t *d = dm_lock();
    int found = -1;
    for (int i = 0; i < DM_MAX_USERS; i++) {
        if (d->user[i].enabled && strcmp(d->user[i].name, user) == 0) { found = i; break; }
    }
    if (found < 0) { dm_unlock(); return ESP_ERR_NOT_FOUND; }
    char h[65];
    hash_pw(d->user[found].salt, pw, h);
    bool ok = (strcmp(h, d->user[found].pw_hash) == 0);
    int role = d->user[found].role;
    dm_unlock();
    if (!ok) return ESP_ERR_INVALID_STATE;

    /* Session anlegen (älteste verdrängen) */
    int slot = -1; time_t oldest = 0;
    time_t now = time(NULL);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (!s_sess[i].used || now - s_sess[i].last_seen > SESSION_TTL_S) { slot = i; break; }
        if (slot < 0 || s_sess[i].last_seen < oldest) { oldest = s_sess[i].last_seen; slot = i; }
    }
    uint8_t t[16]; esp_fill_random(t, 16);
    hex(t, 16, s_sess[slot].token);
    s_sess[slot].user_idx = found;
    s_sess[slot].last_seen = now;
    s_sess[slot].used = true;
    strcpy(out_token, s_sess[slot].token);
    *out_role = role;
    ESP_LOGI(TAG, "Anmeldung: %s (Rolle %d)", user, role);
    return ESP_OK;
}

void auth_logout(const char *token)
{
    for (int i = 0; i < SESSION_MAX; i++)
        if (s_sess[i].used && strcmp(s_sess[i].token, token) == 0)
            s_sess[i].used = false;
}

/* liefert Rolle oder -1 */
int auth_check(const char *token)
{
    if (!token || !token[0]) return -1;
    time_t now = time(NULL);
    for (int i = 0; i < SESSION_MAX; i++) {
        if (s_sess[i].used && strcmp(s_sess[i].token, token) == 0) {
            if (now - s_sess[i].last_seen > SESSION_TTL_S) { s_sess[i].used = false; return -1; }
            s_sess[i].last_seen = now;
            int role;
            dm_t *d = dm_lock();
            role = d->user[s_sess[i].user_idx].role;
            dm_unlock();
            return role;
        }
    }
    return -1;
}
