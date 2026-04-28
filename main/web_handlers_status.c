// ─────────────────────────────────────────────────────────────────────────────
//  web_handlers_status.c — System status JSON + WiFi setup handlers
//  Routes: GET /status.json, GET /wifi, POST /wifi/save
// ─────────────────────────────────────────────────────────────────────────────

#include "web_handlers_status.h"
#include "i2c_manager.h"
#include "pcf8574.h"
#include "ds3231.h"
#include "ft6336u.h"
#include "wifi_sta.h"
#include "netpie_mqtt.h"
#include "offline_sync.h"
#include "cloud_secrets.h"
#include "telegram_bot.h"
#include "dfplayer.h"
#include "pill_sensor_status.h"
#include "vl53l0x_multi.h"
#include "config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_idf_version.h"
#include "dispenser_scheduler.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdlib.h>

static const char *TAG = "web_status";
static char s_cloud_session_token[33] = {0};
static int64_t s_cloud_session_expiry_us = 0;
static char s_maint_session_token[33] = {0};
static int64_t s_maint_session_expiry_us = 0;

#define CLOUD_SESSION_LIFETIME_US (30LL * 60LL * 1000000LL)
#define MAINT_SESSION_LIFETIME_US (5LL * 60LL * 1000000LL)

static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = (char)tolower((unsigned char)ch);
    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
    return -1;
}

static void url_decode_inplace(char *text)
{
    if (!text) return;
    char *src = text;
    char *dst = text;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = hex_val(src[1]);
            int lo = hex_val(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }
        if (*src == '+') {
            *dst++ = ' ';
            ++src;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static bool extract_form_value(const char *body, const char *key, char *out, size_t out_len)
{
    if (!body || !key || !out || out_len == 0) return false;

    char pattern[96];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *start = strstr(body, pattern);
    if (!start) return false;
    start += strlen(pattern);

    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    url_decode_inplace(out);
    return true;
}

static void html_escape(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t used = 0;
    while (*src && used + 1 < dst_len) {
        const char *rep = NULL;
        switch (*src) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }

        if (rep) {
            size_t rep_len = strlen(rep);
            if (used + rep_len >= dst_len) break;
            memcpy(dst + used, rep, rep_len);
            used += rep_len;
        } else {
            dst[used++] = *src;
        }
        ++src;
    }
    dst[used] = '\0';
}

static void cloud_build_session_token(char *dst, size_t dst_len)
{
    if (!dst || dst_len < 33) return;
    for (int i = 0; i < 4; ++i) {
        uint32_t r = esp_random();
        snprintf(dst + (i * 8), dst_len - (size_t)(i * 8), "%08" PRIx32, r);
    }
    dst[32] = '\0';
}

static void cloud_auth_start_session(void)
{
    cloud_build_session_token(s_cloud_session_token, sizeof(s_cloud_session_token));
    s_cloud_session_expiry_us = esp_timer_get_time() + CLOUD_SESSION_LIFETIME_US;
    s_maint_session_token[0] = '\0';
    s_maint_session_expiry_us = 0;
}

static void cloud_auth_clear_session(void)
{
    s_cloud_session_token[0] = '\0';
    s_cloud_session_expiry_us = 0;
    s_maint_session_token[0] = '\0';
    s_maint_session_expiry_us = 0;
}

static void maintenance_auth_start_session(void)
{
    cloud_build_session_token(s_maint_session_token, sizeof(s_maint_session_token));
    s_maint_session_expiry_us = esp_timer_get_time() + MAINT_SESSION_LIFETIME_US;
}

static void maintenance_auth_clear_session(void)
{
    s_maint_session_token[0] = '\0';
    s_maint_session_expiry_us = 0;
}

static bool auth_cookie_valid(httpd_req_t *req, const char *cookie_key, char *token, int64_t *expiry_us)
{
    if (!req || !cookie_key || !token || !expiry_us || !token[0]) return false;
    if (esp_timer_get_time() >= *expiry_us) {
        token[0] = '\0';
        *expiry_us = 0;
        return false;
    }

    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len == 0 || cookie_len >= 256) return false;

    char cookie[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }

    char key[48];
    snprintf(key, sizeof(key), "%s=", cookie_key);
    char *pos = strstr(cookie, key);
    if (!pos) return false;
    pos += strlen(key);
    size_t val_len = strcspn(pos, ";");
    if (val_len != strlen(token)) return false;
    return strncmp(pos, token, val_len) == 0;
}

static bool cloud_auth_cookie_valid(httpd_req_t *req)
{
    return auth_cookie_valid(req, "cloud_auth", s_cloud_session_token, &s_cloud_session_expiry_us);
}

bool web_tech_cookie_valid(httpd_req_t *req)
{
    return auth_cookie_valid(req, "maint_auth", s_maint_session_token, &s_maint_session_expiry_us);
}

static esp_err_t maintenance_send_login_page(httpd_req_t *req, bool show_error)
{
    char *html = (char *)malloc(6144);
    if (!html) return ESP_ERR_NO_MEM;

    snprintf(html, 6144,
        "<!doctype html>"
        "<html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Technician Unlock</title>"
        "<style>"
        "body.lang-en .lang-th{display:none!important}body.lang-th .lang-en{display:none!important}"
        "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px;background:radial-gradient(circle at top,#16345a 0%%,#0a1425 42%%,#050b14 100%%);font-family:'Segoe UI',Tahoma,sans-serif;color:#f4f8ff}"
        ".card{max-width:620px;width:100%%;background:linear-gradient(180deg,rgba(17,36,61,.97),rgba(8,18,32,.98));border:1px solid rgba(255,255,255,.08);border-radius:28px;padding:30px;box-shadow:0 20px 46px rgba(0,0,0,.24)}"
        ".top{display:flex;justify-content:space-between;align-items:flex-start;gap:12px;flex-wrap:wrap}"
        ".pill{display:inline-flex;padding:7px 12px;border-radius:999px;background:rgba(255,255,255,.05);color:#ffd98b;font-size:13px;font-weight:700;letter-spacing:.04em;text-transform:uppercase}"
        ".lang-switch{display:inline-flex;padding:5px;border-radius:999px;background:rgba(3,12,22,.55);border:1px solid rgba(255,255,255,.09);gap:6px}"
        ".lang-switch button{border:none;background:transparent;color:#d7e5f7;min-width:74px;height:38px;border-radius:999px;font-size:14px;font-weight:700;cursor:pointer}"
        ".lang-switch button.active{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032}"
        "h1{margin:18px 0 10px;font-size:34px;line-height:1.15}"
        "p{margin:0;color:#a7bdd7;font-size:16px;line-height:1.8}"
        "label{display:block;margin:18px 0 8px;font-size:15px;font-weight:700}"
        "input{width:100%%;border-radius:16px;border:1px solid #35567f;background:#06101e;color:#f4f8ff;padding:14px 16px;font-size:16px;outline:none}"
        "input:focus{border-color:#6db8ff;box-shadow:0 0 0 3px rgba(109,184,255,.16)}"
        ".warn{margin-top:14px;padding:12px 14px;border-radius:14px;background:rgba(255,209,102,.10);border:1px solid rgba(255,209,102,.18);color:#ffe7a3;font-size:14px;line-height:1.7}"
        ".error{margin-top:14px;padding:12px 14px;border-radius:14px;background:rgba(255,138,128,.10);border:1px solid rgba(255,138,128,.18);color:#ffd9d3;font-size:14px;line-height:1.7}"
        ".actions{display:flex;flex-wrap:wrap;gap:12px;margin-top:22px}"
        ".btn{display:inline-flex;align-items:center;justify-content:center;min-height:48px;padding:0 18px;border-radius:14px;border:none;font-size:15px;font-weight:700;text-decoration:none;cursor:pointer}"
        ".primary{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032}"
        ".secondary{background:rgba(109,184,255,.12);border:1px solid rgba(109,184,255,.28);color:#e4efff}"
        ".hint{margin-top:12px;color:#96acc8;font-size:13px;line-height:1.7}"
        "@media (max-width:720px){.top,.actions{flex-direction:column}.btn{width:100%%}}"
        "</style></head><body class='lang-en'>"
        "<div class='card'>"
        "<div class='top'>"
        "<div class='pill'><span class='lang-en'>Technician Only</span><span class='lang-th'>สำหรับช่างเท่านั้น</span></div>"
        "<div class='lang-switch'>"
        "<button type='button' id='maint-en' class='active' onclick=\"setMaintLang('en')\">ENG</button>"
        "<button type='button' id='maint-th' onclick=\"setMaintLang('th')\">ไทย</button>"
        "</div></div>"
        "<h1><span class='lang-en'>Unlock Maintenance Tools</span><span class='lang-th'>ปลดล็อกเครื่องมือสำหรับช่าง</span></h1>"
        "<p class='lang-en'>Servo tuning, camera diagnostics, and live maintenance controls are protected behind an extra unlock step.</p>"
        "<p class='lang-th'>การปรับองศา servo, ดูกล้อง, และเครื่องมือ maintenance ถูกแยกไว้หลังการยืนยันสิทธิ์อีกชั้นหนึ่ง</p>"
        "<div class='warn'><span class='lang-en'>Only technicians should continue. Everyday users should stay on the Cloud and Wi-Fi setup pages.</span><span class='lang-th'>หน้านี้เหมาะสำหรับช่างเท่านั้น ผู้ใช้งานทั่วไปควรอยู่ที่หน้าตั้งค่า Cloud และ Wi-Fi</span></div>"
        "<form method='POST' action='/maint/unlock'>"
        "<label for='cloud_code'><span class='lang-en'>Technician Access Code</span><span class='lang-th'>รหัสเข้าหน้าช่าง</span></label>"
        "<input id='cloud_code' name='cloud_code' type='password' autocomplete='current-password' autofocus>"
        "%s"
        "<div class='actions'>"
        "<button class='btn primary' type='submit'><span class='lang-en'>Open Maintenance Dashboard</span><span class='lang-th'>เข้า Maintenance Dashboard</span></button>"
        "<a class='btn secondary' href='/cloud'><span class='lang-en'>Back to Cloud Config</span><span class='lang-th'>กลับหน้าตั้งค่า Cloud</span></a>"
        "</div>"
        "<div class='hint lang-en'>The maintenance unlock stays active for 5 minutes, then the dashboard and servo controls will lock again.</div>"
        "<div class='hint lang-th'>การปลดล็อกหน้าช่างจะอยู่ได้ 5 นาที จากนั้น dashboard และการควบคุม servo จะล็อกใหม่อัตโนมัติ</div>"
        "</form>"
        "</div>"
        "<script>"
        "function setMaintLang(lang){document.body.classList.remove('lang-en','lang-th');document.body.classList.add('lang-'+lang);document.documentElement.lang=lang;localStorage.setItem('cloudLang',lang);document.getElementById('maint-en').classList.toggle('active',lang==='en');document.getElementById('maint-th').classList.toggle('active',lang==='th');}"
        "(function(){var lang=localStorage.getItem('cloudLang');if(!lang){lang=(navigator.language||'en').toLowerCase().indexOf('th')===0?'th':'en';}setMaintLang(lang);})();"
        "</script>"
        "</body></html>",
        show_error
            ? "<div class='error'><span class='lang-en'>Access code is incorrect. Please try again.</span><span class='lang-th'>รหัสไม่ถูกต้อง กรุณาลองใหม่อีกครั้ง</span></div>"
            : "");

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return ret;
}

