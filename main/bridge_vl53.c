#include "bridge_vl53.h"
#include "config.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bridge_vl53";

/* Latest parsed values + the millisecond tick they were written at. */
static int      s_dist_mm[BRIDGE_VL53_NUM_SENSORS] = {-1, -1, -1, -1, -1, -1};
static uint32_t s_last_update_ms = 0;
static SemaphoreHandle_t s_lock  = NULL;
static bool     s_started        = false;

/* Read-window tick: 0 = never armed (so the 0-difference check below
 * doesn't accidentally accept readings at boot). Any non-zero value is
 * a millisecond-since-boot timestamp of the most recent arm; readings
 * are committed only while (now - s_arm_ms) < READ_WINDOW_MS. */
static volatile uint32_t s_arm_ms = 0;
#define READ_WINDOW_MS 5000

#define LINE_BUF_SIZE 96

/* Parse "[vl53:]<int>,<int>,...,<int>" into six integers. Permissive
 * about whitespace, the optional "vl53:" prefix, and stray CRs. */
static bool parse_csv_line(const char *line, int out[BRIDGE_VL53_NUM_SENSORS])
{
    if (!line) return false;
    if (strncmp(line, "vl53:", 5) == 0) line += 5;

    int got = 0;
    const char *p = line;
    while (*p && got < BRIDGE_VL53_NUM_SENSORS) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) return false;
        out[got++] = (int)v;
        p = end;
    }
    return (got == BRIDGE_VL53_NUM_SENSORS);
}

static void bridge_vl53_task(void *arg)
{
    (void)arg;

    uart_config_t cfg = {
        .baud_rate  = BRIDGE_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(BRIDGE_UART_NUM, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %d", err);
        vTaskDelete(NULL);
        return;
    }
    err = uart_param_config(BRIDGE_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %d", err);
        vTaskDelete(NULL);
        return;
    }
    err = uart_set_pin(BRIDGE_UART_NUM,
                       BRIDGE_TX_PIN,
                       BRIDGE_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %d", err);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "UART%d up on RX=%d TX=%d @ %d baud — listening for C3 frames",
             (int)BRIDGE_UART_NUM, BRIDGE_RX_PIN, BRIDGE_TX_PIN, BRIDGE_UART_BAUD);

    char line[LINE_BUF_SIZE];
    size_t fill = 0;

    while (1) {
        uint8_t byte;
        int n = uart_read_bytes(BRIDGE_UART_NUM, &byte, 1, pdMS_TO_TICKS(200));
        if (n <= 0) continue;

        if (byte == '\r') continue;
        if (byte == '\n') {
            line[fill] = '\0';
            int parsed[BRIDGE_VL53_NUM_SENSORS];
            if (parse_csv_line(line, parsed)) {
                /* Only commit values that arrive inside the active
                 * read window. The UART task still parses everything
                 * the C3 sends (so we don't accumulate stale bytes in
                 * the kernel buffer) but values outside the window are
                 * discarded — the dashboard freezes on the last
                 * committed values until the operator presses the
                 * button again. */
                uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                uint32_t arm = s_arm_ms;
                bool in_window = (arm != 0) && ((now - arm) < READ_WINDOW_MS);
                if (in_window) {
                    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                        for (int i = 0; i < BRIDGE_VL53_NUM_SENSORS; i++) {
                            s_dist_mm[i] = parsed[i];
                        }
                        s_last_update_ms = now;
                        xSemaphoreGive(s_lock);
                    }
                }
            } else if (fill > 0 && strchr(line, ',') != NULL) {
                ESP_LOGD(TAG, "Malformed: %s", line);
            }
            fill = 0;
            continue;
        }

        if (fill < sizeof(line) - 1) {
            line[fill++] = (char)byte;
        } else {
            fill = 0;
        }
    }
}

bool bridge_vl53_start(void)
{
    if (s_started) return true;

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            ESP_LOGE(TAG, "mutex alloc failed");
            return false;
        }
    }

    BaseType_t ok = xTaskCreate(bridge_vl53_task, "bridge_vl53", 4096, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    s_started = true;
    return true;
}

uint32_t bridge_vl53_get(int *out_mm)
{
    if (!out_mm) return 0;
    uint32_t ts = 0;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (int i = 0; i < BRIDGE_VL53_NUM_SENSORS; i++) {
            out_mm[i] = s_dist_mm[i];
        }
        ts = s_last_update_ms;
        xSemaphoreGive(s_lock);
    } else {
        for (int i = 0; i < BRIDGE_VL53_NUM_SENSORS; i++) {
            out_mm[i] = -1;
        }
    }
    return ts;
}

void bridge_vl53_arm_read(void)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (now == 0) now = 1;   /* keep 0 reserved for "never armed" */
    s_arm_ms = now;
    ESP_LOGI(TAG, "Read window armed — %d ms", READ_WINDOW_MS);
}

