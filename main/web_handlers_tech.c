// web_handlers_tech.c — /tech single-page dashboard with tabs for all tech functions.

#include "web_handlers_tech.h"
#include "web_handlers_status.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "web_tech";

static const char TECH_PAGE[] =
"<!doctype html>"
"<html lang='th'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Technician Dashboard</title>"
"<style>"
"*{box-sizing:border-box}"
"body{margin:0;min-height:100vh;background:radial-gradient(circle at top,#16345a 0%,#0a1425 42%,#050b14 100%);font-family:'Segoe UI',Tahoma,sans-serif;color:#f4f8ff}"
".topbar{position:sticky;top:0;z-index:10;display:flex;align-items:center;gap:14px;padding:14px 22px;background:linear-gradient(180deg,rgba(8,18,32,.96),rgba(8,18,32,.82));border-bottom:1px solid rgba(255,255,255,.07);backdrop-filter:blur(6px)}"
".brand{display:flex;align-items:center;gap:10px;font-weight:800;font-size:18px;letter-spacing:.02em}"
".brand .dot{width:12px;height:12px;border-radius:50%;background:linear-gradient(135deg,#4dd7b0,#2fc4d5);box-shadow:0 0 12px rgba(77,215,176,.6)}"
".role{display:inline-flex;padding:5px 12px;border-radius:999px;background:rgba(77,215,176,.14);color:#9ae8d0;font-size:12px;font-weight:700;letter-spacing:.06em;text-transform:uppercase}"
".spacer{flex:1}"
".logout{color:#a7bdd7;text-decoration:none;font-size:14px;padding:8px 14px;border-radius:10px;border:1px solid rgba(255,255,255,.08)}"
".logout:hover{background:rgba(255,255,255,.05);color:#fff}"
".tabs{display:flex;flex-wrap:wrap;gap:6px;padding:10px 22px;background:rgba(8,18,32,.55);border-bottom:1px solid rgba(255,255,255,.06)}"
".tab{display:inline-flex;align-items:center;gap:8px;padding:10px 16px;border-radius:12px;background:transparent;color:#c7d6ee;font-size:14px;font-weight:600;cursor:pointer;border:1px solid transparent;user-select:none}"
".tab:hover{background:rgba(255,255,255,.05);color:#fff}"
".tab.active{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032;border-color:transparent;box-shadow:0 6px 16px rgba(77,215,176,.22)}"
".panel{display:none;padding:18px 22px}"
".panel.active{display:block}"
".panel h2{margin:4px 0 14px;font-size:22px}"
".panel .sub{color:#a7bdd7;font-size:14px;margin-bottom:14px;line-height:1.65}"
".frame{width:100%;min-height:calc(100vh - 180px);border:1px solid rgba(255,255,255,.08);border-radius:16px;background:#06101e;overflow:hidden}"
".frame iframe{display:block;width:100%;height:calc(100vh - 180px);border:0;background:#06101e}"
".placeholder{padding:42px;border-radius:18px;border:1px dashed rgba(255,255,255,.18);background:rgba(255,255,255,.03);text-align:center;color:#a7bdd7}"
".placeholder h3{margin:0 0 8px;color:#f4f8ff;font-size:20px}"
".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;margin-top:8px}"
".card{padding:16px 18px;border-radius:18px;background:linear-gradient(180deg,rgba(17,36,61,.95),rgba(8,18,32,.95));border:1px solid rgba(255,255,255,.08)}"
".card .k{color:#9ae8d0;font-size:12px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;margin-bottom:6px}"
".card .v{font-size:22px;font-weight:800;letter-spacing:.01em}"
".card .hint{color:#a7bdd7;font-size:12px;margin-top:4px}"
".row{display:flex;flex-wrap:wrap;gap:10px;margin-top:16px}"
".btn{display:inline-flex;align-items:center;justify-content:center;min-height:44px;padding:0 16px;border-radius:12px;border:none;font-size:14px;font-weight:700;text-decoration:none;cursor:pointer}"
".btn.primary{background:linear-gradient(135deg,#4dd7b0,#2fc4d5);color:#042032}"
".btn.warn{background:linear-gradient(135deg,#ff8a80,#ff4f5b);color:#2a0812}"
".btn.ghost{background:rgba(255,255,255,.06);color:#e4efff;border:1px solid rgba(255,255,255,.1)}"
".log-box{white-space:pre-wrap;background:#03080f;border:1px solid rgba(255,255,255,.08);border-radius:14px;padding:14px;height:calc(100vh - 240px);overflow:auto;font-family:Consolas,Monaco,monospace;font-size:12.5px;color:#c8e1ff}"
"@media (max-width:680px){.topbar{padding:10px 14px}.tabs{padding:8px 14px}.panel{padding:14px}}"
"</style></head><body>"