bool web_auth_cookie_valid(httpd_req_t *req)
{
    return cloud_auth_cookie_valid(req);
}

static esp_err_t entry_send_login_page(httpd_req_t *req, bool show_error)
{
    char *html = (char *)malloc(7168);
    if (!html) return ESP_ERR_NO_MEM;

    snprintf(html, 7168,
        "<!doctype html>"
        "<html lang='th'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Automatic Pill Dispenser Login</title>"
        "<style>"
        "body.lang-en .lang-th{display:none!important}body.lang-th .lang-en{display:none!important}"
        "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px;background:radial-gradient(circle at top,#16345a 0%%,#0a1425 42%%,#050b14 100%%);font-family:'Segoe UI',Tahoma,sans-serif;color:#f4f8ff}"
        ".card{max-width:720px;width:100%%;background:linear-gradient(180deg,rgba(17,36,61,.97),rgba(8,18,32,.98));border:1px solid rgba(255,255,255,.08);border-radius:28px;padding:30px;box-shadow:0 20px 46px rgba(0,0,0,.24)}"
        ".top{display:flex;justify-content:space-between;align-items:flex-start;gap:12px;flex-wrap:wrap}"
        ".pill{display:inline-flex;padding:7px 12px;border-radius:999px;background:rgba(255,255,255,.05);color:#9ae8d0;font-size:13px;font-weight:700;letter-spacing:.04em;text-transform:uppercase}"
        ".lang-switch{display:inline-flex;padding:5px;border-radius:999px;background:rgba(3,12,22,.55);border:1px solid rgba(255,255,255,.09);gap:6px}"
        ".lang-switch button{border:none;background:transparent;color:#d7e5f7;min-width:74px;height:38px;border-radius:999px;font-size:14px;font-weight:700;cursor:pointer}"
        ".lang-switch button.active{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032}"
        "h1{margin:18px 0 10px;font-size:34px;line-height:1.15}"
        "p{margin:0;color:#a7bdd7;font-size:16px;line-height:1.8}"
        "label{display:block;margin:18px 0 8px;font-size:15px;font-weight:700}"
        "input{width:100%%;border-radius:16px;border:1px solid #35567f;background:#06101e;color:#f4f8ff;padding:14px 16px;font-size:16px;outline:none}"
        "input:focus{border-color:#6db8ff;box-shadow:0 0 0 3px rgba(109,184,255,.16)}"
        ".error{margin-top:14px;padding:12px 14px;border-radius:14px;background:rgba(255,138,128,.10);border:1px solid rgba(255,138,128,.18);color:#ffd9d3;font-size:14px;line-height:1.7}"
        ".guide{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;margin-top:18px}"
        ".guide-card{border-radius:18px;padding:16px;background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.08)}"
        ".guide-card h3{margin:0 0 6px;font-size:18px}.guide-card p{font-size:14px;line-height:1.7}"
        ".actions{display:flex;flex-wrap:wrap;gap:12px;margin-top:22px}"
        ".btn{display:inline-flex;align-items:center;justify-content:center;min-height:48px;padding:0 18px;border-radius:14px;border:none;font-size:15px;font-weight:700;text-decoration:none;cursor:pointer}"
        ".primary{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032}"
        ".secondary{background:rgba(109,184,255,.12);border:1px solid rgba(109,184,255,.28);color:#e4efff}"
        ".hint{margin-top:12px;color:#96acc8;font-size:13px;line-height:1.7}"
        "@media (max-width:720px){.top,.actions{flex-direction:column}.btn{width:100%%}}"
        "</style></head><body class='lang-th'>"
        "<div class='card'>"
        "<div class='top'>"
        "<div class='pill'><span class='lang-en'>Access Control</span><span class='lang-th'>เข้าสู่ระบบอุปกรณ์</span></div>"
        "<div class='lang-switch'>"
        "<button type='button' id='login-en' onclick=\"setEntryLang('en')\">ENG</button>"
        "<button type='button' id='login-th' class='active' onclick=\"setEntryLang('th')\">ไทย</button>"
        "</div></div>"
        "<h1><span class='lang-en'>Enter one access code</span><span class='lang-th'>กรอกรหัสเพียงครั้งเดียว</span></h1>"
        "<p class='lang-en'>The device will open the correct page automatically. Customer code goes to the user settings page, while technician, admin, or owner codes go straight to the maintenance dashboard.</p>"
        "<p class='lang-th'>ระบบจะพาไปยังหน้าที่ถูกต้องให้อัตโนมัติ ถ้าเป็นรหัสผู้ใช้จะเข้าไปหน้าตั้งค่าสำหรับลูกค้า แต่ถ้าเป็นรหัสช่าง แอดมิน หรือรหัสเจ้าของ จะเข้าหน้าช่างทันที</p>"
        "<form method='POST' action='/cloud/login'>"
        "<label for='access_code'><span class='lang-en'>Access Code</span><span class='lang-th'>รหัสเข้าใช้งาน</span></label>"
        "<input id='access_code' name='access_code' type='password' autocomplete='current-password' autofocus>"
        "%s"
        "<div class='actions'>"
        "<button class='btn primary' type='submit'><span class='lang-en'>Continue</span><span class='lang-th'>เข้าสู่ระบบ</span></button>"
        "<a class='btn secondary' href='/wifi'><span class='lang-en'>Open Wi-Fi Setup</span><span class='lang-th'>ตั้งค่า Wi-Fi</span></a>"
        "</div>"
        "</form>"
        "<div class='guide'>"
        "<div class='guide-card'><h3><span class='lang-en'>Customer Code</span><span class='lang-th'>รหัสผู้ใช้</span></h3><p><span class='lang-en'>Opens the protected customer page for Telegram, Google Sheets, and customer-facing settings.</span><span class='lang-th'>เข้าไปหน้าที่ลูกค้าใช้สำหรับตั้งค่า Telegram, Google Sheets และข้อมูลใช้งานทั่วไป</span></p></div>"
        "<div class='guide-card'><h3><span class='lang-en'>Technician Code</span><span class='lang-th'>รหัสช่าง</span></h3><p><span class='lang-en'>Opens the maintenance dashboard for diagnostics, module checks, camera tools, and backend controls.</span><span class='lang-th'>เข้าไปหน้าช่างสำหรับตรวจระบบ โมดูล กล้อง และฟังก์ชันหลังบ้าน</span></p></div>"
        "</div>"
        "<div class='hint lang-en'>Opening the device IP will always land on this page first unless you already have a valid login session.</div>"
        "<div class='hint lang-th'>เมื่อเปิด IP ของเครื่อง ระบบจะเข้าหน้านี้ก่อนเสมอ ยกเว้นมี session ที่ล็อกอินค้างไว้อยู่แล้ว</div>"
        "</div>"
        "<script>"
        "function setEntryLang(lang){document.body.classList.remove('lang-en','lang-th');document.body.classList.add('lang-'+lang);document.documentElement.lang=lang;localStorage.setItem('cloudLang',lang);document.getElementById('login-en').classList.toggle('active',lang==='en');document.getElementById('login-th').classList.toggle('active',lang==='th');}"
        "(function(){var lang=localStorage.getItem('cloudLang');if(!lang){lang=(navigator.language||'th').toLowerCase().indexOf('th')===0?'th':'en';}setEntryLang(lang);})();"
        "</script>"
        "</body></html>",
        show_error
            ? "<div class='error'><span class='lang-en'>The access code is not correct. Please try again.</span><span class='lang-th'>รหัสไม่ถูกต้อง กรุณาลองใหม่อีกครั้ง</span></div>"
            : "");

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return ret;
}

