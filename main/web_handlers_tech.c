// web_handlers_tech.c — /tech single-page dashboard with tabs for all tech functions.

#include "web_handlers_tech.h"
#include "web_handlers_status.h"
#include "dispenser_scheduler.h"
#include "netpie_mqtt.h"
#include "module_map.h"
#include "config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "web_tech";

static const char TECH_PAGE[] =
"<!doctype html>"
"<html lang='th'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>เครื่องจ่ายยาอัตโนมัติ · แผงควบคุมช่าง</title>"
"<style>"
"*{box-sizing:border-box}"
":root{--bg-deep:#050b14;--bg-card:#0e1a2e;--bg-card-2:#152844;--border:rgba(255,255,255,.08);--accent:#4dd7b0;--accent-2:#2fc4d5;--warn:#ffd166;--bad:#ff8a80;--text:#f4f8ff;--text-dim:#a7bdd7;--text-mute:#5a6b80}"
"body{margin:0;min-height:100vh;background:radial-gradient(circle at top,#16345a 0%,#0a1425 42%,#050b14 100%);font-family:'Segoe UI',Tahoma,sans-serif;color:var(--text);-webkit-font-smoothing:antialiased}"
".topbar{position:sticky;top:0;z-index:10;display:flex;align-items:center;gap:14px;padding:14px 22px;background:linear-gradient(180deg,rgba(8,18,32,.96),rgba(8,18,32,.82));border-bottom:1px solid var(--border);backdrop-filter:blur(8px)}"
".brand{display:flex;align-items:center;gap:10px;font-weight:800;font-size:18px;letter-spacing:.02em}"
".brand .dot{width:12px;height:12px;border-radius:50%;background:linear-gradient(135deg,var(--accent),var(--accent-2));box-shadow:0 0 16px rgba(77,215,176,.55)}"
".role{display:inline-flex;padding:5px 12px;border-radius:999px;background:rgba(77,215,176,.14);color:#9ae8d0;font-size:12px;font-weight:700;letter-spacing:.06em;text-transform:uppercase}"
".spacer{flex:1}"
".logout{color:var(--text-dim);text-decoration:none;font-size:14px;padding:8px 14px;border-radius:10px;border:1px solid var(--border);transition:.15s}"
".logout:hover{background:rgba(255,255,255,.05);color:#fff}"
".tabs{display:flex;flex-wrap:wrap;gap:6px;padding:10px 22px;background:rgba(8,18,32,.55);border-bottom:1px solid rgba(255,255,255,.06);overflow-x:auto}"
".tab{display:inline-flex;align-items:center;gap:8px;padding:10px 16px;border-radius:12px;background:transparent;color:#c7d6ee;font-size:14px;font-weight:600;cursor:pointer;border:1px solid transparent;user-select:none;white-space:nowrap;transition:.15s}"
".tab:hover{background:rgba(255,255,255,.05);color:#fff}"
".tab.active{background:linear-gradient(135deg,var(--accent),var(--accent-2));color:#042032;border-color:transparent;box-shadow:0 8px 22px rgba(77,215,176,.28)}"
".panel{display:none;padding:22px 24px;max-width:1280px;margin:0 auto}"
".panel.active{display:block;animation:fadeIn .2s ease}"
"@keyframes fadeIn{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}"
".panel h2{margin:4px 0 12px;font-size:22px;font-weight:800;letter-spacing:.01em}"
".panel h3{margin:24px 0 10px;font-size:16px;font-weight:700;color:#dbe9ff;text-transform:uppercase;letter-spacing:.08em}"
".panel .sub{color:var(--text-dim);font-size:14px;margin-bottom:16px;line-height:1.6}"
".section{margin-top:22px;padding-top:22px;border-top:1px solid var(--border)}"
".frame{width:100%;min-height:calc(100vh - 200px);border:1px solid var(--border);border-radius:16px;background:#06101e;overflow:hidden}"
".frame iframe{display:block;width:100%;height:calc(100vh - 200px);border:0;background:#06101e}"
".placeholder{padding:42px;border-radius:18px;border:1px dashed rgba(255,255,255,.18);background:rgba(255,255,255,.03);text-align:center;color:var(--text-dim)}"
".placeholder h3{margin:0 0 8px;color:var(--text);font-size:20px;text-transform:none;letter-spacing:0}"
".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;margin-top:8px}"
".card{padding:16px 18px;border-radius:18px;background:linear-gradient(180deg,rgba(17,36,61,.95),rgba(8,18,32,.95));border:1px solid var(--border);transition:.18s}"
".card:hover{border-color:rgba(77,215,176,.25);box-shadow:0 6px 20px rgba(0,0,0,.25)}"
".card .k{color:#9ae8d0;font-size:11px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;margin-bottom:6px}"
".card .v{font-size:22px;font-weight:800;letter-spacing:.01em}"
".card .hint{color:var(--text-dim);font-size:12px;margin-top:4px}"
".pill{display:inline-flex;align-items:center;gap:6px;padding:3px 10px;border-radius:999px;background:rgba(77,215,176,.12);color:#9ae8d0;font-size:11px;font-weight:700;letter-spacing:.04em}"
".pill.bad{background:rgba(255,138,128,.16);color:var(--bad)}"
".pill.warn{background:rgba(255,209,102,.16);color:var(--warn)}"
".row{display:flex;flex-wrap:wrap;gap:10px;margin-top:16px}"
".btn{display:inline-flex;align-items:center;justify-content:center;min-height:42px;padding:0 18px;border-radius:11px;border:none;font-size:14px;font-weight:700;text-decoration:none;cursor:pointer;transition:.15s;font-family:inherit}"
".btn:hover{transform:translateY(-1px);box-shadow:0 6px 16px rgba(0,0,0,.22)}"
".btn:active{transform:translateY(0)}"
".btn:disabled{opacity:.5;cursor:not-allowed;transform:none;box-shadow:none}"
".btn.primary{background:linear-gradient(135deg,var(--accent),var(--accent-2));color:#042032}"
".btn.warn{background:linear-gradient(135deg,#ff8a80,#ff4f5b);color:#2a0812}"
".btn.ghost{background:rgba(255,255,255,.06);color:#e4efff;border:1px solid rgba(255,255,255,.1)}"
".btn.ghost:hover{background:rgba(255,255,255,.1)}"
".btn.sm{min-height:34px;padding:0 12px;font-size:13px}"
".inp,input[type=text],input[type=password],input[type=number],input[type=time],select,textarea{width:100%;padding:9px 12px;border-radius:10px;border:1px solid rgba(255,255,255,.12);background:#06101e;color:var(--text);font-size:14px;font-family:inherit;outline:none;transition:.15s}"
".inp:focus,input:focus,select:focus,textarea:focus{border-color:var(--accent);box-shadow:0 0 0 3px rgba(77,215,176,.18)}"
".inp::placeholder,input::placeholder{color:var(--text-mute)}"
".lbl{display:block;font-size:12px;color:var(--text-dim);font-weight:600;margin-bottom:6px;letter-spacing:.02em}"
".log-box{white-space:pre-wrap;background:#03080f;border:1px solid var(--border);border-radius:14px;padding:14px;height:calc(100vh - 240px);overflow:auto;font-family:Consolas,Monaco,monospace;font-size:12.5px;color:#c8e1ff}"
".state-line{font-size:13px;color:var(--text-dim);margin-top:8px;min-height:18px}"
"@media (max-width:680px){.topbar{padding:10px 14px}.tabs{padding:8px 14px}.panel{padding:16px 14px}.panel h2{font-size:20px}.cards{gap:10px}.card{padding:14px}}"
"</style></head><body>"

"<div class='topbar'>"
"<div class='brand'><span class='dot'></span> เครื่องจ่ายยาอัตโนมัติ &middot; แผงควบคุมช่าง</div>"
"<div class='role'>ช่าง</div>"
"<div class='spacer'></div>"
"<a class='logout' href='/cloud/logout'>ออกจากระบบ</a>"
"</div>"