"<div class='topbar'>"
"<div class='brand'><span class='dot'></span> Unified Cam &middot; Tech Dashboard</div>"
"<div class='role'>Technician</div>"
"<div class='spacer'></div>"
"<a class='logout' href='/cloud/logout'>Logout</a>"
"</div>"

"<div class='tabs' id='tabs'>"
"<div class='tab active' data-tab='dashboard'>Dashboard</div>"
"<div class='tab' data-tab='monitor'>Monitor</div>"
"<div class='tab' data-tab='camera'>Camera</div>"
"<div class='tab' data-tab='dispenser'>Dispenser</div>"
"<div class='tab' data-tab='calibrate'>Calibrate</div>"
"<div class='tab' data-tab='cloud'>Cloud</div>"
"<div class='tab' data-tab='wifi'>WiFi</div>"
"<div class='tab' data-tab='logs'>Logs</div>"
"<div class='tab' data-tab='system'>System</div>"
"</div>"

"<div class='panel active' data-panel='dashboard'>"
"<h2>Dashboard</h2>"
"<div class='sub'>ภาพรวมอุปกรณ์ — uptime, หน่วยความจำ, เครือข่าย, เวอร์ชั่นเฟิร์มแวร์</div>"
"<div class='cards'>"
"<div class='card'><div class='k'>Uptime</div><div class='v' id='db-uptime'>&mdash;</div></div>"
"<div class='card'><div class='k'>Heap Free</div><div class='v' id='db-heap'>&mdash;</div><div class='hint' id='db-heap-min'></div></div>"
"<div class='card'><div class='k'>IP Address</div><div class='v' id='db-ip'>&mdash;</div><div class='hint' id='db-ssid'></div></div>"
"<div class='card'><div class='k'>RSSI</div><div class='v' id='db-rssi'>&mdash;</div></div>"
"<div class='card'><div class='k'>Boot Count</div><div class='v' id='db-boot'>&mdash;</div><div class='hint' id='db-reason'></div></div>"
"<div class='card'><div class='k'>RTC Time</div><div class='v' id='db-time'>&mdash;</div></div>"
"</div>"
"</div>"

"<div class='panel' data-panel='monitor'>"
"<h2>Monitor &middot; Live Sensors</h2>"
"<div class='sub'>VL53L0X x6 — ระยะวัดและจำนวนยาเม็ดในแต่ละช่อง (refresh ทุก 2 วินาที)</div>"
"<div class='frame'><iframe data-src='/sensors' src='about:blank'></iframe></div>"
"</div>"

"<div class='panel' data-panel='camera'>"
"<h2>Camera &middot; Live Stream</h2>"
"<div class='sub'>MJPEG stream (OV5647 &middot; 800x640 &middot; 50 fps) &middot; กด Start เพื่อเริ่มสตรีม</div>"
"<div class='row'>"
"<button class='btn primary' id='cam-start'>Start Stream</button>"
"<button class='btn ghost' id='cam-stop'>Stop</button>"
"<a class='btn ghost' id='cam-snap' target='_blank'>Open Snapshot</a>"
"</div>"
"<div class='frame' style='margin-top:14px;display:flex;align-items:center;justify-content:center;min-height:360px'>"
"<img id='cam-stream' alt='Stream idle' style='max-width:100%;max-height:calc(100vh - 240px);border-radius:12px;background:#03080f' />"
"</div>"
"</div>"

"<div class='panel' data-panel='dispenser'>"
"<h2>Dispenser &middot; Servo Test</h2>"
"<div class='sub'>ทดสอบเซอร์โวแต่ละช่อง &middot; การตั้งชื่อยา/ตารางเวลาทำที่จอสัมผัสของเครื่อง</div>"
"<div class='cards' id='servo-cards'></div>"
"<div class='row' style='margin-top:16px'>"
"<button class='btn primary' id='servo-home-all'>Home All</button>"
"<button class='btn ghost' id='servo-test-all'>Test All</button>"
"</div>"
"<div class='placeholder' style='margin-top:18px'><p>Home = กลับตำแหน่งเริ่มต้น &middot; Work = หมุนจ่ายยา 1 ครั้ง &middot; Test = หมุนไป-กลับเพื่อเช็คการทำงาน</p></div>"
"</div>"