uint32_t bridge_vl53_remaining_ms(void)
{
    uint32_t arm = s_arm_ms;
    if (arm == 0) return 0;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t elapsed = now - arm;
    if (elapsed >= READ_WINDOW_MS) return 0;
    return READ_WINDOW_MS - elapsed;
}

/* ── Web layer (self-contained — no auth, isolated from dispense) ── */

static const char VL53_HTML[] =
"<!DOCTYPE html><html lang=\"th\"><head>"
"<meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>VL53 Bridge — On-demand</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:'Segoe UI',Tahoma,sans-serif;background:#0f172a;color:#e2e8f0;"
"min-height:100vh;padding:1.5rem 1rem}"
".wrap{max-width:720px;margin:0 auto}"
"h1{font-size:1.4rem;margin-bottom:.25rem;display:flex;align-items:center;gap:.5rem}"
".sub{color:#94a3b8;font-size:.9rem;margin-bottom:1.25rem}"
".readbtn{display:block;width:100%;padding:1rem 1.25rem;background:linear-gradient(135deg,#1d4ed8,#38bdf8);"
"color:#fff;border:none;border-radius:14px;font-size:1.1rem;font-weight:900;cursor:pointer;"
"box-shadow:0 12px 24px rgba(29,78,216,.32);transition:.15s;margin-bottom:1.25rem;"
"font-family:inherit}"
".readbtn:active{transform:scale(.98)}"
".readbtn:disabled{background:#475569;box-shadow:none;cursor:not-allowed}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:.9rem}"
".sensor{background:#1e293b;border:1px solid #334155;border-radius:14px;padding:1rem 1.1rem}"
".sensor.live{border-color:#38bdf8;box-shadow:0 0 0 1px #38bdf8}"
".lbl{color:#94a3b8;font-size:.78rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase}"
".val{font-size:2.2rem;font-weight:900;color:#38bdf8;margin-top:.3rem}"
".unit{font-size:1rem;color:#94a3b8;font-weight:600;margin-left:.25rem}"
".bad{color:#ef4444}"
".meta{margin-top:1.5rem;padding:1rem;background:#1e293b;border-radius:12px;font-size:.88rem;"
"color:#94a3b8;border:1px solid #334155}"
".dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#94a3b8;"
"margin-right:.45rem;vertical-align:middle}"
".dot.live{background:#22c55e;animation:p 1s infinite}"
".dot.done{background:#0ea5e9}"
"@keyframes p{0%,100%{opacity:1}50%{opacity:.4}}"
"</style></head><body><div class=\"wrap\">"
"<h1>\xf0\x9f\x93\x8d VL53 Bridge</h1>"
"<div class=\"sub\">\xe0\xb8\xa3\xe0\xb8\xb0\xe0\xb8\xa2\xe0\xb8\xb0\xe0\xb8\x88\xe0\xb8\xb2\xe0\xb8\x81 ESP32-C3 (TCA9548A + 6 \xc3\x97 VL53L0X) \xe2\x80\x94 \xe0\xb8\x81\xe0\xb8\x94\xe0\xb8\x9b\xe0\xb8\xb8\xe0\xb9\x88\xe0\xb8\xa1\xe0\xb9\x80\xe0\xb8\x9e\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb8\xad\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb8\x99\xe0\xb8\x84\xe0\xb9\x88\xe0\xb8\xb2 5 \xe0\xb8\xa7\xe0\xb8\xb4\xe0\xb8\x99\xe0\xb8\xb2\xe0\xb8\x97\xe0\xb8\xb5</div>"
"<button class=\"readbtn\" id=\"readbtn\" onclick=\"startRead()\">\xe2\x96\xb6 \xe0\xb8\xad\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb8\x99\xe0\xb8\x84\xe0\xb9\x88\xe0\xb8\xb2 (5 \xe0\xb8\xa7\xe0\xb8\xb4\xe0\xb8\x99\xe0\xb8\xb2\xe0\xb8\x97\xe0\xb8\xb5)</button>"
"<div class=\"grid\" id=\"grid\"></div>"
"<div class=\"meta\" id=\"meta\"><span class=\"dot\"></span>\xe0\xb8\x9e\xe0\xb8\xa3\xe0\xb9\x89\xe0\xb8\xad\xe0\xb8\xa1 \xe2\x80\x94 \xe0\xb8\x81\xe0\xb8\x94\xe0\xb8\x9b\xe0\xb8\xb8\xe0\xb9\x88\xe0\xb8\xa1\xe0\xb8\x94\xe0\xb9\x89\xe0\xb8\xb2\xe0\xb8\x99\xe0\xb8\x9a\xe0\xb8\x99\xe0\xb9\x80\xe0\xb8\x9e\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb9\x80\xe0\xb8\xa3\xe0\xb8\xb4\xe0\xb9\x88\xe0\xb8\xa1\xe0\xb8\xad\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb8\x99</div>"
"</div><script>"
"const G=document.getElementById('grid'),M=document.getElementById('meta'),B=document.getElementById('readbtn');"
"for(let i=0;i<6;i++){const c=document.createElement('div');c.className='sensor';c.id='s'+i;"
"c.innerHTML='<div class=\"lbl\">Sensor '+i+'</div><div class=\"val\">--<span class=\"unit\">mm</span></div>';G.appendChild(c)}"
"let pollT=null;"
"function paint(d){for(let i=0;i<6;i++){const el=document.getElementById('s'+i),v=d.mm[i];"
"const vEl=el.querySelector('.val');"
"if(v<0||v>9999){vEl.innerHTML='<span class=\"bad\">--</span><span class=\"unit\">mm</span>'}"
"else{vEl.innerHTML=v+'<span class=\"unit\">mm</span>'}"
"el.className='sensor'+(d.remaining_ms>0?' live':'')}}"
"async function poll(){try{const r=await fetch('/vl53.json',{cache:'no-store'});if(!r.ok)throw 0;"
"const d=await r.json();paint(d);"
"const rem=d.remaining_ms;"
"if(rem>0){M.innerHTML='<span class=\"dot live\"></span>\xe0\xb8\x81\xe0\xb8\xb3\xe0\xb8\xa5\xe0\xb8\xb1\xe0\xb8\x87\xe0\xb8\xad\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb8\x99... '+Math.ceil(rem/1000)+' \xe0\xb8\xa7\xe0\xb8\xb4\xe0\xb8\x99\xe0\xb8\xb2\xe0\xb8\x97\xe0\xb8\xb5'}"
"else{stopPoll();M.innerHTML='<span class=\"dot done\"></span>\xe0\xb8\xad\xe0\xb9\x88\xe0\xb8\xb2\xe0\xb8\x99\xe0\xb9\x80\xe0\xb8\xaa\xe0\xb8\xa3\xe0\xb9\x87\xe0\xb8\x88 \xe2\x80\x94 \xe0\xb8\x81\xe0\xb8\x94\xe0\xb8\xad\xe0\xb8\xb5\xe0\xb8\x81\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb8\xb1\xe0\xb9\x89\xe0\xb8\x87\xe0\xb9\x80\xe0\xb8\x9e\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb8\xa3\xe0\xb8\xb5\xe0\xb9\x80\xe0\xb8\x9f\xe0\xb8\xa3\xe0\xb8\xa1';B.disabled=false;}}"
"catch(e){stopPoll();M.innerHTML='<span class=\"dot\"></span>\xe0\xb9\x80\xe0\xb8\x8a\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb8\xa1\xe0\xb8\x95\xe0\xb9\x88\xe0\xb8\xad\xe0\xb9\x80\xe0\xb8\x84\xe0\xb8\xa3\xe0\xb8\xb7\xe0\xb9\x88\xe0\xb8\xad\xe0\xb8\x87\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb9\x88\xe0\xb9\x84\xe0\xb8\x94\xe0\xb9\x89';B.disabled=false;}}"
"function stopPoll(){if(pollT){clearInterval(pollT);pollT=null}}"
"async function startRead(){B.disabled=true;stopPoll();"
"try{await fetch('/vl53/read',{method:'POST'});poll();pollT=setInterval(poll,400);}"
"catch(e){M.innerHTML='<span class=\"dot\"></span>\xe0\xb8\xaa\xe0\xb9\x88\xe0\xb8\x87\xe0\xb8\x84\xe0\xb8\xb3\xe0\xb8\xaa\xe0\xb8\xb1\xe0\xb9\x88\xe0\xb8\x87\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb9\x88\xe0\xb9\x84\xe0\xb8\x94\xe0\xb9\x89';B.disabled=false;}}"
"</script></body></html>";

esp_err_t bridge_vl53_html_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, VL53_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t bridge_vl53_json_handler(httpd_req_t *req)
{
    int mm[BRIDGE_VL53_NUM_SENSORS];
    uint32_t ts = bridge_vl53_get(mm);
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t age = (ts == 0) ? 0 : (now_ms - ts);
    uint32_t remaining = bridge_vl53_remaining_ms();

    char body[200];
    int n = snprintf(body, sizeof(body),
                     "{\"ts_ms\":%lu,\"age_ms\":%lu,\"remaining_ms\":%lu,"
                     "\"mm\":[%d,%d,%d,%d,%d,%d]}",
                     (unsigned long)ts, (unsigned long)age,
                     (unsigned long)remaining,
                     mm[0], mm[1], mm[2], mm[3], mm[4], mm[5]);
    if (n < 0 || n >= (int)sizeof(body)) n = (int)sizeof(body) - 1;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

esp_err_t bridge_vl53_read_handler(httpd_req_t *req)
{
    bridge_vl53_arm_read();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "{\"ok\":true,\"window_ms\":5000}",
                           HTTPD_RESP_USE_STRLEN);
}