esp_err_t web_entry_handler(httpd_req_t *req)
{
    // If user already has a valid session, keep them signed in and redirect
    // to the appropriate dashboard instead of logging them out.
    if (web_tech_cookie_valid(req)) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Location", "/tech");
        return httpd_resp_send(req, NULL, 0);
    }
    if (cloud_auth_cookie_valid(req)) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Location", "/cloud");
        return httpd_resp_send(req, NULL, 0);
    }
    maintenance_auth_clear_session();
    cloud_auth_clear_session();
    httpd_resp_set_hdr(req, "Set-Cookie", "cloud_auth=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
    httpd_resp_set_hdr(req, "Set-Cookie", "maint_auth=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
    return entry_send_login_page(req, false);
}

esp_err_t web_redirect_to_cloud_login(httpd_req_t *req, bool clear_session)
{
    if (clear_session) {
        cloud_auth_clear_session();
    }

    char host[96] = {0};
    char redirect[160] = {0};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK && host[0] != '\0') {
        char *colon = strchr(host, ':');
        if (colon) *colon = '\0';
    } else {
        const char *ip = wifi_sta_get_ip();
        snprintf(host, sizeof(host), "%s", (ip && ip[0]) ? ip : "192.168.4.1");
    }

    snprintf(redirect, sizeof(redirect), "http://%s/", host);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (clear_session) {
        httpd_resp_set_hdr(req, "Set-Cookie", "cloud_auth=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
        httpd_resp_set_hdr(req, "Set-Cookie", "maint_auth=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
    }
    httpd_resp_set_hdr(req, "Location", redirect);
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_require_maintenance_auth(httpd_req_t *req)
{
    if (cloud_auth_cookie_valid(req)) {
        return ESP_OK;
    }
    return web_redirect_to_cloud_login(req, false);
}

esp_err_t web_require_tech_page_auth(httpd_req_t *req)
{
    if (web_tech_cookie_valid(req)) {
        return ESP_OK;
    }
    return web_redirect_to_cloud_login(req, false);
}

esp_err_t web_require_tech_api_auth(httpd_req_t *req)
{
    if (web_tech_cookie_valid(req)) {
        return ESP_OK;
    }
    if (!cloud_auth_cookie_valid(req)) {
        return web_redirect_to_cloud_login(req, false);
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"technician_unlock_required\"}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t cloud_send_login_page(httpd_req_t *req, bool show_error)
{
    char *html = (char *)malloc(6144);
    if (!html) return ESP_ERR_NO_MEM;

    snprintf(html, 6144,
        "<!doctype html>"
        "<html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Cloud Access</title>"
        "<style>"
        "body.lang-en .lang-th{display:none!important}body.lang-th .lang-en{display:none!important}"
        "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px;background:radial-gradient(circle at top,#16345a 0%%,#0a1425 42%%,#050b14 100%%);font-family:'Segoe UI',Tahoma,sans-serif;color:#f4f8ff}"
        ".card{max-width:620px;width:100%%;background:linear-gradient(180deg,rgba(17,36,61,.97),rgba(8,18,32,.98));border:1px solid rgba(255,255,255,.08);border-radius:28px;padding:30px;box-shadow:0 20px 46px rgba(0,0,0,.24)}"
        ".top{display:flex;justify-content:space-between;align-items:flex-start;gap:12px;flex-wrap:wrap}"
        ".pill{display:inline-flex;padding:7px 12px;border-radius:999px;background:rgba(255,255,255,.05);color:#9ae8d0;font-size:13px;font-weight:700;letter-spacing:.04em;text-transform:uppercase}"
        ".lang-switch{display:inline-flex;padding:5px;border-radius:999px;background:rgba(3,12,22,.55);border:1px solid rgba(255,255,255,.09);gap:6px}"
        ".lang-switch button{border:none;background:transparent;color:#d7e5f7;min-width:74px;height:38px;border-radius:999px;font-size:14px;font-weight:700;cursor:pointer}"
        ".lang-switch button.active{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032}"
        "h1{margin:18px 0 10px;font-size:34px;line-height:1.15}"
        "p{margin:0;color:#a7bdd7;font-size:16px;line-height:1.8}"
        "label{display:block;margin:18px 0 8px;font-size:15px;font-weight:700}"
        "input{width:100%%;border-radius:16px;border:1px solid #35567f;background:#06101e;color:#f4f8ff;padding:14px 16px;font-size:16px;outline:none}"
        "input:focus{border-color:#6db8ff;box-shadow:0 0 0 3px rgba(109,184,255,.16)}"
        ".error{margin-top:14px;padding:12px 14px;border-radius:14px;background:rgba(255,138,128,.10);border:1px solid rgba(255,138,128,.18);color:#ffd9d3;font-size:14px;line-height:1.7}"
        ".actions{display:flex;flex-wrap:wrap;gap:12px;margin-top:22px}"
        ".btn{display:inline-flex;align-items:center;justify-content:center;min-height:48px;padding:0 18px;border-radius:14px;border:none;font-size:15px;font-weight:700;text-decoration:none;cursor:pointer}"
        ".primary{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032}"
        ".secondary{background:rgba(109,184,255,.12);border:1px solid rgba(109,184,255,.28);color:#e4efff}"
        ".hint{margin-top:12px;color:#96acc8;font-size:13px;line-height:1.7}"
        "@media (max-width:720px){.top,.actions{flex-direction:column}.btn{width:100%%}}"
        "</style></head><body class='lang-en'>"
        "<div class='card'>"
        "<div class='top'>"
        "<div class='pill'><span class='lang-en'>Protected Access</span><span class='lang-th'>พื้นที่ตั้งค่าที่ป้องกันไว้</span></div>"
        "<div class='lang-switch'>"
        "<button type='button' id='login-en' class='active' onclick=\"setLoginLang('en')\">ENG</button>"
        "<button type='button' id='login-th' onclick=\"setLoginLang('th')\">ไทย</button>"
        "</div></div>"
        "<h1><span class='lang-en'>Enter Cloud Access Code</span><span class='lang-th'>กรอกรหัสเข้าใช้งาน Cloud</span></h1>"
        "<p class='lang-en'>The Cloud configuration page is protected. Enter the access code to manage Telegram and cloud settings.</p>"
        "<p class='lang-th'>หน้าตั้งค่า Cloud ถูกป้องกันไว้ กรุณากรอกรหัสเข้าใช้งานก่อนจัดการค่า Telegram และค่าคลาวด์</p>"
        "<form method='POST' action='/cloud/login'>"
        "<label for='cloud_code'><span class='lang-en'>Cloud Access Code</span><span class='lang-th'>รหัสเข้าใช้งาน Cloud</span></label>"
        "<input id='cloud_code' name='cloud_code' type='password' autocomplete='current-password' autofocus>"
        "%s"
        "<div class='actions'>"
        "<button class='btn primary' type='submit'><span class='lang-en'>Unlock Cloud Settings</span><span class='lang-th'>ปลดล็อกหน้าตั้งค่า</span></button>"
        "<a class='btn secondary' href='/wifi'><span class='lang-en'>Open Wi‑Fi Setup</span><span class='lang-th'>ไปหน้าตั้งค่า Wi‑Fi</span></a>"
        "</div>"
        "<div class='hint lang-en'>Open the device IP to reach this protected login page. After login, technicians can open /maint for the maintenance dashboard.</div>"
        "<div class='hint lang-th'>เมื่อพิมพ์ IP ของเครื่อง ระบบจะพาเข้าหน้าล็อกอินนี้ก่อนเสมอ หลังล็อกอินแล้วช่างสามารถเข้า /maint เพื่อดูหน้า maintenance dashboard ได้</div>"
        "</form>"
        "</div>"
        "<script>"
        "function setLoginLang(lang){document.body.classList.remove('lang-en','lang-th');document.body.classList.add('lang-'+lang);document.documentElement.lang=lang;localStorage.setItem('cloudLang',lang);document.getElementById('login-en').classList.toggle('active',lang==='en');document.getElementById('login-th').classList.toggle('active',lang==='th');}"
        "(function(){var lang=localStorage.getItem('cloudLang');if(!lang){lang=(navigator.language||'en').toLowerCase().indexOf('th')===0?'th':'en';}setLoginLang(lang);})();"
        "</script>"
        "</body></html>",
        show_error
            ? "<div class='error'><span class='lang-en'>Access code is incorrect. Please try again.</span><span class='lang-th'>รหัสไม่ถูกต้อง กรุณาลองใหม่อีกครั้ง</span></div>"
            : "");

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return ret;
}

/* ── GET /status.json — คืน JSON สถานะ sensors ทุกตัว ── */
esp_err_t status_json_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_maintenance_auth(req);
    if (auth != ESP_OK) return auth;
    /* ตรวจสอบ I2C devices */
    bool pcf = (i2c_manager_ping(ADDR_PCF8574) == ESP_OK);
    bool pca = (i2c_manager_ping(ADDR_PCA9685) == ESP_OK);
    bool rtc = (i2c_manager_ping(ADDR_DS3231)  == ESP_OK);

    /* อ่านค่า IR sensors */
    uint8_t ir_byte = 0xFF;
    if (pcf) {
        pcf8574_set_all_input();
        pcf8574_read(&ir_byte);
    }

    /* อ่านเวลา RTC */
    char t_str[32] = "--:--:--";
    if (rtc) ds3231_get_time_str(t_str, sizeof(t_str));

    /* อ่านค่า Touch screen */
    uint16_t touch_x = 0, touch_y = 0;
    bool touch_active = false;
    ft6336u_read_touch(&touch_x, &touch_y, &touch_active);
    bool ctp = (i2c_manager_ping(ADDR_FT6336U) == ESP_OK);

    netpie_shadow_t shadow = {0};
    bool shadow_ok = netpie_shadow_copy(&shadow);
    bool netpie_connected = netpie_is_connected();
    bool offline_pending = offline_sync_has_pending_work();
    uint32_t last_rx_ticks = netpie_mqtt_get_last_rx_time();
    int last_rx_age_sec = -1;
    if (last_rx_ticks > 0) {
        uint32_t now_ticks = xTaskGetTickCount();
        uint32_t age_ticks = now_ticks - last_rx_ticks;
        last_rx_age_sec = (int)(age_ticks * portTICK_PERIOD_MS / 1000U);
    }

    const char *ip = wifi_sta_get_ip();
    bool is_online = (ip != NULL && strcmp(ip, "0.0.0.0") != 0);

    cJSON *root = cJSON_CreateObject();
    cJSON *meds = cJSON_CreateArray();
    cJSON *slot_times = cJSON_CreateArray();
    if (!root || !meds || !slot_times) {
        cJSON_Delete(root);
        cJSON_Delete(meds);
        cJSON_Delete(slot_times);
        return httpd_resp_send_500(req);
    }

    cJSON_AddBoolToObject(root, "wifi_connected", is_online);
    cJSON_AddStringToObject(root, "ip", ip ? ip : "0.0.0.0");
    cJSON_AddStringToObject(root, "ssid", wifi_sta_get_ssid());

    /* Runtime stats consumed by the /tech dashboard cards. */
    extern uint32_t s_boot_count;
    extern const char *reset_reason_str(esp_reset_reason_t reason);
    cJSON_AddNumberToObject(root, "uptime_s",
        (double)(xTaskGetTickCount() / configTICK_RATE_HZ));
    cJSON_AddNumberToObject(root, "heap_free", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "heap_min",
        (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "boot_count", (double)s_boot_count);
    cJSON_AddStringToObject(root, "reset_reason",
        reset_reason_str(esp_reset_reason()));
    cJSON_AddStringToObject(root, "idf_version", esp_get_idf_version());

    int rssi_dbm = 0;
    {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi_dbm = (int)ap.rssi;
            cJSON_AddNumberToObject(root, "rssi", (double)rssi_dbm);
        } else {
            cJSON_AddNullToObject(root, "rssi");
        }
    }
    cJSON_AddStringToObject(root, "time", t_str);  /* alias for tech dashboard */

    extern bool dispenser_emergency_active(void);
    cJSON_AddBoolToObject(root, "estop", dispenser_emergency_active());
    cJSON_AddBoolToObject(root, "pcf_present", pcf);
    cJSON_AddBoolToObject(root, "pca_present", pca);
    cJSON_AddBoolToObject(root, "rtc_present", rtc);
    cJSON_AddBoolToObject(root, "eeprom_present", false);
    cJSON_AddBoolToObject(root, "ctp_present", ctp);
    cJSON_AddBoolToObject(root, "ir_read_ok", pcf);
    char ir_hex[5];
    snprintf(ir_hex, sizeof(ir_hex), "%02X", ir_byte);
    cJSON_AddStringToObject(root, "ir_byte", ir_hex);
    cJSON_AddNumberToObject(root, "ir_p0", (ir_byte >> 0) & 1);
    cJSON_AddNumberToObject(root, "ir_p1", (ir_byte >> 1) & 1);
    cJSON_AddNumberToObject(root, "ir_p2", (ir_byte >> 2) & 1);
    cJSON_AddNumberToObject(root, "ir_p3", (ir_byte >> 3) & 1);
    cJSON_AddNumberToObject(root, "ir_p4", (ir_byte >> 4) & 1);
    cJSON_AddNumberToObject(root, "ir_p5", (ir_byte >> 5) & 1);
    cJSON_AddStringToObject(root, "rtc_time", t_str);
    cJSON_AddBoolToObject(root, "touch_active", touch_active);
    cJSON_AddNumberToObject(root, "touch_x", (int)touch_x);
    cJSON_AddNumberToObject(root, "touch_y", (int)touch_y);
    cJSON_AddBoolToObject(root, "netpie_connected", netpie_connected);
    cJSON_AddBoolToObject(root, "shadow_loaded", shadow_ok && shadow.loaded);
    cJSON_AddBoolToObject(root, "shadow_enabled", shadow_ok && shadow.enabled);
    cJSON_AddBoolToObject(root, "offline_pending", offline_pending);
    cJSON_AddNumberToObject(root, "netpie_last_rx_sec", last_rx_age_sec);

    for (int i = 0; i < 7; ++i) {
        cJSON_AddItemToArray(slot_times, cJSON_CreateString((shadow_ok && shadow.slot_time[i][0]) ? shadow.slot_time[i] : "--:--"));
    }
    cJSON_AddItemToObject(root, "slot_times", slot_times);

    for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
        cJSON *med = cJSON_CreateObject();
        if (!med) continue;
        cJSON_AddNumberToObject(med, "id", i + 1);
        cJSON_AddStringToObject(med, "name", (shadow_ok && shadow.med[i].name[0]) ? shadow.med[i].name : "");
        cJSON_AddNumberToObject(med, "count", shadow_ok ? shadow.med[i].count : 0);
        cJSON_AddNumberToObject(med, "slots", shadow_ok ? shadow.med[i].slots : 0);
        cJSON_AddItemToArray(meds, med);
    }
    cJSON_AddItemToObject(root, "meds", meds);

    char *j = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!j) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, j, strlen(j));
    cJSON_free(j);
    return ret;
}

/* ── GET /wifi — หน้า form ตั้งค่า WiFi SSID/Password ── */
esp_err_t wifi_setup_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_maintenance_auth(req);
    if (auth != ESP_OK) return auth;
    const char html[] =
    "<html><body style='background:#050a18;color:#fff;font-family:sans-serif;padding:20px'>"
    "<h2>WiFi Setup</h2><form method='POST' action='/wifi/save'>"
    "<p>SSID: <input name='ssid'></p><p>PASS: <input type='password' name='pass'></p>"
    "<button type='submit'>Save &amp; Restart</button></form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

esp_err_t cloud_login_handler(httpd_req_t *req)
{
    size_t body_len = (req->content_len > 0) ? (size_t)req->content_len : 0;
    if (body_len == 0 || body_len > 256) {
        return entry_send_login_page(req, true);
    }

    char *buf = (char *)calloc(body_len + 1, 1);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t received = 0;
    while (received < body_len) {
        int r = httpd_req_recv(req, buf + received, body_len - received);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    buf[received] = '\0';

    char code[64] = {0};
    bool has_entry_code = extract_form_value(buf, "access_code", code, sizeof(code));
    bool has_cloud_code = false;
    if (!has_entry_code) {
        has_cloud_code = extract_form_value(buf, "cloud_code", code, sizeof(code));
    }
    free(buf);

    bool owner_ok = cloud_secrets_verify_owner_override_code(code);
    bool tech_ok = cloud_secrets_verify_technician_access_code(code);
    bool admin_ok = cloud_secrets_verify_admin_access_code(code);
    bool cloud_ok = cloud_secrets_verify_cloud_access_code(code);

    if (tech_ok || admin_ok || owner_ok) {
        cloud_auth_start_session();
        maintenance_auth_start_session();

        char cookie_cloud[128];
        char cookie_maint[128];
        snprintf(cookie_cloud, sizeof(cookie_cloud), "cloud_auth=%s; Path=/; HttpOnly; SameSite=Lax", s_cloud_session_token);
        snprintf(cookie_maint, sizeof(cookie_maint), "maint_auth=%s; Path=/; HttpOnly; SameSite=Lax", s_maint_session_token);

        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/tech");
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_cloud);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_maint);
        return httpd_resp_send(req, NULL, 0);
    }

    if (!cloud_ok) {
        return has_cloud_code ? cloud_send_login_page(req, true) : entry_send_login_page(req, true);
    }

    cloud_auth_start_session();
    char cookie_cloud[128];
    snprintf(cookie_cloud, sizeof(cookie_cloud), "cloud_auth=%s; Path=/; HttpOnly; SameSite=Lax", s_cloud_session_token);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/cloud");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie_cloud);
    httpd_resp_set_hdr(req, "Set-Cookie", "maint_auth=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t maintenance_unlock_handler(httpd_req_t *req)
{
    if (!cloud_auth_cookie_valid(req)) {
        return web_redirect_to_cloud_login(req, true);
    }

    size_t body_len = (req->content_len > 0) ? (size_t)req->content_len : 0;
    if (body_len == 0 || body_len > 256) {
        return maintenance_send_login_page(req, true);
    }

    char *buf = (char *)calloc(body_len + 1, 1);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t received = 0;
    while (received < body_len) {
        int r = httpd_req_recv(req, buf + received, body_len - received);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    buf[received] = '\0';

    char code[64] = {0};
    extract_form_value(buf, "cloud_code", code, sizeof(code));
    free(buf);

    if (!cloud_secrets_verify_technician_access_code(code) &&
        !cloud_secrets_verify_owner_override_code(code)) {
        return maintenance_send_login_page(req, true);
    }

    maintenance_auth_start_session();
    char cookie[128];
    snprintf(cookie, sizeof(cookie), "maint_auth=%s; Path=/; HttpOnly; SameSite=Lax", s_maint_session_token);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/maint");
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t cloud_logout_handler(httpd_req_t *req)
{
    maintenance_auth_clear_session();
    cloud_auth_clear_session();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Set-Cookie", "cloud_auth=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
    httpd_resp_set_hdr(req, "Set-Cookie", "maint_auth=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t cloud_setup_handler(httpd_req_t *req) {
    if (!cloud_auth_cookie_valid(req)) {
        return web_redirect_to_cloud_login(req, false);
    }

    char token[200];
    char chat_id[48];
    char gs_url[320];
    char token_html[400];
    char chat_html[96];
    char url_html[640];

    snprintf(token, sizeof(token), "%s", cloud_secrets_get_telegram_token());
    snprintf(chat_id, sizeof(chat_id), "%s", cloud_secrets_get_telegram_chat_id());
    snprintf(gs_url, sizeof(gs_url), "%s", cloud_secrets_get_google_script_url());

    html_escape(token, token_html, sizeof(token_html));
    html_escape(chat_id, chat_html, sizeof(chat_html));
    html_escape(gs_url, url_html, sizeof(url_html));

    char *html = (char *)malloc(7168);
    if (!html) return ESP_ERR_NO_MEM;

    snprintf(html, 7168,
        "<!doctype html>"
        "<html lang='en'>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Cloud Config</title>"
        "<style>"
        ":root{color-scheme:dark;--text:#f4f8ff;--muted:#96acc8;--accent:#4dd7b0;--accent2:#6db8ff;}"
        "*{box-sizing:border-box}"
        "body{margin:0;font-family:'Segoe UI',Tahoma,sans-serif;background:radial-gradient(circle at top,#173860 0%%,#0b1629 38%%,#050b14 100%%);color:var(--text)}"
        ".shell{max-width:860px;margin:0 auto;padding:28px 18px 44px}"
        ".card{background:linear-gradient(180deg,rgba(17,36,61,.97),rgba(8,18,32,.98));border:1px solid rgba(255,255,255,.08);border-radius:28px;padding:28px;box-shadow:0 18px 50px rgba(0,0,0,.28)}"
        ".eyebrow{display:inline-flex;align-items:center;padding:7px 12px;border-radius:999px;background:rgba(77,215,176,.12);color:#9ae8d0;font-size:13px;font-weight:700;letter-spacing:.04em;text-transform:uppercase}"
        "h1{margin:18px 0 10px;font-size:clamp(30px,5vw,42px);line-height:1.08}"
        ".lead{margin:0 0 24px;color:var(--muted);font-size:16px;line-height:1.75}"
        ".field{margin-bottom:18px}"
        "label{display:block;margin-bottom:8px;font-size:15px;font-weight:700}"
        ".label-sub{display:block;margin-top:4px;color:var(--muted);font-size:12px;font-weight:500}"
        "input,textarea{width:100%%;border-radius:16px;border:1px solid #35567f;background:#06101e;color:var(--text);padding:14px 16px;font-size:15px;outline:none;transition:border-color .18s ease,box-shadow .18s ease}"
        "input:focus,textarea:focus{border-color:var(--accent2);box-shadow:0 0 0 3px rgba(109,184,255,.16)}"
        "textarea{min-height:132px;resize:vertical}"
        ".note{margin-top:22px;padding:16px 18px;border-radius:18px;background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.08);color:var(--muted);font-size:14px;line-height:1.7}"
        ".actions{display:flex;flex-wrap:wrap;gap:12px;margin-top:24px}"
        ".btn{display:inline-flex;align-items:center;justify-content:center;min-height:48px;padding:0 18px;border-radius:14px;border:none;font-size:15px;font-weight:700;text-decoration:none;cursor:pointer}"
        ".btn-primary{background:linear-gradient(135deg,var(--accent),#2fc4d5);color:#042032;box-shadow:0 10px 24px rgba(77,215,176,.22)}"
        ".btn-secondary{background:rgba(109,184,255,.12);color:#dbe9ff;border:1px solid rgba(109,184,255,.28)}"
        "@media (max-width:720px){.card{padding:22px}.actions{flex-direction:column}.btn{width:100%%}}"
        "</style>"
        "</head>"
        "<body>"
        "<div class='shell'>"
        "<section class='card'>"
        "<div class='eyebrow'>Cloud Setup</div>"
        "<h1>Cloud Config</h1>"
        "<p class='lead'>Use this page only for Telegram and Google Sheets settings. Other technician, monitor, Wi-Fi, and access-code functions were removed from this page.</p>"
        "<form method='POST' action='/cloud/save'>"
        "<div class='field'>"
        "<label for='tg_token'>Telegram Bot Token"
        "<span class='label-sub'>Paste the full token from BotFather.</span></label>"
        "<input id='tg_token' name='tg_token' value=\"%s\" autocomplete='off' spellcheck='false' placeholder='123456:ABC...'>"
        "</div>"
        "<div class='field'>"
        "<label for='tg_chat'>Telegram Chat ID"
        "<span class='label-sub'>Chat or user ID that should receive bot messages.</span></label>"
        "<input id='tg_chat' name='tg_chat' value=\"%s\" autocomplete='off' spellcheck='false' placeholder='8146728406'>"
        "</div>"
        "<div class='field'>"
        "<label for='gs_url'>Google Apps Script URL"
        "<span class='label-sub'>Optional. Leave empty if Google Sheets is not used.</span></label>"
        "<textarea id='gs_url' name='gs_url' spellcheck='false' placeholder='https://script.google.com/macros/s/.../exec'>%s</textarea>"
        "</div>"
        "<div class='actions'>"
        "<button class='btn btn-primary' type='submit'>บันทึก Cloud Settings</button>"
        "<a class='btn btn-secondary' href='/cloud/test?mode=message'>ทดสอบส่งข้อความ Telegram</a>"
        "<a class='btn btn-secondary' href='/cloud/test?mode=photo'>ทดสอบส่งรูป Telegram</a>"
        "<a class='btn btn-secondary' href='/cloud/logout' style='background:rgba(255,138,128,.12);color:#ffb3a8;border:1px solid rgba(255,138,128,.32);margin-left:auto'>ออกจากระบบ</a>"
        "</div>"
        "</form>"
        "<div class='note'>บันทึกก่อนแล้วค่อยทดสอบ &middot; กด \"ออกจากระบบ\" เพื่อล็อกเอ้าท์</div>"
        "</section>"
        "</div>"
        "</body></html>",
        token_html, chat_html, url_html);

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return ret;
}
esp_err_t wifi_save_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_maintenance_auth(req);
    if (auth != ESP_OK) return auth;
    size_t body_len = (req->content_len > 0) ? (size_t)req->content_len : 0;
    if (body_len == 0 || body_len > 512) return ESP_FAIL;

    char *buf = (char *)calloc(body_len + 1, 1);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t received = 0;
    while (received < body_len) {
        int r = httpd_req_recv(req, buf + received, body_len - received);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    buf[received] = '\0';

    char ssid[64]={0}, pass[64]={0};
    extract_form_value(buf, "ssid", ssid, sizeof(ssid));
    extract_form_value(buf, "pass", pass, sizeof(pass));
    free(buf);

    ESP_LOGI(TAG, "WiFi credentials saved: SSID=%s", ssid);

    const char *resp_template = "<html><body style='background:#050a18;color:#22d3a7;font-family:sans-serif;text-align:center;padding:50px'>"
                                "<h2>Credentials Saved!</h2>"
                                "<p>The device is now attempting to connect to <b>%s</b>.</p>"
                                "<p>If successful, this setup network will disappear.</p>"
                                "</body></html>";
                                
    char html[512] = {0};
    snprintf(html, sizeof(html), resp_template, ssid);
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));

    // Wait a brief moment for the HTTP response to be fully transmitted
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Instead of restarting, reconnect dynamically
    wifi_sta_reconnect(ssid, pass);
    
    return ESP_OK;
}

esp_err_t cloud_save_handler(httpd_req_t *req) {
    if (!cloud_auth_cookie_valid(req)) {
        return cloud_send_login_page(req, true);
    }

    size_t body_len = (req->content_len > 0) ? (size_t)req->content_len : 0;
    if (body_len == 0 || body_len > 1024) return ESP_FAIL;

    char *buf = (char *)calloc(body_len + 1, 1);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t received = 0;
    while (received < body_len) {
        int r = httpd_req_recv(req, buf + received, body_len - received);
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += (size_t)r;
    }
    buf[received] = '\0';

    char tg_token[200] = {0};
    char tg_chat[48] = {0};
    char gs_url[320] = {0};
    extract_form_value(buf, "tg_token", tg_token, sizeof(tg_token));
    extract_form_value(buf, "tg_chat", tg_chat, sizeof(tg_chat));
    extract_form_value(buf, "gs_url", gs_url, sizeof(gs_url));
    free(buf);

    bool ok = cloud_secrets_store(tg_token, tg_chat, gs_url);
    const char *status = ok ? "Cloud Settings Saved" : "Save Failed";
    const char *msg = ok
        ? "Telegram and Google Sheets settings were saved successfully."
        : "The device could not save the cloud settings.";
    const char *accent = ok ? "#4dd7b0" : "#ff8a80";
    const char *card_glow = ok ? "rgba(77,215,176,.18)" : "rgba(255,138,128,.18)";

    char html[2048];
    snprintf(html, sizeof(html),
        "<!doctype html>"
        "<html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Cloud Config Result</title>"
        "<style>"
        "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px;background:radial-gradient(circle at top,#16345a 0%%,#0a1425 42%%,#050b14 100%%);font-family:'Segoe UI',Tahoma,sans-serif;color:#f4f8ff}"
        ".card{max-width:720px;width:100%%;background:linear-gradient(180deg,rgba(17,36,61,.97),rgba(8,18,32,.98));border:1px solid rgba(255,255,255,.08);border-radius:28px;padding:30px;box-shadow:0 20px 46px %s}"
        ".pill{display:inline-flex;padding:7px 12px;border-radius:999px;background:rgba(255,255,255,.05);color:%s;font-size:13px;font-weight:700;letter-spacing:.04em;text-transform:uppercase}"
        "h1{margin:18px 0 12px;font-size:34px;line-height:1.15}"
        "p{margin:0;color:#a7bdd7;font-size:16px;line-height:1.8}"
        ".actions{display:flex;flex-wrap:wrap;gap:12px;margin-top:24px}"
        ".btn{display:inline-flex;align-items:center;justify-content:center;min-height:48px;padding:0 18px;border-radius:14px;font-size:15px;font-weight:700;text-decoration:none}"
        ".primary{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032}"
        "@media (max-width:720px){.actions{flex-direction:column}.btn{width:100%%}}"
        "</style></head><body>"
        "<div class='card'>"
        "<div class='pill'>Cloud Setup Result</div>"
        "<h1>%s</h1>"
        "<p>%s</p>"
        "<div class='actions'>"
        "<a class='btn primary' href='/cloud'>Back to Cloud Config</a>"
        "</div>"
        "</div>"
        "</body></html>",
        card_glow, accent, status, msg);

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}
esp_err_t cloud_test_handler(httpd_req_t *req) {
    if (!cloud_auth_cookie_valid(req)) {
        return cloud_send_login_page(req, true);
    }

    char query[64] = {0};
    char mode[16] = "message";
    bool photo_mode = false;

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char mode_raw[16] = {0};
        if (httpd_query_key_value(query, "mode", mode_raw, sizeof(mode_raw)) == ESP_OK) {
            snprintf(mode, sizeof(mode), "%s", mode_raw);
            for (size_t i = 0; mode[i] != '\0'; ++i) {
                mode[i] = (char)tolower((unsigned char)mode[i]);
            }
        }
    }
    photo_mode = (strcmp(mode, "photo") == 0);

    bool has_tg = cloud_secrets_has_telegram();
    if (has_tg) {
        if (photo_mode) {
            telegram_send_test_snapshot();
        } else {
            telegram_send_test_message();
        }
    }

    const char *pill_en = has_tg ? "Telegram Test Sent" : "Telegram Setup Missing";
    const char *pill_th = has_tg ? "ส่งคำสั่งทดสอบแล้ว" : "ยังไม่ได้ตั้งค่า Telegram";
    const char *title_en = has_tg
        ? (photo_mode ? "Photo test request sent" : "Message test request sent")
        : "Telegram credentials are not ready";
    const char *title_th = has_tg
        ? (photo_mode ? "ส่งคำขอทดสอบภาพแล้ว" : "ส่งคำขอทดสอบข้อความแล้ว")
        : "ข้อมูล Telegram ยังไม่พร้อมใช้งาน";
    const char *msg_en = has_tg
        ? (photo_mode
            ? "The device is trying to capture a snapshot and send it to Telegram now. Please check your chat in a few seconds."
            : "The device has queued a test Telegram message. Please check your chat in a few seconds.")
        : "Please save a valid Telegram Bot Token and Chat ID on the Cloud Config page before running a test.";
    const char *msg_th = has_tg
        ? (photo_mode
            ? "เครื่องกำลังพยายามถ่ายภาพและส่งไปยัง Telegram ตอนนี้ กรุณาตรวจในแชทภายในไม่กี่วินาที"
            : "เครื่องได้คิวข้อความทดสอบ Telegram แล้ว กรุณาตรวจในแชทภายในไม่กี่วินาที")
        : "กรุณาบันทึก Telegram Bot Token และ Chat ID ให้ถูกต้องในหน้า Cloud Config ก่อนทดสอบ";
    const char *accent = has_tg ? "#4dd7b0" : "#ff8a80";
    const char *card_glow = has_tg ? "rgba(77,215,176,.18)" : "rgba(255,138,128,.18)";

    char *html = (char *)malloc(6144);
    if (!html) return ESP_ERR_NO_MEM;
    snprintf(html, 6144,
        "<!doctype html>"
        "<html lang='en'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Telegram Test Result</title>"
        "<style>"
        "body.lang-en .lang-th{display:none!important}body.lang-th .lang-en{display:none!important}"
        "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px;background:radial-gradient(circle at top,#16345a 0%%,#0a1425 42%%,#050b14 100%%);font-family:'Segoe UI',Tahoma,sans-serif;color:#f4f8ff}"
        ".card{max-width:760px;width:100%%;background:linear-gradient(180deg,rgba(17,36,61,.97),rgba(8,18,32,.98));border:1px solid rgba(255,255,255,.08);border-radius:28px;padding:30px;box-shadow:0 20px 46px %s}"
        ".top{display:flex;justify-content:space-between;align-items:flex-start;gap:12px;flex-wrap:wrap}"
        ".pill{display:inline-flex;padding:7px 12px;border-radius:999px;background:rgba(255,255,255,.05);color:%s;font-size:13px;font-weight:700;letter-spacing:.04em;text-transform:uppercase}"
        ".lang-switch{display:inline-flex;padding:5px;border-radius:999px;background:rgba(3,12,22,.55);border:1px solid rgba(255,255,255,.09);gap:6px}"
        ".lang-switch button{border:none;background:transparent;color:#d7e5f7;min-width:74px;height:38px;border-radius:999px;font-size:14px;font-weight:700;cursor:pointer}"
        ".lang-switch button.active{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032}"
        "h1{margin:18px 0 12px;font-size:34px;line-height:1.15}"
        "p{margin:0;color:#a7bdd7;font-size:16px;line-height:1.8}"
        ".actions{display:flex;flex-wrap:wrap;gap:12px;margin-top:24px}"
        ".btn{display:inline-flex;align-items:center;justify-content:center;min-height:48px;padding:0 18px;border-radius:14px;font-size:15px;font-weight:700;text-decoration:none}"
        ".primary{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032}"
        ".secondary{background:rgba(109,184,255,.12);border:1px solid rgba(109,184,255,.28);color:#e4efff}"
        ".ghost{background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.1);color:#f4f8ff}"
        "@media (max-width:720px){.top,.actions{flex-direction:column}.btn{width:100%%}}"
        "</style></head><body class='lang-en'>"
        "<div class='card'>"
        "<div class='top'>"
        "<div class='pill'><span class='lang-en'>%s</span><span class='lang-th'>%s</span></div>"
        "<div class='lang-switch'>"
        "<button type='button' id='test-en' class='active' onclick=\"setTestLang('en')\">ENG</button>"
        "<button type='button' id='test-th' onclick=\"setTestLang('th')\">ไทย</button>"
        "</div>"
        "</div>"
        "<h1><span class='lang-en'>%s</span><span class='lang-th'>%s</span></h1>"
        "<p><span class='lang-en'>%s</span><span class='lang-th'>%s</span></p>"
        "<div class='actions'>"
        "<a class='btn primary' href='/cloud'><span class='lang-en'>Back to Cloud Config</span><span class='lang-th'>กลับหน้าตั้งค่า Cloud</span></a>"
        "<a class='btn secondary' href='/cloud/test?mode=message'><span class='lang-en'>Test Message Again</span><span class='lang-th'>ทดสอบข้อความอีกครั้ง</span></a>"
        "<a class='btn ghost' href='/cloud/test?mode=photo'><span class='lang-en'>Test Photo</span><span class='lang-th'>ทดสอบส่งภาพ</span></a>"
        "</div>"
        "</div>"
        "<script>"
        "function setTestLang(lang){document.body.classList.remove('lang-en','lang-th');document.body.classList.add('lang-'+lang);document.documentElement.lang=lang;localStorage.setItem('cloudLang',lang);document.getElementById('test-en').classList.toggle('active',lang==='en');document.getElementById('test-th').classList.toggle('active',lang==='th');}"
        "(function(){var lang=localStorage.getItem('cloudLang');if(!lang){lang=(navigator.language||'en').toLowerCase().indexOf('th')===0?'th':'en';}setTestLang(lang);})();"
        "</script>"
        "</body></html>",
        card_glow, accent, pill_en, pill_th, title_en, title_th, msg_en, msg_th);

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return ret;
}

esp_err_t wifi_scan_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_maintenance_auth(req);
    if (auth != ESP_OK) return auth;
    wifi_ap_record_t aps[15];
    
    wifi_sta_start_scan();
    int count = -1;
    for (int i = 0; i < 60; i++) { // Wait up to 6 seconds
        vTaskDelay(pdMS_TO_TICKS(100));
        count = wifi_sta_get_scan_results(aps, 15);
        if (count >= 0) break;
    }
    
    if (count < 0) {
        count = 0; // Timeout, return empty array
    }
    
    char *j = malloc(2048);
    if (!j) return ESP_FAIL;
    strcpy(j, "[");
    for (int i=0; i<count; i++) {
        char item[128];
        snprintf(item, sizeof(item), "{\"ssid\":\"%s\",\"rssi\":%d}%s", 
            aps[i].ssid, aps[i].rssi, (i==count-1)?"":",");
        strncat(j, item, 2048 - strlen(j) - 1);
    }
    strncat(j, "]", 2048 - strlen(j) - 1);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, j, strlen(j));
    free(j);
    return ESP_OK;
}