"<div class='tabs' id='tabs'>"
"<div class='tab active' data-tab='dashboard'>ภาพรวม</div>"
"<div class='tab' data-tab='camera'>กล้อง</div>"
"<div class='tab' data-tab='dispenser'>เซอร์โว</div>"
"<div class='tab' data-tab='sound'>เสียง</div>"
"<div class='tab' data-tab='audit'>ประวัติยา</div>"
"<div class='tab' data-tab='cloud'>Telegram</div>"
"<div class='tab' data-tab='wifi'>เครือข่าย</div>"
"<div class='tab' data-tab='logs'>บันทึก</div>"
"<div class='tab' data-tab='system'>ระบบ</div>"
"</div>"

"<div class='panel active' data-panel='dashboard'>"
"<h2>ภาพรวมระบบ</h2>"
"<div class='sub'>สถานะอุปกรณ์ &middot; ระยะเวลาทำงาน &middot; หน่วยความจำ &middot; เครือข่าย &middot; เฟิร์มแวร์</div>"
"<div class='cards'>"
"<div class='card'><div class='k'>ระยะเวลาทำงาน</div><div class='v' id='db-uptime'>&mdash;</div><div class='hint' id='db-conn'>กำลังเชื่อมต่อ…</div></div>"
"<div class='card'><div class='k'>หน่วยความจำว่าง</div><div class='v' id='db-heap'>&mdash;</div><div class='hint' id='db-heap-min'></div></div>"
"<div class='card'><div class='k'>หมายเลข IP</div><div class='v' id='db-ip'>&mdash;</div><div class='hint' id='db-ssid'></div></div>"
"<div class='card'><div class='k'>ความแรงสัญญาณ</div><div class='v' id='db-rssi'>&mdash;</div></div>"
"<div class='card'><div class='k'>จำนวนการบูต</div><div class='v' id='db-boot'>&mdash;</div><div class='hint' id='db-reason'></div></div>"
"<div class='card'><div class='k'>เวลานาฬิกา</div><div class='v' id='db-time'>&mdash;</div></div>"
"</div>"
"<div class='section'>"
"<h3>เซ็นเซอร์ตรวจจับยา</h3>"
"<div class='sub'>สถานะ IR 6 โมดูล &middot; <span class='pill bad'>ON</span> = มียา/วัตถุบังลำแสง &middot; <span class='pill'>OFF</span> = ลำแสงโล่ง</div>"
"<div class='cards' style='grid-template-columns:repeat(auto-fit,minmax(120px,1fr))'>"
"<div class='card' style='text-align:center'><div class='k'>โมดูล 1</div><div class='v' id='ir-p0' style='font-size:18px'>&mdash;</div></div>"
"<div class='card' style='text-align:center'><div class='k'>โมดูล 2</div><div class='v' id='ir-p1' style='font-size:18px'>&mdash;</div></div>"
"<div class='card' style='text-align:center'><div class='k'>โมดูล 3</div><div class='v' id='ir-p2' style='font-size:18px'>&mdash;</div></div>"
"<div class='card' style='text-align:center'><div class='k'>โมดูล 4</div><div class='v' id='ir-p3' style='font-size:18px'>&mdash;</div></div>"
"<div class='card' style='text-align:center'><div class='k'>โมดูล 5</div><div class='v' id='ir-p4' style='font-size:18px'>&mdash;</div></div>"
"<div class='card' style='text-align:center'><div class='k'>โมดูล 6</div><div class='v' id='ir-p5' style='font-size:18px'>&mdash;</div></div>"
"</div>"
"</div>"
"</div>"

"<div class='panel' data-panel='camera'>"
"<h2>ภาพถ่ายทอดสด</h2>"
"<div class='sub'>MJPEG stream &middot; OV5647 &middot; 800&times;640 &middot; 50 fps &middot; ลด JPEG Quality เพื่อให้ภาพลื่นขึ้น</div>"
"<div class='row'>"
"<button class='btn primary' id='cam-start'>▶ เริ่มสตรีม</button>"
"<button class='btn ghost' id='cam-stop'>หยุด</button>"
"<a class='btn ghost' id='cam-snap' target='_blank'>เปิดภาพนิ่ง</a>"
"</div>"
"<div class='row' style='align-items:center;margin-top:10px'>"
"<label style='display:flex;align-items:center;gap:10px;color:var(--text-dim);font-size:13px'>JPEG Quality"
"<input id='cam-q' type='range' min='20' max='60' step='5' value='30' style='width:200px'>"
"<span id='cam-q-val' style='color:var(--accent);font-weight:800;min-width:36px;text-align:right'>30</span>"
"</label>"
"<button class='btn ghost sm' id='cam-q-apply'>นำไปใช้</button>"
"<span class='state-line'>เกิน 50 ลิงก์ Wi-Fi อาจรับไม่ทัน → ภาพเฟรมขาด</span>"
"</div>"

"<div class='row' style='align-items:center;margin-top:14px;flex-wrap:wrap;gap:16px'>"
"<label style='display:flex;align-items:center;gap:10px;color:var(--text-dim);font-size:13px'>ความสว่าง"
"<input id='cam-bri' type='range' min='-3' max='3' step='1' value='0' style='width:160px'>"
"<span id='cam-bri-val' style='color:var(--accent);font-weight:800;min-width:28px;text-align:right'>0</span>"
"</label>"
"<label style='display:flex;align-items:center;gap:10px;color:var(--text-dim);font-size:13px'>ความเปรียบต่าง"
"<input id='cam-con' type='range' min='-3' max='3' step='1' value='0' style='width:160px'>"
"<span id='cam-con-val' style='color:var(--accent);font-weight:800;min-width:28px;text-align:right'>0</span>"
"</label>"
"<label style='display:flex;align-items:center;gap:10px;color:var(--text-dim);font-size:13px'>อิ่มสี"
"<input id='cam-sat' type='range' min='-3' max='3' step='1' value='0' style='width:160px'>"
"<span id='cam-sat-val' style='color:var(--accent);font-weight:800;min-width:28px;text-align:right'>0</span>"
"</label>"
"<button class='btn ghost sm' id='cam-img-apply'>ปรับภาพ</button>"
"</div>"
"<div class='frame' style='margin-top:14px;display:flex;align-items:center;justify-content:center;min-height:360px'>"
"<img id='cam-stream' alt='ยังไม่ได้สตรีม' style='max-width:100%;max-height:calc(100vh - 260px);border-radius:12px;background:#03080f' />"
"</div>"
"</div>"

"<div class='panel' data-panel='dispenser'>"
"<h2>ทดสอบเซอร์โว &amp; ตั้งค่ามุม</h2>"
"<div class='sub'>ตั้งค่ามุมตำแหน่งเริ่มต้น/จ่ายยาต่อช่อง + ทดสอบ &middot; บันทึกลง NVS อัตโนมัติ</div>"
"<div class='cards' id='servo-cards'></div>"
"<div class='row'>"
"<button class='btn primary' id='servo-home-all'>↩ กลับตำแหน่งเริ่มต้นทุกช่อง</button>"
"<button class='btn ghost' id='servo-test-all'>ทดสอบทุกช่อง</button>"
"<button class='btn ghost' id='servo-refresh'>รีเฟรชสถานะ</button>"
"</div>"
"<div class='placeholder' style='margin-top:18px'><p>ตำแหน่งเริ่มต้น/จ่ายยา = มุม (0–180&deg;) &middot; <b>บันทึก</b> = save NVS &middot; <b>ทดสอบ</b> = หมุนไป-กลับเพื่อตรวจสอบ</p></div>"
"</div>"


"<div class='panel' data-panel='sound'>"
"<h2>เสียง &amp; การแจ้งเตือน</h2>"
"<div class='sub'>เลือกหมายเลขแทร็กของเสียงเตือน เสียงปุ่ม เสียงรายงาน &middot; กดปุ่ม ▶ เพื่อทดสอบ &middot; บันทึกลง NVS</div>"
"<div class='cards' id='sound-cards' style='grid-template-columns:repeat(auto-fit,minmax(260px,1fr))'></div>"
"<div class='row'>"
"<button class='btn primary' id='snd-save'>💾 บันทึกการตั้งค่าเสียง</button>"
"<button class='btn ghost' id='snd-reload'>รีโหลดค่าจากเครื่อง</button>"
"<span id='snd-state' class='state-line' style='margin-top:0'></span>"
"</div>"
"</div>"