"<div class='panel' data-panel='calibrate'>"
"<h2>Calibrate &middot; VL53L0X</h2>"
"<div class='sub'>ตั้งค่าคาลิเบรตต่อช่อง — full_dist, pill_height, max_pills, offset (จะทำใน Phase 2)</div>"
"<div class='placeholder'><h3>Coming in Phase 2</h3><p>หน้าคาลิเบรตพร้อม auto-calibrate wizard ต่อช่อง 6 ชุด — เก็บใน NVS</p></div>"
"</div>"

"<div class='panel' data-panel='cloud'>"
"<h2>Cloud &middot; Telegram / Google Sheets / NETPIE</h2>"
"<div class='sub'>ตั้งค่า Bot Token, Chat ID, Google Apps Script URL</div>"
"<div class='frame'><iframe data-src='/cloud' src='about:blank'></iframe></div>"
"</div>"

"<div class='panel' data-panel='wifi'>"
"<h2>WiFi Setup</h2>"
"<div class='sub'>เลือก SSID และบันทึก — บอร์ดจะรีสตาร์ทให้อัตโนมัติ</div>"
"<div class='frame'><iframe data-src='/wifi' src='about:blank'></iframe></div>"
"</div>"

"<div class='panel' data-panel='logs'>"
"<h2>Logs &middot; System Output</h2>"
"<div class='sub'>ring buffer ล่าสุด refresh ทุก 3 วินาที</div>"
"<div class='row'><button class='btn ghost' id='log-refresh'>Refresh now</button><button class='btn ghost' id='log-toggle'>Pause auto-refresh</button></div>"
"<pre class='log-box' id='log-box'>Loading...</pre>"
"</div>"

"<div class='panel' data-panel='system'>"
"<h2>System</h2>"
"<div class='sub'>รีบูตและข้อมูลเฟิร์มแวร์</div>"
"<div class='cards'>"
"<div class='card'><div class='k'>Firmware</div><div class='v'>unified_cam</div><div class='hint'>ESP32-P4-NANO &middot; ESP-IDF v5.3.2</div></div>"
"<div class='card'><div class='k'>IDF Version</div><div class='v' id='sys-idf'>&mdash;</div></div>"
"</div>"
"<div class='row' style='margin-top:22px'>"
"<button class='btn warn' id='sys-reboot'>Reboot Device</button>"
"<a class='btn ghost' href='/' target='_blank'>Open Main Page</a>"
"</div>"
"<p class='sub' style='margin-top:18px'>Reboot จะตัดการเชื่อมต่อชั่วคราว ~15 วินาที</p>"
"</div>"

"<script>"
"const tabs=document.querySelectorAll('.tab');"
"const panels=document.querySelectorAll('.panel');"
"function activate(name){"
"  tabs.forEach(t=>t.classList.toggle('active',t.dataset.tab===name));"
"  panels.forEach(p=>{"
"    const on=p.dataset.panel===name;"
"    p.classList.toggle('active',on);"
"    if(on){const f=p.querySelector('iframe[data-src]');if(f&&f.src==='about:blank')f.src=f.dataset.src;}"
"  });"
"  try{localStorage.setItem('techTab',name);history.replaceState(null,'','#'+name);}catch(e){}"
"}"
"tabs.forEach(t=>t.addEventListener('click',()=>activate(t.dataset.tab)));"
"(function(){const h=location.hash.slice(1)||localStorage.getItem('techTab')||'dashboard';if(document.querySelector('.tab[data-tab=\"'+h+'\"]'))activate(h);})();"

"function fmtUp(s){if(s==null)return '-';const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),ss=s%60;if(d)return d+'d '+h+'h';if(h)return h+'h '+m+'m';if(m)return m+'m '+ss+'s';return ss+'s';}"
"function fmtKB(b){return b==null?'-':(b/1024).toFixed(1)+' KB';}"
"function tabActive(name){const p=document.querySelector('[data-panel=\"'+name+'\"]');return p&&p.classList.contains('active')&&document.visibilityState!=='hidden';}"
"let dashBusy=false,dashTimer=null;"
"async function refreshDash(){if(dashBusy||!tabActive('dashboard'))return;dashBusy=true;"
"  try{const r=await fetch('/status.json',{cache:'no-store'});if(r.ok){const j=await r.json();"
"    document.getElementById('db-uptime').textContent=fmtUp(j.uptime||j.uptime_s);"
"    document.getElementById('db-heap').textContent=fmtKB(j.heap_free||j.heap);"
"    if(j.heap_min)document.getElementById('db-heap-min').textContent='min '+fmtKB(j.heap_min);"
"    document.getElementById('db-ip').textContent=j.ip||'-';"
"    if(j.ssid)document.getElementById('db-ssid').textContent=j.ssid;"
"    document.getElementById('db-rssi').textContent=(j.rssi!=null?j.rssi+' dBm':'-');"
"    document.getElementById('db-boot').textContent=j.boot_count||'-';"
"    if(j.reset_reason)document.getElementById('db-reason').textContent=j.reset_reason;"
"    document.getElementById('db-time').textContent=j.time||j.rtc_time||'-';"
"    if(j.idf_version)document.getElementById('sys-idf').textContent=j.idf_version;"
"  }}catch(e){}finally{dashBusy=false;}"
"}"