/* ── POST /wifi/forget — ลืม WiFi ปัจจุบัน ── */
esp_err_t wifi_forget_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_maintenance_auth(req);
    if (auth != ESP_OK) return auth;
    wifi_sta_forget();
    
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t web_require_maintenance_api_auth(httpd_req_t *req)
{
    if (cloud_auth_cookie_valid(req)) {
        return ESP_OK;
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false,\"error\":\"login_required\"}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t access_state_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char json[256];
    snprintf(json, sizeof(json),
             "{\"cloud_code\":\"%s\",\"tech_code\":\"%s\",\"admin_code\":\"%s\"}",
             cloud_secrets_get_cloud_access_code(),
             cloud_secrets_get_technician_access_code(),
             cloud_secrets_get_admin_access_code());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t access_save_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char body[384] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len < 0) return ESP_FAIL;
    body[len] = '\0';

    char cloud[32] = {0};
    char tech[32] = {0};
    char admin[32] = {0};
    if (!extract_form_value(body, "cloud_code", cloud, sizeof(cloud))) {
        extract_form_value(body, "cloud", cloud, sizeof(cloud));
    }
    if (!extract_form_value(body, "tech_code", tech, sizeof(tech))) {
        extract_form_value(body, "tech", tech, sizeof(tech));
    }
    if (!extract_form_value(body, "admin_code", admin, sizeof(admin))) {
        extract_form_value(body, "admin", admin, sizeof(admin));
    }

    bool ok = cloud_secrets_store_access_codes(cloud, tech, admin);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
}