"<div class='panel' data-panel='audit'>"
"<h2>ประวัติการจ่ายยา</h2>"
"<div class='sub'>32 รายการล่าสุด &middot; <span class='pill'>M = Manual</span> <span class='pill'>S = Schedule</span> <span class='pill'>V = Sensor</span> <span class='pill'>W = Web</span></div>"
"<div class='row'><button class='btn ghost' id='audit-refresh'>↻ รีเฟรช</button>"
"<span id='audit-state' class='state-line' style='margin-top:0'></span></div>"
"<div id='audit-list' style='margin-top:14px;display:flex;flex-direction:column;gap:8px;max-height:calc(100vh - 320px);overflow-y:auto'>"
"<div style='color:var(--text-dim);padding:18px;text-align:center'>กดรีเฟรชเพื่อโหลดประวัติ</div>"
"</div>"
"</div>"

"<div class='panel' data-panel='cloud'>"
"<h2>การเชื่อมต่อ Telegram Bot</h2>"
"<div class='sub'>ตั้งค่า Bot Token + Chat ID &middot; ใช้สำหรับส่งการแจ้งเตือนและภาพถ่ายไปยังผู้ดูแล</div>"
"<div class='frame'><iframe data-src='/cloud' src='about:blank'></iframe></div>"
"</div>"

"<div class='panel' data-panel='wifi'>"
"<h2>ตั้งค่าเครือข่าย Wi-Fi</h2>"
"<div class='sub'>กดสแกนเพื่อค้นหาเครือข่าย &middot; เลือกชื่อจากรายการ &middot; บันทึกแล้วบอร์ดจะรีสตาร์ทอัตโนมัติ</div>"
"<div class='cards' style='grid-template-columns:1fr'>"
"<div class='card'>"
"<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:12px'>"
"<b style='font-size:15px'>เครือข่ายที่พบ</b>"
"<button class='btn ghost sm' id='wifi-scan'>↻ สแกน Wi-Fi</button>"
"</div>"
"<div id='wifi-list' style='display:flex;flex-direction:column;gap:8px;max-height:300px;overflow-y:auto'>"
"<div style='color:var(--text-dim);font-size:13px;padding:18px;text-align:center'>กด \"สแกน Wi-Fi\" เพื่อเริ่มค้นหา</div>"
"</div>"
"</div>"
"<div class='card'>"
"<div class='k' style='margin-bottom:14px'>เชื่อมต่อเครือข่าย</div>"
"<label class='lbl'>SSID</label>"
"<input id='wifi-ssid' type='text' placeholder='เลือกจากรายการ หรือกรอกเอง'>"
"<label class='lbl' style='margin-top:14px'>รหัสผ่าน</label>"
"<input id='wifi-pass' type='password' placeholder='ใส่รหัสผ่าน Wi-Fi'>"
"<div class='row'>"
"<button class='btn primary' id='wifi-save'>💾 บันทึก &amp; รีสตาร์ท</button>"
"<button class='btn warn' id='wifi-forget'>🗑 ล้างค่า Wi-Fi</button>"
"<span id='wifi-state' class='state-line' style='margin-top:0'></span>"
"</div>"
"</div>"
"</div>"
"</div>"

"<div class='panel' data-panel='logs'>"
"<h2>บันทึกของระบบ</h2>"
"<div class='sub'>Ring buffer ล่าสุด &middot; รีเฟรชอัตโนมัติทุก 3 วินาที</div>"
"<div class='row'><button class='btn ghost' id='log-refresh'>รีเฟรชทันที</button><button class='btn ghost' id='log-toggle'>หยุดรีเฟรชอัตโนมัติ</button></div>"
"<pre class='log-box' id='log-box'>กำลังโหลด...</pre>"
"</div>"

"<div class='panel' data-panel='system'>"
"<h2>ระบบและเฟิร์มแวร์</h2>"
"<div class='sub'>ควบคุมการทำงาน &middot; รีบูต &middot; หยุดฉุกเฉิน &middot; ช่วงเวลาเงียบ</div>"
"<div class='cards'>"
"<div class='card'><div class='k'>เฟิร์มแวร์</div><div class='v'>เครื่องจ่ายยาอัตโนมัติ</div><div class='hint'>ESP32-P4-NANO &middot; ESP-IDF v5.3.2</div></div>"
"<div class='card'><div class='k'>IDF Version</div><div class='v' id='sys-idf'>&mdash;</div></div>"
"</div>"
"<div class='row'>"
"<button class='btn warn' id='sys-estop' style='font-size:15px;padding:0 22px'>🛑 หยุดฉุกเฉิน</button>"
"<button class='btn warn' id='sys-reboot'>↻ รีบูตอุปกรณ์</button>"
"<a class='btn ghost' href='/' target='_blank'>↗ เปิดหน้าหลัก</a>"
"<span id='sys-estop-state' style='align-self:center;font-weight:700'></span>"
"</div>"
"<p class='sub' style='margin-top:14px'>หยุดฉุกเฉิน = บล็อกการจ่ายยาทุกประเภท (manual + scheduled) จนกว่าจะกดอีกครั้งเพื่อยกเลิก สถานะคงอยู่หลังรีบูต &middot; การรีบูตจะตัดการเชื่อมต่อชั่วคราว ~15 วินาที</p>"

"<div class='section'>"
"<h3>ช่วงเวลาเงียบ (Quiet Hours)</h3>"
"<p class='sub'>กำหนดช่วงเวลาที่ห้ามจ่ายยาอัตโนมัติ &middot; ตัวอย่าง 22:00 → 06:00 = ช่วงนอน &middot; manual + Telegram /dispense ยังจ่ายได้ปกติ &middot; เคลียร์ทั้งคู่เป็น 00:00 = ปิดฟีเจอร์</p>"
"<div class='row' style='align-items:flex-end;gap:14px'>"
"<div style='flex:0 1 140px'><label class='lbl'>เริ่มเงียบ</label><input id='qh-start' type='time' value='00:00'></div>"
"<div style='flex:0 1 140px'><label class='lbl'>สิ้นสุด</label><input id='qh-end' type='time' value='00:00'></div>"
"<button class='btn primary' id='qh-save'>💾 บันทึก</button>"
"<button class='btn ghost' id='qh-disable'>ปิดใช้งาน</button>"
"<span id='qh-state' class='state-line' style='margin-top:0'></span>"
"</div>"
"</div>"

"<div class='section'>"
"<h3>จำนวนยาสูงสุดต่อโมดูล</h3>"
"<p class='sub'>ตั้งเพดานจำนวนเม็ดยาที่ใส่ได้ต่อโมดูล &middot; ใช้ทั้งบนจอสัมผัสและ NETPIE widget &middot; ค่าเริ่มต้น 16 เม็ด &middot; ค่าที่ตั้งจะถูกบันทึกใน NVS และซิงก์ผ่าน NETPIE shadow</p>"
"<div class='row' style='align-items:flex-end;gap:14px'>"
"<div style='flex:0 1 180px'>"
"<label class='lbl'>เม็ดสูงสุด (1–999)</label>"
"<input id='mp-input' type='number' min='1' max='999' value='16' inputmode='numeric'>"
"</div>"
"<button class='btn primary' id='mp-save'>💾 บันทึก</button>"
"<span id='mp-state' class='state-line' style='margin-top:0'></span>"
"</div>"
"</div>"