"let logPaused=false,logTimer=null,logBusy=false;"
"async function refreshLogs(){if(logBusy||!tabActive('logs'))return;logBusy=true;try{const r=await fetch('/logs/tail',{cache:'no-store'});if(r.ok){const t=await r.text();const el=document.getElementById('log-box');el.textContent=t;el.scrollTop=el.scrollHeight;}}catch(e){}finally{logBusy=false;}}"

"function tickPolls(){if(tabActive('dashboard'))refreshDash();if(tabActive('logs')&&!logPaused)refreshLogs();}"
"setInterval(tickPolls,5000);tickPolls();"
"document.addEventListener('visibilitychange',()=>{if(document.visibilityState==='visible')tickPolls();});"
"document.getElementById('log-refresh').addEventListener('click',refreshLogs);"
"document.getElementById('log-toggle').addEventListener('click',e=>{logPaused=!logPaused;e.target.textContent=logPaused?'Resume auto-refresh':'Pause auto-refresh';if(!logPaused)refreshLogs();});"
"tabs.forEach(t=>t.addEventListener('click',()=>setTimeout(tickPolls,100)));"

"document.getElementById('sys-reboot').addEventListener('click',async()=>{"
"  if(!confirm('Reboot device now?'))return;"
"  try{await fetch('/tech/reboot',{method:'POST'});}catch(e){}"
"  alert('Rebooting... reload in ~15 seconds');"
"});"

"const camImg=document.getElementById('cam-stream');"
"const streamBase=location.protocol+'//'+location.hostname+':81';"
"document.getElementById('cam-snap').href=streamBase+'/capture';"
"function camStart(){camImg.alt='Loading stream...';camImg.src=streamBase+'/stream?t='+Date.now();}"
"function camStop(){if(camImg.src)camImg.src='';camImg.alt='Stream stopped';}"
"document.getElementById('cam-start').addEventListener('click',camStart);"
"document.getElementById('cam-stop').addEventListener('click',camStop);"
"(new MutationObserver(()=>{const p=document.querySelector('[data-panel=camera]');if(p&&!p.classList.contains('active'))camStop();})).observe(document.querySelector('[data-panel=camera]'),{attributes:true,attributeFilter:['class']});"
"document.addEventListener('visibilitychange',()=>{if(document.visibilityState==='hidden')camStop();});"
"window.addEventListener('pagehide',camStop);"

"function servoAction(ch,act){return fetch('/servo/'+act+'?ch='+ch,{cache:'no-store'}).then(r=>r.text()).catch(()=>null);}"
"function renderServos(){const wrap=document.getElementById('servo-cards');let h='';for(let i=0;i<6;i++){h+=\"<div class='card'><div class='k'>Servo Ch\"+i+\"</div><div class='row' style='margin-top:10px;gap:6px'><button class='btn ghost' data-ch='\"+i+\"' data-act='home'>Home</button><button class='btn primary' data-ch='\"+i+\"' data-act='work'>Work</button><button class='btn ghost' data-ch='\"+i+\"' data-act='test'>Test</button></div></div>\";}wrap.innerHTML=h;wrap.querySelectorAll('button[data-ch]').forEach(b=>b.addEventListener('click',()=>servoAction(b.dataset.ch,b.dataset.act)));}"
"renderServos();"
"document.getElementById('servo-home-all').addEventListener('click',async()=>{for(let i=0;i<6;i++)await servoAction(i,'home');});"
"document.getElementById('servo-test-all').addEventListener('click',async()=>{for(let i=0;i<6;i++){await servoAction(i,'test');await new Promise(r=>setTimeout(r,400));}});"
"</script>"
"</body></html>";

esp_err_t tech_dashboard_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_page_auth(req);
    if (auth != ESP_OK) return auth;

    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, TECH_PAGE, HTTPD_RESP_USE_STRLEN);
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "Reboot requested from /tech dashboard");
    esp_restart();
}

esp_err_t tech_reboot_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"rebooting\"}");

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}