// Sound config now reads/writes the live g_snd_* globals (used by every
// dispenser_scheduler / ui_* play call) and persists via settings_save_nvs,
// so changes from the web survive reboots and immediately affect playback.
extern int g_snd_alarm;
extern int g_snd_disp_th;
extern int g_snd_return_th;
extern int g_snd_nomeds_th;
extern int g_snd_disp_en;
extern int g_snd_return_en;
extern int g_snd_nomeds_en;
extern int g_snd_button;
extern int g_snd_volup_th;
extern int g_snd_volup_en;
extern int g_snd_voldn_th;
extern int g_snd_voldn_en;
extern int g_alert_volume;
extern void settings_save_nvs(void);

static int read_form_int_or_keep(const char *body, const char *key, int current, int min_v, int max_v)
{
    char tmp[16] = {0};
    if (!extract_form_value(body, key, tmp, sizeof(tmp))) {
        return current;
    }

    int value = atoi(tmp);
    if (value < min_v) value = min_v;
    if (value > max_v) value = max_v;
    return value;
}

esp_err_t sound_state_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_maintenance_api_auth(req);
    if (auth != ESP_OK) return auth;

    char json[384];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"volume\":%d,\"alarm\":%d,\"disp_th\":%d,\"ret_th\":%d,"
             "\"nomeds_th\":%d,\"disp_en\":%d,\"ret_en\":%d,\"nomeds_en\":%d,\"button\":%d,"
             "\"volup_th\":%d,\"volup_en\":%d,\"voldn_th\":%d,\"voldn_en\":%d}",
             g_alert_volume,
             g_snd_alarm, g_snd_disp_th, g_snd_return_th,
             g_snd_nomeds_th, g_snd_disp_en, g_snd_return_en,
             g_snd_nomeds_en, g_snd_button,
             g_snd_volup_th, g_snd_volup_en, g_snd_voldn_th, g_snd_voldn_en);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t sound_save_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_maintenance_api_auth(req);
    if (auth != ESP_OK) return auth;

    char body[256] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len < 0) return ESP_FAIL;
    body[len] = '\0';

    char volume[16] = {0};
    if (extract_form_value(body, "volume", volume, sizeof(volume))) {
        int v = atoi(volume);
        if (v < 0) v = 0;
        if (v > 30) v = 30;
        g_alert_volume = v;
        dfplayer_set_volume((uint8_t)v);
    }

    g_snd_alarm     = read_form_int_or_keep(body, "alarm",     g_snd_alarm,     1, 999);
    g_snd_disp_th   = read_form_int_or_keep(body, "disp_th",   g_snd_disp_th,   1, 999);
    g_snd_return_th = read_form_int_or_keep(body, "ret_th",    g_snd_return_th, 1, 999);
    g_snd_nomeds_th = read_form_int_or_keep(body, "nomeds_th", g_snd_nomeds_th, 1, 999);
    g_snd_disp_en   = read_form_int_or_keep(body, "disp_en",   g_snd_disp_en,   1, 999);
    g_snd_return_en = read_form_int_or_keep(body, "ret_en",    g_snd_return_en, 1, 999);
    g_snd_nomeds_en = read_form_int_or_keep(body, "nomeds_en", g_snd_nomeds_en, 1, 999);
    g_snd_button    = read_form_int_or_keep(body, "button",    g_snd_button,    1, 999);
    g_snd_volup_th  = read_form_int_or_keep(body, "volup_th",  g_snd_volup_th,  1, 999);
    g_snd_volup_en  = read_form_int_or_keep(body, "volup_en",  g_snd_volup_en,  1, 999);
    g_snd_voldn_th  = read_form_int_or_keep(body, "voldn_th",  g_snd_voldn_th,  1, 999);
    g_snd_voldn_en  = read_form_int_or_keep(body, "voldn_en",  g_snd_voldn_en,  1, 999);

    // Persist to NVS so the choice survives reboots.
    settings_save_nvs();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t sound_play_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_maintenance_api_auth(req);
    if (auth != ESP_OK) return auth;

    char query[64] = {0};
    char track_s[16] = {0};
    int track = g_snd_alarm;
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "track", track_s, sizeof(track_s)) == ESP_OK) {
        track = atoi(track_s);
    }
    if (track < 1 || track > 999) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid_track\"}", HTTPD_RESP_USE_STRLEN);
    }

    dfplayer_play_track_force_vol((uint16_t)track, (uint8_t)g_alert_volume);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t sensors_json_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_maintenance_api_auth(req);
    if (auth != ESP_OK) return auth;

    const pill_sensor_status_t *s = pill_sensor_status_get_all();
    char json[2048];
    size_t off = 0;
    off += snprintf(json + off, sizeof(json) - off, "{\"enabled\":%s,\"channels\":[",
#if ENABLE_VL53_PILL_SENSORS
                    "true"
#else
                    "false"
#endif
    );
    for (int i = 0; i < PILL_SENSOR_COUNT && off < sizeof(json); i++) {
        off += snprintf(json + off, sizeof(json) - off,
                        "%s{\"ch\":%d,\"idx\":%d,\"addr\":%u,\"present\":%s,\"valid\":%s,"
                        "\"raw_mm\":%d,\"filtered_mm\":%d,\"pill_count\":%d,"
                        "\"is_empty\":%s,\"is_full\":%s,"
                        "\"full_dist_mm\":%d,\"pill_height_mm\":%d,\"max_pills\":%d}",
                        i ? "," : "", i, i + 1, (unsigned)s[i].address,
                        s[i].present ? "true" : "false",
                        s[i].valid ? "true" : "false",
                        s[i].raw_mm, s[i].filtered_mm, s[i].pill_count,
                        s[i].is_empty ? "true" : "false",
                        s[i].is_full ? "true" : "false",
                        s[i].full_dist_mm, s[i].pill_height_mm, s[i].max_pills);
    }
    snprintf(json + off, sizeof(json) - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t sensors_config_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char body[192] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len < 0) return ESP_FAIL;
    body[len] = '\0';

    char ch_s[8] = {0};
    char full_s[16] = {0};
    char height_s[16] = {0};
    char max_s[16] = {0};
    extract_form_value(body, "ch", ch_s, sizeof(ch_s));
    extract_form_value(body, "full_dist_mm", full_s, sizeof(full_s));
    extract_form_value(body, "pill_height_mm", height_s, sizeof(height_s));
    extract_form_value(body, "max_pills", max_s, sizeof(max_s));

    int ch = atoi(ch_s);
    int full = atoi(full_s);
    int height = atoi(height_s);
    int max_pills = atoi(max_s);
    // Sanity-check ranges so a typo doesn't permanently corrupt cal NVS:
    //  full   1..500 mm   (cartridge length sanely bounded)
    //  height 1..50 mm    (a 50 mm "pill" is a hard sanity limit)
    //  max    1..200      (200 pills × 50 mm = 10 m, well past full range)
    //  full + max*height should not exceed VL53 max range (8190 mm).
    if (ch < 0 || ch >= PILL_SENSOR_COUNT ||
        full <= 0   || full > 500   ||
        height <= 0 || height > 50  ||
        max_pills <= 0 || max_pills > 200 ||
        (full + max_pills * height) > 8190) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid_sensor_config\"}", HTTPD_RESP_USE_STRLEN);
    }

    vl53l0x_set_channel_config(ch, full, height, max_pills);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