"<div class='section'>"
"<h3>สลับโมดูลจ่ายยา (Servo + IR)</h3>"
"<p class='sub'>เลือกว่าโมดูลทางตรรกะ (ยา 1–6 ในรายการ) จะใช้ชุดฮาร์ดแวร์ตัวไหนจริง &middot; เซอร์โว + เซ็นเซอร์ IR ของช่องเดียวกันจะถูกสลับไปด้วยกัน &middot; ใช้เมื่อช่องใดช่องหนึ่งเสียและต้องการย้ายไปใช้ฮาร์ดแวร์ของอีกช่องโดยไม่เดินสายใหม่ &middot; เป็นการแมปแบบ permutation — แต่ละช่องฮาร์ดแวร์ต้องถูกใช้ครั้งเดียว</p>"
"<div class='cards' id='mm-cards' style='grid-template-columns:repeat(auto-fit,minmax(220px,1fr))'>"
"<div class='card'><div class='k'>โมดูล 1 (ยา 1)</div><select id='mm-0' class='inp'></select></div>"
"<div class='card'><div class='k'>โมดูล 2 (ยา 2)</div><select id='mm-1' class='inp'></select></div>"
"<div class='card'><div class='k'>โมดูล 3 (ยา 3)</div><select id='mm-2' class='inp'></select></div>"
"<div class='card'><div class='k'>โมดูล 4 (ยา 4)</div><select id='mm-3' class='inp'></select></div>"
"<div class='card'><div class='k'>โมดูล 5 (ยา 5)</div><select id='mm-4' class='inp'></select></div>"
"<div class='card'><div class='k'>โมดูล 6 (ยา 6)</div><select id='mm-5' class='inp'></select></div>"
"</div>"
"<div class='row' style='align-items:flex-end;gap:14px;margin-top:14px'>"
"<button class='btn primary' id='mm-save'>💾 บันทึกการสลับ</button>"
"<button class='btn ghost'   id='mm-reset'>↩ รีเซ็ตเป็นค่าเดิม (1→1, 2→2, …)</button>"
"<span id='mm-state' class='state-line' style='margin-top:0'></span>"
"</div>"
"</div>"
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
/* Realtime clock state — last sync from server + locally-derived ticks. */
"let s_clock_baseSec=null,s_clock_baseAtMs=0,s_clock_uptimeBase=null,s_clock_uptimeAtMs=0;"
"function pad2(n){return n<10?'0'+n:String(n);}"
"function tickClock(){"
"  if(s_clock_baseSec!=null){"
"    const elapsed=Math.floor((Date.now()-s_clock_baseAtMs)/1000);"
"    let t=(s_clock_baseSec+elapsed)%86400;if(t<0)t+=86400;"
"    const h=Math.floor(t/3600),m=Math.floor(t%3600/60),s=t%60;"
"    const el=document.getElementById('db-time');if(el)el.textContent=pad2(h)+':'+pad2(m)+':'+pad2(s);"
"  }"
"  if(s_clock_uptimeBase!=null){"
"    const u=s_clock_uptimeBase+Math.floor((Date.now()-s_clock_uptimeAtMs)/1000);"
"    const el=document.getElementById('db-uptime');if(el)el.textContent=fmtUp(u);"
"  }"
"}"
"setInterval(tickClock,1000);"
"let dashBusy=false,dashTimer=null;"
"let s_lastOk=Date.now();"
"function fmtConn(ageMs){const s=Math.floor(ageMs/1000);"
"  if(s<10)return '🟢 ออนไลน์';"
"  if(s<60)return '🟡 ขาดการเชื่อมต่อ '+s+' วิ';"
"  const m=Math.floor(s/60);"
"  return '🔴 ออฟไลน์ '+m+' นาที';}"
"function paintConn(){const el=document.getElementById('db-conn');if(!el)return;"
"  const age=Date.now()-s_lastOk;el.textContent=fmtConn(age);"
"  el.style.color=age<10000?'#9ae8d0':age<60000?'#ffd166':'#ff8a80';}"
"setInterval(paintConn,1000);"
"async function refreshDash(){if(dashBusy||!tabActive('dashboard'))return;dashBusy=true;"
"  try{const r=await fetch('/status.json',{cache:'no-store'});if(r.ok){const j=await r.json();"
"    s_lastOk=Date.now();paintConn();"
"    if(j.uptime_s!=null||j.uptime!=null){s_clock_uptimeBase=j.uptime_s||j.uptime;s_clock_uptimeAtMs=Date.now();}"
"    document.getElementById('db-heap').textContent=fmtKB(j.heap_free||j.heap);"
"    if(j.heap_min)document.getElementById('db-heap-min').textContent='min '+fmtKB(j.heap_min);"
"    document.getElementById('db-ip').textContent=j.ip||'-';"
"    if(j.ssid)document.getElementById('db-ssid').textContent=j.ssid;"
"    document.getElementById('db-rssi').textContent=(j.rssi!=null?j.rssi+' dBm':'-');"
"    document.getElementById('db-boot').textContent=j.boot_count||'-';"
"    if(j.reset_reason)document.getElementById('db-reason').textContent=j.reset_reason;"
"    const tstr=j.time||j.rtc_time;"
"    if(tstr&&/^\\d{2}:\\d{2}:\\d{2}/.test(tstr)){const [hh,mm,ss]=tstr.split(':').map(Number);s_clock_baseSec=hh*3600+mm*60+ss;s_clock_baseAtMs=Date.now();}"
"    if(j.idf_version)document.getElementById('sys-idf').textContent=j.idf_version;"
"    setIrCell('ir-p0',j.ir_p0);setIrCell('ir-p1',j.ir_p1);setIrCell('ir-p2',j.ir_p2);"
"    setIrCell('ir-p3',j.ir_p3);setIrCell('ir-p4',j.ir_p4);setIrCell('ir-p5',j.ir_p5);"
"    tickClock();"
"  }}catch(e){}finally{dashBusy=false;}"
"}"
"function setIrCell(id,v){const el=document.getElementById(id);if(!el)return;"
"  if(v==null){el.textContent='--';el.style.color='';return;}"
"  const blocked=(v===1||v==='1'||v===true);"
"  el.textContent=blocked?'🔴 ON':'⚪ OFF';"
"  el.style.color=blocked?'#ff8a80':'#5a6b80';}"
"let logPaused=false,logTimer=null,logBusy=false;"
"async function refreshLogs(){if(logBusy||!tabActive('logs'))return;logBusy=true;try{const r=await fetch('/logs/tail',{cache:'no-store'});if(r.ok){const t=await r.text();const el=document.getElementById('log-box');el.textContent=t;el.scrollTop=el.scrollHeight;}}catch(e){}finally{logBusy=false;}}"

"function tickPolls(){if(tabActive('dashboard'))refreshDash();if(tabActive('logs')&&!logPaused)refreshLogs();}"
"setInterval(tickPolls,1500);tickPolls();"
"document.addEventListener('visibilitychange',()=>{if(document.visibilityState==='visible')tickPolls();});"
"document.getElementById('log-refresh').addEventListener('click',refreshLogs);"
"document.getElementById('log-toggle').addEventListener('click',e=>{logPaused=!logPaused;e.target.textContent=logPaused?'กลับมารีเฟรชอัตโนมัติ':'หยุดรีเฟรชอัตโนมัติ';if(!logPaused)refreshLogs();});"
"tabs.forEach(t=>t.addEventListener('click',()=>setTimeout(tickPolls,100)));"

"document.getElementById('sys-reboot').addEventListener('click',async()=>{"
"  if(!confirm('ยืนยันการรีบูตอุปกรณ์?'))return;"
"  try{await fetch('/tech/reboot',{method:'POST'});}catch(e){}"
"  alert('กำลังรีบูต... กรุณารอ ~15 วินาที แล้วโหลดหน้าใหม่');"
"});"
"function renderEstop(active){const b=document.getElementById('sys-estop');const s=document.getElementById('sys-estop-state');"
"  if(active){b.textContent='🟢 ยกเลิกหยุดฉุกเฉิน';b.style.background='linear-gradient(135deg,#22c55e,#16a34a)';b.style.color='#fff';s.textContent='🛑 ระบบหยุดอยู่';s.style.color='#ff8a80';}"
"  else{b.textContent='🛑 หยุดฉุกเฉิน';b.style.background='';b.style.color='';s.textContent='✅ ทำงานปกติ';s.style.color='#9ae8d0';}}"
"async function refreshEstop(){try{const r=await fetch('/status.json');const j=await r.json();renderEstop(!!j.estop);}catch(e){}}"
"document.getElementById('sys-estop').addEventListener('click',async()=>{"
"  const willStop=!document.getElementById('sys-estop').textContent.includes('ยกเลิก');"
"  if(willStop&&!confirm('ยืนยันหยุดฉุกเฉิน? การจ่ายยาทุกอันจะถูกบล็อกทันที'))return;"
"  if(!willStop&&!confirm('ยกเลิกหยุดฉุกเฉิน? เครื่องจะกลับมาจ่ายยาตามกำหนด'))return;"
"  try{const r=await fetch('/tech/estop?action=toggle',{method:'POST'});const j=await r.json();renderEstop(!!j.active);}catch(e){alert('ขัดข้อง');}"
"});"
"refreshEstop();setInterval(refreshEstop,5000);"

/* Quiet hours load + save (System tab) */
"function minToHHMM(m){const h=Math.floor(m/60),mm=m%60;return String(h).padStart(2,'0')+':'+String(mm).padStart(2,'0');}"
"function hhmmToMin(s){const [h,m]=s.split(':').map(Number);return h*60+m;}"
"async function loadQuiet(){try{const r=await fetch('/status.json');const j=await r.json();"
"  document.getElementById('qh-start').value=minToHHMM(j.quiet_start_min||0);"
"  document.getElementById('qh-end').value=minToHHMM(j.quiet_end_min||0);"
"  const st=document.getElementById('qh-state');"
"  if((j.quiet_start_min||0)===(j.quiet_end_min||0)){st.textContent='ปิดอยู่';st.style.color='#a7bdd7';}"
"  else{st.textContent='เปิดอยู่ '+minToHHMM(j.quiet_start_min)+' → '+minToHHMM(j.quiet_end_min);st.style.color='#9ae8d0';}"
"}catch(e){}}"
"document.getElementById('qh-save').addEventListener('click',async()=>{"
"  const s=hhmmToMin(document.getElementById('qh-start').value);"
"  const e=hhmmToMin(document.getElementById('qh-end').value);"
"  const st=document.getElementById('qh-state');st.textContent='กำลังบันทึก…';"
"  try{await fetch('/tech/quiet',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'start_min='+s+'&end_min='+e});loadQuiet();}"
"  catch(err){st.textContent='ขัดข้อง';st.style.color='#ff8a80';}});"
"document.getElementById('qh-disable').addEventListener('click',async()=>{"
"  if(!confirm('ปิดช่วงเวลาเงียบ? (สเก็ดดูลจะกลับมาจ่ายตามปกติทันที)'))return;"
"  const st=document.getElementById('qh-state');st.textContent='กำลังปิด…';"
"  try{await fetch('/tech/quiet',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'start_min=0&end_min=0'});"
"    document.getElementById('qh-start').value='00:00';document.getElementById('qh-end').value='00:00';loadQuiet();}"
"  catch(err){st.textContent='ขัดข้อง';st.style.color='#ff8a80';}});"
"loadQuiet();"

/* Max-pills setting — global ceiling, persisted to NVS via shadow.
 * Status comes back through /status.json so the same poll keeps both
 * quiet-hours and max-pills fresh. */
"async function loadMaxPills(){"
"  try{const r=await fetch('/status.json',{cache:'no-store'});const j=await r.json();"
"  const v=j&&j.max_pills?parseInt(j.max_pills,10):16;"
"  const el=document.getElementById('mp-input');if(el)el.value=v;"
"  const st=document.getElementById('mp-state');"
"  if(st){st.textContent='ค่าปัจจุบัน: '+v+' เม็ด';st.style.color='var(--text-dim)';}"
"  }catch(e){}}"
"document.getElementById('mp-save').addEventListener('click',async()=>{"
"  const v=parseInt(document.getElementById('mp-input').value,10);"
"  if(!v||v<1||v>999){alert('กรุณาใส่ค่า 1–999');return;}"
"  const st=document.getElementById('mp-state');st.textContent='กำลังบันทึก…';st.style.color='var(--accent)';"
"  try{const r=await fetch('/tech/maxpills',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'max_pills='+v});"
"    const j=await r.json();"
"    if(j&&j.ok){st.textContent='บันทึกแล้ว (ค่า '+v+' เม็ด)';st.style.color='var(--ok)';loadMaxPills();}"
"    else{st.textContent='ผิดพลาด: '+(j&&j.error||'unknown');st.style.color='#ff8a80';}"
"  }catch(e){st.textContent='ขัดข้อง — ลองใหม่';st.style.color='#ff8a80';}"
"});"
"loadMaxPills();"

/* Module-map (Servo+IR swap). Each select holds physical-slot options 1..6;
 * the saved value MUST be a permutation (each phys slot used exactly once)
 * — server validates again, but check client-side too for a friendly error. */
"function mmFillSelects(map){"
"  for(let i=0;i<6;i++){"
"    const sel=document.getElementById('mm-'+i);if(!sel)continue;"
"    sel.innerHTML='';"
"    for(let p=0;p<6;p++){"
"      const o=document.createElement('option');o.value=p;o.textContent='ช่องฮาร์ดแวร์ '+(p+1);"
"      if(map[i]===p)o.selected=true;"
"      sel.appendChild(o);"
"    }"
"  }"
"}"
"async function mmLoad(){"
"  try{const r=await fetch('/tech/module_map',{cache:'no-store'});const j=await r.json();"
"    if(j&&j.ok&&Array.isArray(j.map)&&j.map.length===6){mmFillSelects(j.map.map(Number));"
"      const st=document.getElementById('mm-state');if(st){st.textContent='ค่าปัจจุบัน: '+j.map.map(v=>parseInt(v,10)+1).join(',');st.style.color='var(--text-dim)';}"
"    }"
"  }catch(e){}"
"}"
"function mmReadSelects(){const out=[];for(let i=0;i<6;i++){const sel=document.getElementById('mm-'+i);if(!sel)return null;out.push(parseInt(sel.value,10));}return out;}"
"function mmIsPermutation(a){const s=new Set(a);return s.size===6&&Array.from(s).every(v=>v>=0&&v<=5);}"
"document.getElementById('mm-save').addEventListener('click',async()=>{"
"  const m=mmReadSelects();const st=document.getElementById('mm-state');"
"  if(!m||!mmIsPermutation(m)){st.textContent='ผิดพลาด: ต้องเลือกฮาร์ดแวร์ทุกช่องเป็นค่าไม่ซ้ำกัน';st.style.color='#ff8a80';return;}"
"  st.textContent='กำลังบันทึก…';st.style.color='var(--accent)';"
"  try{const r=await fetch('/tech/module_map',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'map='+m.join(',')});"
"    const j=await r.json();"
"    if(j&&j.ok){st.textContent='บันทึกแล้ว: '+m.map(v=>v+1).join(',');st.style.color='var(--ok)';}"
"    else{st.textContent='ผิดพลาด: '+(j&&j.error||'unknown');st.style.color='#ff8a80';}"
"  }catch(e){st.textContent='ขัดข้อง — ลองใหม่';st.style.color='#ff8a80';}"
"});"
"document.getElementById('mm-reset').addEventListener('click',async()=>{"
"  if(!confirm('รีเซ็ตการสลับโมดูลเป็นค่าเดิม (1→1, 2→2, … , 6→6)?'))return;"
"  const st=document.getElementById('mm-state');st.textContent='กำลังรีเซ็ต…';st.style.color='var(--accent)';"
"  try{const r=await fetch('/tech/module_map',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'map=0,1,2,3,4,5'});"
"    const j=await r.json();"
"    if(j&&j.ok){st.textContent='รีเซ็ตเป็นค่าเดิมแล้ว';st.style.color='var(--ok)';mmLoad();}"
"    else{st.textContent='ผิดพลาด: '+(j&&j.error||'unknown');st.style.color='#ff8a80';}"
"  }catch(e){st.textContent='ขัดข้อง — ลองใหม่';st.style.color='#ff8a80';}"
"});"
"mmLoad();"