/* POST /sensors/cal_capture — capture the current sensor reading as either
 * the "full" distance (cartridge loaded) or the "empty" distance (cartridge
 * empty). When mode=empty, pill_height_mm is derived from the gap between
 * the empty reading and the saved full distance, divided by max_pills.
 *
 * body: ch=N&mode=full   → full_dist_mm = current filtered_mm
 *       ch=N&mode=empty  → pill_height_mm = (current - full_dist) / max_pills
 */
esp_err_t sensors_capture_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char body[96] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len < 0) return ESP_FAIL;
    body[len] = '\0';

    char ch_s[8] = {0};
    char mode[16] = {0};
    extract_form_value(body, "ch", ch_s, sizeof(ch_s));
    extract_form_value(body, "mode", mode, sizeof(mode));
    int ch = atoi(ch_s);

    if (ch < 0 || ch >= PILL_SENSOR_COUNT) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid_ch\"}", HTTPD_RESP_USE_STRLEN);
    }

    const pill_sensor_status_t *all = pill_sensor_status_get_all();
    const pill_sensor_status_t *s = &all[ch];
    if (!s->present || !s->valid || s->filtered_mm <= 0) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"no_live_reading\"}", HTTPD_RESP_USE_STRLEN);
    }

    int captured = s->filtered_mm;
    int full_dist = s->full_dist_mm;
    int pill_h    = s->pill_height_mm;
    int max_pills = s->max_pills > 0 ? s->max_pills : 30;
    char resp[160];

    if (strcmp(mode, "full") == 0) {
        full_dist = captured;
        if (pill_h <= 0) pill_h = 1;
        vl53l0x_set_channel_config(ch, full_dist, pill_h, max_pills);
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"mode\":\"full\",\"captured_mm\":%d,"
                 "\"full_dist_mm\":%d,\"pill_height_mm\":%d,\"max_pills\":%d}",
                 captured, full_dist, pill_h, max_pills);
    } else if (strcmp(mode, "empty") == 0) {
        if (full_dist <= 0 || captured <= full_dist || max_pills <= 0) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req,
                "{\"ok\":false,\"error\":\"need_full_first_or_invalid_geometry\"}",
                HTTPD_RESP_USE_STRLEN);
        }
        int derived = (captured - full_dist) / max_pills;
        if (derived <= 0) derived = 1;
        pill_h = derived;
        vl53l0x_set_channel_config(ch, full_dist, pill_h, max_pills);
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"mode\":\"empty\",\"captured_mm\":%d,"
                 "\"full_dist_mm\":%d,\"pill_height_mm\":%d,\"max_pills\":%d}",
                 captured, full_dist, pill_h, max_pills);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"mode_must_be_full_or_empty\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