/* Audit history panel */
"function fmtTs(ts){if(!ts)return '—';const d=new Date(ts*1000);"
"  return d.getFullYear()+'-'+String(d.getMonth()+1).padStart(2,'0')+'-'+String(d.getDate()).padStart(2,'0')+' '"
"  +String(d.getHours()).padStart(2,'0')+':'+String(d.getMinutes()).padStart(2,'0')+':'+String(d.getSeconds()).padStart(2,'0');}"
"function srcLabel(c){return ({M:'Manual',S:'Schedule',V:'Sensor',W:'Web'})[c]||c;}"
"function srcColor(c){return ({M:'#ffd166',S:'#9ae8d0',V:'#6db8ff',W:'#dbe9ff'})[c]||'#a7bdd7';}"
"async function auditRefresh(){const list=document.getElementById('audit-list');const st=document.getElementById('audit-state');"
"  st.textContent='กำลังโหลด…';"
"  try{const r=await fetch('/audit.json',{cache:'no-store'});const j=await r.json();"
"    if(!j.ok||!j.entries.length){list.innerHTML='<div style=\"color:#a7bdd7;padding:14px;text-align:center\">ยังไม่มีประวัติ</div>';st.textContent='';return;}"
"    let h='';for(const e of j.entries){const delta=e.to-e.from;const sign=delta>0?'+':'';const dColor=delta<0?'#ff8a80':'#9ae8d0';"
"      h+=\"<div style='display:flex;justify-content:space-between;align-items:center;padding:10px 14px;background:#06101e;border:1px solid #1a2a44;border-radius:10px'>\""
"      +\"<div style='flex:1'><div style='font-weight:700'>ช่อง \"+e.med+(e.name?' &middot; '+e.name:'')+\"</div>\""
"      +\"<div style='font-size:11px;color:#a7bdd7'>\"+fmtTs(e.ts)+\"</div></div>\""
"      +\"<div style='text-align:right'>\""
"      +\"<div style='font-size:18px;font-weight:800;color:\"+dColor+\"'>\"+e.from+'→'+e.to+' (<span>'+sign+delta+'</span>)'+\"</div>\""
"      +\"<div style='font-size:11px;color:\"+srcColor(e.src)+\"'>\"+srcLabel(e.src)+\"</div>\""
"      +\"</div></div>\";}"
"    list.innerHTML=h;st.textContent=j.count+' รายการ';"
"  }catch(e){list.innerHTML='<div style=\"color:#ff8a80;padding:14px\">โหลดไม่สำเร็จ: '+e.message+'</div>';st.textContent='';}}"
"document.getElementById('audit-refresh').addEventListener('click',auditRefresh);"
"(new MutationObserver(()=>{const p=document.querySelector('[data-panel=audit]');if(p&&p.classList.contains('active'))auditRefresh();})).observe(document.querySelector('[data-panel=audit]'),{attributes:true,attributeFilter:['class']});"

"const camImg=document.getElementById('cam-stream');"
"const streamBase=location.protocol+'//'+location.hostname+':81';"
"document.getElementById('cam-snap').href=streamBase+'/capture';"
"function camStart(){camImg.alt='กำลังโหลดสตรีม...';camImg.src=streamBase+'/stream?t='+Date.now();}"
"function camStop(){if(camImg.src)camImg.src='';camImg.alt='หยุดสตรีมแล้ว';}"
"document.getElementById('cam-start').addEventListener('click',camStart);"
"document.getElementById('cam-stop').addEventListener('click',camStop);"
"const camQ=document.getElementById('cam-q');const camQV=document.getElementById('cam-q-val');"
"const camBri=document.getElementById('cam-bri');const camBriV=document.getElementById('cam-bri-val');"
"const camCon=document.getElementById('cam-con');const camConV=document.getElementById('cam-con-val');"
"const camSat=document.getElementById('cam-sat');const camSatV=document.getElementById('cam-sat-val');"
"if(camQ){camQ.addEventListener('input',()=>camQV.textContent=camQ.value);"
"  if(camBri)camBri.addEventListener('input',()=>camBriV.textContent=camBri.value);"
"  if(camCon)camCon.addEventListener('input',()=>camConV.textContent=camCon.value);"
"  if(camSat)camSat.addEventListener('input',()=>camSatV.textContent=camSat.value);"
"  fetch('/camera/state').then(r=>r.json()).then(j=>{if(j){if(j.jpeg_quality){camQ.value=j.jpeg_quality;camQV.textContent=j.jpeg_quality;}if(typeof j.brightness==='number'){camBri.value=j.brightness;camBriV.textContent=j.brightness;}if(typeof j.contrast==='number'){camCon.value=j.contrast;camConV.textContent=j.contrast;}if(typeof j.saturation==='number'){camSat.value=j.saturation;camSatV.textContent=j.saturation;}}}).catch(()=>{});}"
"document.getElementById('cam-q-apply').addEventListener('click',async()=>{const v=camQ.value;try{await fetch('/camera/set?quality='+v);camStart();}catch(e){}});"
"const camImgApply=document.getElementById('cam-img-apply');"
"if(camImgApply)camImgApply.addEventListener('click',async()=>{try{await fetch('/camera/set?brightness='+camBri.value+'&contrast='+camCon.value+'&saturation='+camSat.value);camStart();}catch(e){}});"
"(new MutationObserver(()=>{const p=document.querySelector('[data-panel=camera]');if(p&&!p.classList.contains('active'))camStop();})).observe(document.querySelector('[data-panel=camera]'),{attributes:true,attributeFilter:['class']});"
"document.addEventListener('visibilitychange',()=>{if(document.visibilityState==='hidden')camStop();});"
"window.addEventListener('pagehide',camStop);"

"function servoAction(ch,act){return fetch('/servo/'+act+'?ch='+ch,{cache:'no-store'}).then(r=>r.text()).catch(()=>null);}"
"function servoSet(ch,home,work){return fetch('/servo/set?ch='+ch+'&home='+home+'&work='+work,{cache:'no-store'}).then(r=>r.json()).catch(()=>null);}"
"async function renderServos(){const wrap=document.getElementById('servo-cards');"
"  let cur=Array(6).fill({home:90,work:45,cur:0});"
"  try{const r=await fetch('/servo/state',{cache:'no-store'});if(r.ok){const j=await r.json();if(j.channels)cur=j.channels;}}catch(e){}"
"  let h='';for(let i=0;i<6;i++){const s=cur[i]||{home:90,work:45,cur:0};"
"    h+=\"<div class='card'><div class='k'>เซอร์โว ช่อง \"+i+\" <span style='float:right;color:#9ae8d0'>ปัจจุบัน: \"+s.cur+\"&deg;</span></div>\""
"    +\"<div style='display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px'>\""
"    +\"<label style='font-size:12px;color:#a7bdd7'>ตำแหน่งเริ่มต้น<input id='h\"+i+\"' type='number' min='0' max='180' value='\"+s.home+\"' style='width:100%;margin-top:4px;padding:6px 8px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff'></label>\""
"    +\"<label style='font-size:12px;color:#a7bdd7'>ตำแหน่งจ่ายยา<input id='w\"+i+\"' type='number' min='0' max='180' value='\"+s.work+\"' style='width:100%;margin-top:4px;padding:6px 8px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff'></label>\""
"    +\"</div><div class='row' style='margin-top:10px;gap:6px;flex-wrap:wrap'>\""
"    +\"<button class='btn primary' data-save='\"+i+\"' style='min-height:36px;padding:0 10px;font-size:13px'>บันทึก</button>\""
"    +\"<button class='btn ghost' data-ch='\"+i+\"' data-act='home' style='min-height:36px;padding:0 10px;font-size:13px'>เริ่มต้น</button>\""
"    +\"<button class='btn ghost' data-ch='\"+i+\"' data-act='work' style='min-height:36px;padding:0 10px;font-size:13px'>จ่ายยา</button>\""
"    +\"<button class='btn ghost' data-ch='\"+i+\"' data-act='test' style='min-height:36px;padding:0 10px;font-size:13px'>ทดสอบ</button>\""
"    +\"</div></div>\";}wrap.innerHTML=h;"
"  wrap.querySelectorAll('button[data-ch]').forEach(b=>b.addEventListener('click',()=>servoAction(b.dataset.ch,b.dataset.act)));"
"  wrap.querySelectorAll('button[data-save]').forEach(b=>b.addEventListener('click',async()=>{const i=b.dataset.save;const hh=parseInt(document.getElementById('h'+i).value);const ww=parseInt(document.getElementById('w'+i).value);if(isNaN(hh)||isNaN(ww))return alert('มุมไม่ถูกต้อง');b.textContent='กำลังบันทึก...';const res=await servoSet(i,hh,ww);b.textContent=res&&res.ok?'บันทึกแล้ว':'ผิดพลาด';setTimeout(()=>{b.textContent='บันทึก';},1200);}));"
"}"
"renderServos();"
"document.getElementById('servo-refresh').addEventListener('click',renderServos);"
"document.getElementById('servo-home-all').addEventListener('click',async()=>{if(!confirm('กลับตำแหน่งเริ่มต้นทุกช่อง? (servo จะหมุนพร้อมกัน 6 ช่อง)'))return;for(let i=0;i<6;i++)await servoAction(i,'home');});"
"document.getElementById('servo-test-all').addEventListener('click',async()=>{if(!confirm('ทดสอบทุกช่อง? ⚠️ ถ้ามียาในตลับ ยาจะออกมา ทำในช่วงตลับว่างเท่านั้น'))return;for(let i=0;i<6;i++){await servoAction(i,'test');await new Promise(r=>setTimeout(r,400));}});"

/* Sound config panel */
"const SOUND_FIELDS=[{k:'alarm',label:'เสียงเตือนหลัก'},{k:'button',label:'เสียงปุ่ม'},{k:'volup_th',label:'เพิ่มเสียง (TH)'},{k:'voldn_th',label:'ลดเสียง (TH)'},{k:'volup_en',label:'Increase (EN)'},{k:'voldn_en',label:'Decrease (EN)'},{k:'disp_th',label:'จ่ายยาสำเร็จ (TH)'},{k:'ret_th',label:'คืนยาสำเร็จ (TH)'},{k:'nomeds_th',label:'ไม่พบยา (TH)'},{k:'disp_en',label:'Dispensed (EN)'},{k:'ret_en',label:'Returned (EN)'},{k:'nomeds_en',label:'No medication (EN)'}];"
"async function sndRender(){"
"  let cfg={alarm:1,button:10,disp_th:83,ret_th:84,nomeds_th:85,disp_en:86,ret_en:87,nomeds_en:88,volup_th:95,volup_en:97,voldn_th:96,voldn_en:98,volume:25,alarm_interval_s:15};"
"  try{const r=await fetch('/sound/config',{cache:'no-store'});if(r.ok){const j=await r.json();if(j.ok)Object.assign(cfg,j);}}catch(e){}"
"  const wrap=document.getElementById('sound-cards');let h='';"
"  h+=\"<div class='card'><div class='k'>ระดับเสียงทดสอบ (0-30)</div><input id='snd_volume' type='number' min='0' max='30' value='\"+cfg.volume+\"' style='width:100%;margin-top:8px;padding:8px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff'></div>\";"
"  h+=\"<div class='card'><div class='k'>ระยะห่างเสียงเตือน (วินาที)</div><div style='font-size:11px;color:#a7bdd7;margin-bottom:6px'>เวลาเงียบระหว่างเล่นซ้ำตอนรอผู้ใช้กดยืนยัน &middot; 5–120 วิ &middot; ตั้งให้ยาวกว่าเสียงเตือน</div><input id='snd_alarm_interval_s' type='number' min='5' max='120' value='\"+cfg.alarm_interval_s+\"' style='width:100%;padding:8px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff'></div>\";"
"  for(const f of SOUND_FIELDS){"
"    h+=\"<div class='card'><div class='k'>\"+f.label+\"</div>\""
"     +\"<div style='display:flex;gap:6px;margin-top:8px;align-items:center'>\""
"     +\"<input id='snd_\"+f.k+\"' type='number' min='1' max='999' value='\"+cfg[f.k]+\"' style='flex:1;padding:8px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff;font-size:14px'>\""
"     +\"<button class='btn ghost' data-test='\"+f.k+\"' style='min-height:38px;padding:0 12px'>▶ ทดสอบ</button>\""
"     +\"</div></div>\";"
"  }wrap.innerHTML=h;"
"  wrap.querySelectorAll('button[data-test]').forEach(b=>b.addEventListener('click',async()=>{"
"    const k=b.dataset.test;const n=parseInt(document.getElementById('snd_'+k).value);"
"    if(!n||n<1)return;b.textContent='กำลังเล่น...';"
"    try{const r=await fetch('/sound/play?track='+n);const j=await r.json();b.textContent=j.ok?'▶ ทดสอบ':'ผิดพลาด';}catch(e){b.textContent='ผิดพลาด';}"
"    setTimeout(()=>{b.textContent='▶ ทดสอบ';},1200);"
"  }));"
"}"
"async function sndSave(){const st=document.getElementById('snd-state');st.textContent='กำลังบันทึก...';"
"  const fd=new URLSearchParams();fd.set('volume',document.getElementById('snd_volume').value);"
"  fd.set('alarm_interval_s',document.getElementById('snd_alarm_interval_s').value);"
"  for(const f of SOUND_FIELDS){fd.set(f.k,document.getElementById('snd_'+f.k).value);}"
"  try{const r=await fetch('/sound/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:fd});const j=await r.json();st.textContent=j.ok?'บันทึกแล้ว ✓':'ผิดพลาด';}catch(e){st.textContent='ขัดข้อง';}}"
"document.getElementById('snd-save').addEventListener('click',sndSave);"
"document.getElementById('snd-reload').addEventListener('click',()=>{document.getElementById('snd-state').textContent='โหลดแล้ว ✓';sndRender();});"
"(new MutationObserver(()=>{const p=document.querySelector('[data-panel=sound]');if(p&&p.classList.contains('active'))sndRender();})).observe(document.querySelector('[data-panel=sound]'),{attributes:true,attributeFilter:['class']});"

/* WiFi panel: native scan list + connect form */
"function wifiSignalLabel(rssi){if(rssi>=-55)return 'แรงมาก';if(rssi>=-67)return 'แรง';if(rssi>=-78)return 'ปานกลาง';return 'อ่อน';}"
"function wifiBars(rssi){const lvl=rssi>=-55?4:rssi>=-67?3:rssi>=-78?2:1;let b='';for(let i=0;i<4;i++)b+=i<lvl?'▮':'▯';return b;}"
"async function wifiScan(){"
"  const list=document.getElementById('wifi-list');list.innerHTML='<div style=\"color:#a7bdd7;padding:14px;text-align:center\">กำลังสแกน… (~10 วินาที)</div>';"
"  try{const r=await fetch('/wifi/scan',{cache:'no-store'});const j=await r.json();"
"    if(!Array.isArray(j)||j.length===0){list.innerHTML='<div style=\"color:#ff8a80;padding:14px;text-align:center\">ไม่พบเครือข่าย</div>';return;}"
"    j.sort((a,b)=>(b.rssi||-100)-(a.rssi||-100));"
"    let h='';for(const n of j){const ssid=(n.ssid||'').replace(/[<>\"']/g,'?');"
"      h+=\"<div data-ssid='\"+ssid+\"' style='display:flex;justify-content:space-between;align-items:center;padding:10px 14px;border-radius:10px;background:#06101e;border:1px solid #1a2a44;cursor:pointer'>\""
"       +\"<div><div style='font-weight:700'>\"+(ssid||'(ไม่ระบุชื่อ)')+\"</div><div style='font-size:11px;color:#a7bdd7'>\"+wifiSignalLabel(n.rssi)+\" · \"+(n.rssi||'-')+\" dBm</div></div>\""
"       +\"<div style='color:#9ae8d0;font-family:monospace'>\"+wifiBars(n.rssi)+\"</div></div>\";}"
"    list.innerHTML=h;"
"    list.querySelectorAll('div[data-ssid]').forEach(d=>d.addEventListener('click',()=>{document.getElementById('wifi-ssid').value=d.dataset.ssid;document.getElementById('wifi-pass').focus();list.querySelectorAll('div[data-ssid]').forEach(x=>x.style.borderColor='#1a2a44');d.style.borderColor='#9ae8d0';}));"
"  }catch(e){list.innerHTML='<div style=\"color:#ff8a80;padding:14px;text-align:center\">สแกนล้มเหลว: '+e.message+'</div>';}}"
"async function wifiSave(){const st=document.getElementById('wifi-state');"
"  const ssid=document.getElementById('wifi-ssid').value.trim();"
"  const pass=document.getElementById('wifi-pass').value;"
"  if(!ssid){st.textContent='ต้องเลือก SSID ก่อน';st.style.color='#ff8a80';return;}"
"  if(!confirm('บันทึกและรีสตาร์ทเพื่อเชื่อมต่อ \"'+ssid+'\" ?'))return;"
"  st.textContent='กำลังบันทึก…';st.style.color='#9ae8d0';"
"  try{const fd=new URLSearchParams();fd.set('ssid',ssid);fd.set('pass',pass);"
"    await fetch('/wifi/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:fd});"
"    st.textContent='บันทึกแล้ว — บอร์ดกำลังรีสตาร์ท ~15 วินาที';"
"  }catch(e){st.textContent='ขัดข้อง: '+e.message;st.style.color='#ff8a80';}}"
"async function wifiForget(){if(!confirm('ลบรหัส Wi-Fi ที่เก็บไว้และรีสตาร์ท?'))return;"
"  const st=document.getElementById('wifi-state');st.textContent='กำลังล้าง…';"
"  try{await fetch('/wifi/forget',{method:'POST'});st.textContent='ล้างแล้ว — รีสตาร์ทใน 5 วินาที';}"
"  catch(e){st.textContent='ขัดข้อง';st.style.color='#ff8a80';}}"
"document.getElementById('wifi-scan').addEventListener('click',wifiScan);"
"document.getElementById('wifi-save').addEventListener('click',wifiSave);"
"document.getElementById('wifi-forget').addEventListener('click',wifiForget);"
"(new MutationObserver(()=>{const p=document.querySelector('[data-panel=wifi]');if(p&&p.classList.contains('active')&&!document.getElementById('wifi-list').dataset.scanned){document.getElementById('wifi-list').dataset.scanned='1';wifiScan();}})).observe(document.querySelector('[data-panel=wifi]'),{attributes:true,attributeFilter:['class']});"
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