esp_err_t audit_json_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_maintenance_api_auth(req);
    if (auth != ESP_OK) return auth;

    dispenser_audit_entry_t entries[32];
    size_t n = dispenser_audit_get(entries, 32);

    char *out = (char *)malloc(4096);
    if (!out) return httpd_resp_send_500(req);
    size_t off = 0;

    const netpie_shadow_t *sh = netpie_get_shadow();
    off += snprintf(out + off, 4096 - off, "{\"ok\":true,\"count\":%u,\"entries\":[", (unsigned)n);
    for (size_t i = 0; i < n && off < 4000; ++i) {
        const dispenser_audit_entry_t *e = &entries[i];
        const char *name = (sh && e->med_idx >= 0 && e->med_idx < DISPENSER_MED_COUNT &&
                            sh->med[e->med_idx].name[0]) ? sh->med[e->med_idx].name : "";
        off += snprintf(out + off, 4096 - off,
                        "%s{\"ts\":%lu,\"med\":%d,\"name\":\"%s\",\"from\":%d,\"to\":%d,\"src\":\"%c\"}",
                        i ? "," : "", (unsigned long)e->timestamp,
                        e->med_idx + 1, name, e->from_count, e->to_count, e->source);
    }
    snprintf(out + off, 4096 - off, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ret;
}