static void factory_reset_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));   /* let HTTP response flush */
    ESP_LOGW(TAG, "FACTORY RESET — erasing NVS + rebooting");
    nvs_flash_erase();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* POST /api/factory-reset?confirm=YES
 *
 * Wipes the NVS partition (clears WiFi creds, schedules, stock counts,
 * sound settings, etc.) and reboots into a fresh factory state.
 * Requires confirm=YES query param to prevent accidental wipes from
 * typo'd URLs or browser prefetch. */
esp_err_t factory_reset_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char qbuf[64] = "";
    size_t qlen = httpd_req_get_url_query_len(req);
    bool confirmed = false;
    if (qlen > 0 && qlen < sizeof(qbuf)) {
        if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
            char val[16] = "";
            if (httpd_query_key_value(qbuf, "confirm", val, sizeof(val)) == ESP_OK) {
                if (strcmp(val, "YES") == 0) confirmed = true;
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (!confirmed) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req,
            "{\"ok\":false,\"msg\":\"missing confirm=YES query param\"}");
        ESP_LOGW(TAG, "Factory reset DENIED — missing confirm=YES");
        return ESP_OK;
    }
    httpd_resp_sendstr(req,
        "{\"ok\":true,\"msg\":\"NVS wiped — rebooting in 1s\"}");
    xTaskCreate(factory_reset_task, "fact_rst", 3072, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t tech_reboot_handler(httpd_req_t *req)
{
    /* Reboot endpoint TEMPORARILY DISABLED to stop a phantom-reboot loop:
     * something (browser tab, polling script, leftover JS on /tech page)
     * was calling /tech/reboot every few seconds, panicking the system
     * mid-init and leaving the I2C bus + camera in stuck states. Coredump
     * consistently showed httpd task in esp_restart_noos. Until we find
     * what's calling it, this endpoint returns 503 — power-cycle the
     * board manually if a reboot is genuinely needed. */
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req,
        "{\"ok\":false,\"msg\":\"reboot endpoint disabled — power-cycle manually\"}");
    ESP_LOGW(TAG, "Reboot request DENIED (endpoint disabled to stop reboot loop)");
    return ESP_OK;
    /* Original code kept for re-enabling later:
     *   esp_err_t auth = web_require_tech_api_auth(req);
     *   if (auth != ESP_OK) return auth;
     *   httpd_resp_sendstr(req, "{\"ok\":true,\"msg\":\"rebooting\"}");
     *   xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
     */
    (void)reboot_task;  /* suppress unused warning */
}

extern void dispenser_emergency_set(void);
extern void dispenser_emergency_clear(void);
extern bool dispenser_emergency_active(void);
extern void dispenser_set_quiet_hours(int start_min, int end_min);
extern void dispenser_get_quiet_hours(int *start_min, int *end_min);

/* POST /tech/estop?action=set|clear|toggle (default: toggle).
 * Returns the resulting state. */
esp_err_t tech_estop_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char query[64] = {0};
    char action[16] = "toggle";
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "action", action, sizeof(action));
    }

    if (strcmp(action, "set") == 0) {
        dispenser_emergency_set();
    } else if (strcmp(action, "clear") == 0) {
        dispenser_emergency_clear();
    } else {  // toggle
        if (dispenser_emergency_active()) dispenser_emergency_clear();
        else dispenser_emergency_set();
    }

    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"active\":%s}",
             dispenser_emergency_active() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* POST /tech/quiet?start_min=N&end_min=M
 * Save quiet-hours window. start==end disables it. */
esp_err_t tech_quiet_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char body[80] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len < 0) return ESP_FAIL;
    body[len] = '\0';

    // Minimal x-www-form-urlencoded parser — accepts start_min=N&end_min=M.
    int s = 0, e = 0;
    const char *p_s = strstr(body, "start_min=");
    const char *p_e = strstr(body, "end_min=");
    if (p_s) s = atoi(p_s + 10);
    if (p_e) e = atoi(p_e + 8);
    if (s < 0 || s >= 1440) s = 0;
    if (e < 0 || e >= 1440) e = 0;
    dispenser_set_quiet_hours(s, e);

    char out[80];
    snprintf(out, sizeof(out), "{\"ok\":true,\"start_min\":%d,\"end_min\":%d}", s, e);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

/* POST /tech/maxpills?max_pills=N
 * Global per-module pill ceiling. Updates the NETPIE shadow's max_pills
 * field which firmware reads via dispenser_max_pills(); the touch UI
 * and MQTT count clamps both pick it up immediately. NETPIE publishes
 * back out so the widget reflects the new ceiling. NVS persistence is
 * handled by the shadow-cache layer. */
esp_err_t tech_maxpills_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char body[80] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len < 0) return ESP_FAIL;
    body[len] = '\0';

    const char *p = strstr(body, "max_pills=");
    int v = p ? atoi(p + 10) : 0;
    if (v < 1 || v > 999) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"value must be 1..999\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    /* Local update + MQTT publish in one call — sets s_shadow.max_pills
     * immediately so dispenser_max_pills() returns the new value on the
     * very next call without waiting for the MQTT round-trip echo. */
    netpie_shadow_update_max_pills(v);

    char out[80];
    snprintf(out, sizeof(out), "{\"ok\":true,\"max_pills\":%d}", v);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

/* GET  /tech/module_map → {"ok":true,"map":[p0,p1,p2,p3,p4,p5]}
 * POST /tech/module_map  body "map=p0,p1,p2,p3,p4,p5"
 *
 * Each pX is the physical slot (0..5) for logical med X. The full set
 * must be a permutation — duplicates rejected with 400. Persisted to
 * NVS by module_map_set_all so it survives reboot. */
esp_err_t tech_module_map_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (req->method == HTTP_GET) {
        int cur[DISPENSER_MED_COUNT];
        module_map_copy(cur);
        char out[96];
        snprintf(out, sizeof(out),
                 "{\"ok\":true,\"map\":[%d,%d,%d,%d,%d,%d]}",
                 cur[0], cur[1], cur[2], cur[3], cur[4], cur[5]);
        return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    }

    /* POST */
    char body[80] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len < 0) return ESP_FAIL;
    body[len] = '\0';

    const char *p = strstr(body, "map=");
    if (!p) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"missing map=\"}", HTTPD_RESP_USE_STRLEN);
    }
    p += 4;

    int parsed[DISPENSER_MED_COUNT];
    int n = 0;
    while (*p && n < DISPENSER_MED_COUNT) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        parsed[n++] = (int)v;
        p = end;
        if (*p == ',') p++;
    }
    if (n != DISPENSER_MED_COUNT) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"need 6 comma-separated values\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    if (!module_map_set_all(parsed)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"map must be a permutation of 0..5\"}",
            HTTPD_RESP_USE_STRLEN);
    }

    char out[96];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"map\":[%d,%d,%d,%d,%d,%d]}",
             parsed[0], parsed[1], parsed[2], parsed[3], parsed[4], parsed[5]);
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