esp_err_t sensors_page_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_maintenance_auth(req);
    if (auth != ESP_OK) return auth;

    const char *html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Pill Sensors</title><style>"
        "body{margin:0;background:#07131f;color:#eef;font-family:Segoe UI,Tahoma,sans-serif;padding:24px}"
        ".top{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}"
        "a,button{border:1px solid #246070;background:#0d3440;color:#dff;border-radius:12px;padding:10px 14px;text-decoration:none;font-weight:700}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px;margin-top:18px}"
        ".card{background:#101f2a;border:1px solid #223747;border-radius:18px;padding:16px}"
        ".badge{float:right;border-radius:999px;padding:5px 10px;font-size:12px;font-weight:800;background:#35404f}"
        ".ok{background:#115b38}.warn{background:#6b530b}.bad{background:#64202b}"
        ".dist{font-size:34px;font-weight:900;margin:10px 0}.muted{color:#9eb2c7;font-size:13px;line-height:1.6}"
        ".bar{height:9px;background:#1b3140;border-radius:99px;overflow:hidden}.fill{height:100%;background:#4dd7b0}"
        "input{width:70px;background:#07131f;color:#eef;border:1px solid #29475b;border-radius:9px;padding:6px;margin:4px}"
        "</style></head><body>"
        "<div class='top'><div><h1>Pill Level Sensors</h1><div class='muted'>VL53L0X x6 through TCA9548A multiplexer. Live values update every 2 seconds.</div></div><a href='/maint'>Back to Dashboard</a></div>"
        "<div id='grid' class='grid'></div>"
        "<script>"
        "function pct(s){return (!s.valid||s.pill_count<0||!s.max_pills)?0:Math.max(0,Math.min(100,Math.round(s.pill_count/s.max_pills*100)));}"
        "function save(ch){const b=new URLSearchParams();b.set('ch',ch);b.set('full_dist_mm',document.getElementById('fd'+ch).value);b.set('pill_height_mm',document.getElementById('ph'+ch).value);b.set('max_pills',document.getElementById('mp'+ch).value);fetch('/sensors/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(tick)}"
        "async function tick(){let j=await (await fetch('/sensors.json',{cache:'no-store'})).json();let g=document.getElementById('grid');g.innerHTML='';(j.channels||[]).forEach(s=>{let p=pct(s);let state=!s.present?'NO SENSOR':(!s.valid?'NO READING':(s.is_empty?'EMPTY':(p<25?'LOW':'READY')));let cls=!s.present||s.is_empty?'bad':(!s.valid||p<25?'warn':'ok');g.innerHTML+=`<div class='card'><span class='badge ${cls}'>${state}</span><h3>Module ${s.idx}</h3><div class='dist'>${s.valid?s.filtered_mm:'--'} <small>mm</small></div><div class='bar'><div class='fill' style='width:${p}%'></div></div><p>${s.pill_count>=0?s.pill_count:'-'}/${s.max_pills} pills</p><div class='muted'>Addr 0x${Number(s.addr).toString(16)} | Raw ${s.raw_mm||'--'} mm</div><div><input id='fd${s.ch}' value='${s.full_dist_mm}'><input id='ph${s.ch}' value='${s.pill_height_mm}'><input id='mp${s.ch}' value='${s.max_pills}'><button onclick='save(${s.ch})'>Save</button></div></div>`})}"
        "tick();setInterval(tick,2000);"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}
