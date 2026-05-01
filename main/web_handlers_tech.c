// web_handlers_tech.c — /tech single-page dashboard with tabs for all tech functions.

#include "web_handlers_tech.h"
#include "web_handlers_status.h"
#include "dispenser_scheduler.h"
#include "config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
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
"<div class='brand'><span class='dot'></span> เครื่องจ่ายยาอัตโนมัติ &middot; แผงควบคุมช่าง</div>"
"<div class='role'>ช่าง</div>"
"<div class='spacer'></div>"
"<a class='logout' href='/cloud/logout'>ออกจากระบบ</a>"
"</div>"

"<div class='tabs' id='tabs'>"
"<div class='tab active' data-tab='dashboard'>ภาพรวม</div>"
"<div class='tab' data-tab='monitor'>ดูเซ็นเซอร์</div>"
"<div class='tab' data-tab='camera'>กล้อง</div>"
"<div class='tab' data-tab='dispenser'>เซอร์โว</div>"
"<div class='tab' data-tab='calibrate'>คาริเบรต</div>"
"<div class='tab' data-tab='sound'>เสียง</div>"
"<div class='tab' data-tab='audit'>ประวัติยา</div>"
"<div class='tab' data-tab='cloud'>คลาวด์</div>"
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
"<h2 style='margin-top:24px'>เซ็นเซอร์ IR (PCF8574)</h2>"
"<div class='sub'>สถานะ IR ตรวจจับ 6 ช่อง &middot; ON = ตรวจพบวัตถุ &middot; OFF = ว่าง</div>"
"<div class='cards' id='ir-cards' style='grid-template-columns:repeat(auto-fit,minmax(120px,1fr))'>"
"<div class='card' style='text-align:center'><div class='k'>IR P0</div><div class='v' id='ir-p0' style='font-size:18px'>&mdash;</div></div>"
"<div class='card' style='text-align:center'><div class='k'>IR P1</div><div class='v' id='ir-p1' style='font-size:18px'>&mdash;</div></div>"
"<div class='card' style='text-align:center'><div class='k'>IR P2</div><div class='v' id='ir-p2' style='font-size:18px'>&mdash;</div></div>"
"<div class='card' style='text-align:center'><div class='k'>IR P3</div><div class='v' id='ir-p3' style='font-size:18px'>&mdash;</div></div>"
"<div class='card' style='text-align:center'><div class='k'>IR P4</div><div class='v' id='ir-p4' style='font-size:18px'>&mdash;</div></div>"
"<div class='card' style='text-align:center'><div class='k'>IR P5</div><div class='v' id='ir-p5' style='font-size:18px'>&mdash;</div></div>"
"</div>"
"<div class='sub' style='margin-top:6px;font-size:12px'>byte ปัจจุบัน: <code id='ir-byte' style='color:#9ae8d0'>--</code></div>"
"</div>"

"<div class='panel' data-panel='monitor'>"
"<h2>ดูเซ็นเซอร์แบบเรียลไทม์</h2>"
"<div class='sub'>VL53L0X x6 &middot; ระยะวัดและจำนวนยาในแต่ละช่อง (อัปเดตทุก 2 วินาที)</div>"
"<div class='cards' id='mon-grid' style='grid-template-columns:repeat(auto-fit,minmax(200px,1fr))'></div>"
"</div>"

"<div class='panel' data-panel='camera'>"
"<h2>ภาพถ่ายทอดสด</h2>"
"<div class='sub'>MJPEG stream (OV5647 &middot; 800x640 &middot; 50 fps) &middot; ลด JPEG Quality เพื่อให้ลื่นขึ้น (ภาพหยาบลงเล็กน้อย)</div>"
"<div class='row'>"
"<button class='btn primary' id='cam-start'>เริ่มสตรีม</button>"
"<button class='btn ghost' id='cam-stop'>หยุด</button>"
"<a class='btn ghost' id='cam-snap' target='_blank'>เปิดภาพนิ่ง</a>"
"</div>"
"<div class='row' style='margin-top:10px;align-items:center'>"
"<label style='font-size:12px;color:#a7bdd7;display:flex;align-items:center;gap:8px'>JPEG Quality"
"<input id='cam-q' type='range' min='20' max='60' step='5' value='30' style='width:200px'>"
"<span id='cam-q-val' style='color:#9ae8d0;font-weight:700;min-width:36px;text-align:right'>30</span>"
"</label>"
"<button class='btn ghost' id='cam-q-apply' style='min-height:36px'>นำไปใช้</button>"
"<span style='font-size:12px;color:#a7bdd7'>เกิน 50 ลิงก์ Wi-Fi อาจรับไม่ทัน → ภาพเฟรมขาด/ภาพแตก</span>"
"</div>"
"<div class='frame' style='margin-top:14px;display:flex;align-items:center;justify-content:center;min-height:360px'>"
"<img id='cam-stream' alt='ยังไม่ได้สตรีม' style='max-width:100%;max-height:calc(100vh - 240px);border-radius:12px;background:#03080f' />"
"</div>"
"</div>"

"<div class='panel' data-panel='dispenser'>"
"<h2>ทดสอบเซอร์โว &middot; ตั้งค่ามุม</h2>"
"<div class='sub'>ตั้งค่ามุมตำแหน่งเริ่มต้น/จ่ายยาต่อช่อง + ทดสอบ &middot; บันทึกลง NVS อัตโนมัติ</div>"
"<div class='cards' id='servo-cards'></div>"
"<div class='row' style='margin-top:16px'>"
"<button class='btn primary' id='servo-home-all'>กลับตำแหน่งเริ่มต้นทุกช่อง</button>"
"<button class='btn ghost' id='servo-test-all'>ทดสอบทุกช่อง</button>"
"<button class='btn ghost' id='servo-refresh'>รีเฟรชสถานะ</button>"
"</div>"
"<div class='placeholder' style='margin-top:18px'><p>ตำแหน่งเริ่มต้น/จ่ายยา = มุม (0-180&deg;) &middot; <b>บันทึก</b> = save NVS &middot; <b>ทดสอบ</b> = หมุนไป-กลับเพื่อตรวจสอบ</p></div>"
"</div>"

"<div class='panel' data-panel='calibrate'>"
"<h2>คาริเบรตเซ็นเซอร์ระยะยา</h2>"
"<div class='sub'>ตั้งระยะยาเต็ม &middot; ความสูงเม็ดยา &middot; ความจุสูงสุด &middot; บันทึกอัตโนมัติ &middot; หรือใช้ <b>โหมดคาริเบรตจริง</b> ด้านล่าง</div>"
"<div class='cards' id='cal-cards'></div>"
"<div class='row' style='margin-top:14px'>"
"<button class='btn ghost' id='cal-refresh'>รีเฟรชค่าปัจจุบัน</button>"
"</div>"

"<h3 style='margin-top:30px'>ออโต้คาริเบรต (จ่ายยาเองทีละเม็ด)</h3>"
"<p class='sub'>เติมยาเต็มตลับ → ใส่จำนวนเม็ดที่เต็ม → กด <b>\"เริ่มออโต้คาริเบรต\"</b> · ระบบจะจ่ายยาออกทีละเม็ดจนหมดและบันทึกระยะที่วัดได้ทุกครั้งโดยอัตโนมัติ · ความสูงเม็ดยา <b>ฟิกที่ 15 มม.</b> เป็นพื้นฐาน ระบบจะหาเฉพาะ <b>ระยะยาเต็ม</b> ของตลับนั้นแล้วบันทึกทันที</p>"
"<div class='row' style='align-items:flex-end;gap:10px;flex-wrap:wrap'>"
"<label style='font-size:12px;color:#a7bdd7'>ช่อง"
"<select id='cg-ch' style='display:block;margin-top:4px;padding:10px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff'>"
"<option value='0'>ช่อง 0</option><option value='1'>ช่อง 1</option><option value='2'>ช่อง 2</option>"
"<option value='3'>ช่อง 3</option><option value='4'>ช่อง 4</option><option value='5'>ช่อง 5</option>"
"</select></label>"
"<label style='font-size:12px;color:#a7bdd7'>จำนวนยาที่เต็ม (เม็ด)"
"<input id='cg-max' type='number' min='1' max='99' value='13' style='display:block;margin-top:4px;padding:10px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff;width:120px'></label>"
"<button class='btn primary' id='cg-auto' style='min-height:44px;background:linear-gradient(135deg,#6db8ff,#4dd7b0)'>▶ เริ่มออโต้คาริเบรต</button>"
"<button class='btn warn' id='cg-reset' style='min-height:44px'>ยกเลิก</button>"
"</div>"
"<div id='cg-state' style='margin-top:10px;font-size:13px;color:#a7bdd7'></div>"
"<div id='cg-progress' style='margin-top:8px;height:8px;background:#06101e;border-radius:4px;overflow:hidden;border:1px solid #1a2a44'><div id='cg-progress-bar' style='height:100%;width:0;background:linear-gradient(90deg,#6db8ff,#4dd7b0);transition:width .4s'></div></div>"
"<div id='cg-table' style='margin-top:10px'></div>"
"</div>"

"<div class='panel' data-panel='sound'>"
"<h2>ตั้งค่าเสียงและทดสอบ</h2>"
"<div class='sub'>เลือกหมายเลขแทร็กของเสียงเตือน เสียงปุ่ม เสียงรายงาน &middot; กดปุ่ม ▶ เพื่อทดสอบ &middot; บันทึกลง NVS</div>"
"<div class='cards' id='sound-cards' style='grid-template-columns:repeat(auto-fit,minmax(260px,1fr))'></div>"
"<div class='row' style='margin-top:14px'>"
"<button class='btn primary' id='snd-save'>บันทึกการตั้งค่าเสียง</button>"
"<button class='btn ghost' id='snd-reload'>รีโหลดค่าจากเครื่อง</button>"
"<span id='snd-state' style='align-self:center;color:#9ae8d0;font-size:13px'></span>"
"</div>"
"</div>"

"<div class='panel' data-panel='audit'>"
"<h2>ประวัติการเพิ่ม/ลด/จ่ายยา</h2>"
"<div class='sub'>32 รายการล่าสุด &middot; M=manual, S=scheduled, V=sensor sync</div>"
"<div class='row'><button class='btn ghost' id='audit-refresh'>รีเฟรช</button>"
"<span id='audit-state' style='align-self:center;color:#a7bdd7;font-size:13px'></span></div>"
"<div id='audit-list' style='margin-top:12px;display:flex;flex-direction:column;gap:6px;max-height:calc(100vh - 300px);overflow-y:auto'>"
"<div style='color:#a7bdd7;padding:14px;text-align:center'>กดรีเฟรชเพื่อโหลดประวัติ</div>"
"</div>"
"</div>"

"<div class='panel' data-panel='cloud'>"
"<h2>คลาวด์ &middot; Telegram / Google Sheets / NETPIE</h2>"
"<div class='sub'>ตั้งค่า Bot Token, Chat ID, Google Apps Script URL</div>"
"<div class='frame'><iframe data-src='/cloud' src='about:blank'></iframe></div>"
"</div>"

"<div class='panel' data-panel='wifi'>"
"<h2>ตั้งค่าเครือข่าย Wi-Fi</h2>"
"<div class='sub'>กดสแกนเพื่อค้นหาเครือข่าย &middot; เลือกชื่อจากรายการ &middot; บันทึกแล้วบอร์ดจะรีสตาร์ทอัตโนมัติ</div>"
"<div class='cards' style='grid-template-columns:1fr'>"
"<div class='card'>"
"<div class='card-hdr' style='display:flex;justify-content:space-between;align-items:center;margin-bottom:10px'>"
"<b>เครือข่ายที่พบ</b>"
"<button class='btn ghost' id='wifi-scan' style='min-height:36px;font-size:13px'>↻ สแกน Wi-Fi</button>"
"</div>"
"<div id='wifi-list' style='display:flex;flex-direction:column;gap:6px;max-height:280px;overflow-y:auto'>"
"<div style='color:#a7bdd7;font-size:13px;padding:12px;text-align:center'>กด \"สแกน Wi-Fi\" เพื่อเริ่มค้นหา</div>"
"</div>"
"</div>"
"<div class='card'>"
"<div class='k' style='margin-bottom:10px'>เชื่อมต่อ</div>"
"<label style='font-size:12px;color:#a7bdd7'>SSID"
"<input id='wifi-ssid' type='text' placeholder='เลือกจากรายการ หรือกรอกเอง' style='width:100%;margin-top:4px;padding:10px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff'></label>"
"<label style='font-size:12px;color:#a7bdd7;display:block;margin-top:10px'>รหัสผ่าน"
"<input id='wifi-pass' type='password' placeholder='ใส่รหัสผ่าน Wi-Fi' style='width:100%;margin-top:4px;padding:10px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff'></label>"
"<div class='row' style='margin-top:14px;gap:10px'>"
"<button class='btn primary' id='wifi-save'>บันทึก &amp; รีสตาร์ท</button>"
"<button class='btn warn' id='wifi-forget'>ล้างค่า Wi-Fi</button>"
"<span id='wifi-state' style='align-self:center;color:#9ae8d0;font-size:13px'></span>"
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
"<div class='sub'>รีบูตอุปกรณ์ &middot; ข้อมูลเวอร์ชันเฟิร์มแวร์</div>"
"<div class='cards'>"
"<div class='card'><div class='k'>เฟิร์มแวร์</div><div class='v'>เครื่องจ่ายยาอัตโนมัติ</div><div class='hint'>ESP32-P4-NANO &middot; ESP-IDF v5.3.2</div></div>"
"<div class='card'><div class='k'>IDF Version</div><div class='v' id='sys-idf'>&mdash;</div></div>"
"</div>"
"<div class='row' style='margin-top:22px'>"
"<button class='btn warn' id='sys-estop' style='font-size:16px;padding:0 22px'>🛑 หยุดฉุกเฉิน</button>"
"<button class='btn warn' id='sys-reboot'>รีบูตอุปกรณ์</button>"
"<a class='btn ghost' href='/' target='_blank'>เปิดหน้าหลัก</a>"
"<span id='sys-estop-state' style='align-self:center;font-weight:700'></span>"
"</div>"
"<p class='sub' style='margin-top:18px'>หยุดฉุกเฉิน = บล็อกการจ่ายยาทุกประเภท (manual + scheduled) จนกว่าจะกดอีกครั้งเพื่อยกเลิก สถานะคงอยู่หลังรีบูต</p>"
"<p class='sub'>การรีบูตจะตัดการเชื่อมต่อชั่วคราวประมาณ 15 วินาที</p>"

"<h3 style='margin-top:30px'>ช่วงเวลาเงียบ (Quiet Hours)</h3>"
"<p class='sub'>กำหนดช่วงเวลาที่ห้ามจ่ายยาอัตโนมัติ &middot; ตัวอย่าง 22:00 → 06:00 = ช่วงนอน &middot; manual + Telegram /dispense ยังจ่ายได้ปกติ &middot; เคลียร์ทั้งคู่เป็น 00:00 = ปิดฟีเจอร์</p>"
"<div class='row' style='align-items:flex-end;gap:10px'>"
"<label style='font-size:12px;color:#a7bdd7'>เริ่มเงียบ"
"<input id='qh-start' type='time' value='00:00' style='display:block;margin-top:4px;padding:10px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff;font-family:inherit'></label>"
"<label style='font-size:12px;color:#a7bdd7'>สิ้นสุด"
"<input id='qh-end' type='time' value='00:00' style='display:block;margin-top:4px;padding:10px;border-radius:8px;border:1px solid #35567f;background:#06101e;color:#f4f8ff;font-family:inherit'></label>"
"<button class='btn primary' id='qh-save' style='min-height:44px'>บันทึก</button>"
"<button class='btn ghost' id='qh-disable' style='min-height:44px'>ปิดใช้งาน</button>"
"<span id='qh-state' style='align-self:center;color:#9ae8d0;font-size:13px'></span>"
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
"function setIrCell(id,v){const el=document.getElementById(id);if(!el)return;if(v==null){el.textContent='--';el.style.color='';return;}"
"  const on=(v===0||v==='0'||v===false);el.textContent=on?'● ON':'○ OFF';el.style.color=on?'#9ae8d0':'#5a6b80';}"
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
"    const irb=document.getElementById('ir-byte');if(irb&&j.ir_byte!=null)irb.textContent='0x'+j.ir_byte;"
"    tickClock();"
"  }}catch(e){}finally{dashBusy=false;}"
"}"

"let logPaused=false,logTimer=null,logBusy=false;"
"async function refreshLogs(){if(logBusy||!tabActive('logs'))return;logBusy=true;try{const r=await fetch('/logs/tail',{cache:'no-store'});if(r.ok){const t=await r.text();const el=document.getElementById('log-box');el.textContent=t;el.scrollTop=el.scrollHeight;}}catch(e){}finally{logBusy=false;}}"

"function tickPolls(){if(tabActive('dashboard'))refreshDash();if(tabActive('logs')&&!logPaused)refreshLogs();}"
"setInterval(tickPolls,5000);tickPolls();"
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

/* Monitor panel (native — replaces the old /sensors iframe) */
"async function monRender(){if(!tabActive('monitor'))return;"
"  let chans=[];try{const r=await fetch('/sensors.json',{cache:'no-store'});if(r.ok){const j=await r.json();chans=j.channels||[];}}catch(e){return;}"
"  const g=document.getElementById('mon-grid');let h='';"
"  for(const s of chans){"
"    const max=s.max_pills||1;const pct=s.valid&&s.pill_count>=0?Math.min(100,Math.round(s.pill_count/max*100)):0;"
"    let state,clr;if(!s.present){state='ไม่พบเซ็นเซอร์';clr='#ff8a80';}"
"      else if(!s.valid){state='ไม่มีข้อมูล';clr='#ffd166';}"
"      else if(s.is_empty){state='ว่าง';clr='#ff8a80';}"
"      else if(pct<25){state='ใกล้หมด';clr='#ffd166';}"
"      else{state='พร้อม';clr='#9ae8d0';}"
"    h+=\"<div class='card'><div class='card-hdr' style='display:flex;justify-content:space-between;align-items:center'><b>ช่อง \"+(s.idx)+\"</b><span style='color:\"+clr+\";font-size:12px;font-weight:700'>\"+state+\"</span></div>\""
"     +\"<div style='font-size:28px;font-weight:800;margin-top:6px'>\"+(s.valid?s.filtered_mm+' <small style=\\\"font-size:14px;color:#a7bdd7\\\">mm</small>':'-- mm')+\"</div>\""
"     +\"<div style='height:6px;background:#1a2a44;border-radius:3px;margin-top:8px;overflow:hidden'><div style='height:100%;width:\"+pct+\"%;background:\"+clr+\"'></div></div>\""
"     +\"<div style='font-size:13px;color:#a7bdd7;margin-top:6px'>\"+(s.pill_count>=0?s.pill_count:'-')+\" / \"+(s.max_pills||0)+\" เม็ด</div>\""
"     +\"<div style='font-size:11px;color:#5a6b80;margin-top:4px'>raw=\"+(s.raw_mm||'-')+\"mm &middot; full=\"+(s.full_dist_mm||'-')+\"mm\"+\"</div>\""
"     +\"</div>\";}"
"  g.innerHTML=h;}"
"setInterval(monRender,2000);"
"(new MutationObserver(()=>{const p=document.querySelector('[data-panel=monitor]');if(p&&p.classList.contains('active'))monRender();})).observe(document.querySelector('[data-panel=monitor]'),{attributes:true,attributeFilter:['class']});"

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
"if(camQ){camQ.addEventListener('input',()=>camQV.textContent=camQ.value);"
"  fetch('/camera/state').then(r=>r.json()).then(j=>{if(j&&j.jpeg_quality){camQ.value=j.jpeg_quality;camQV.textContent=j.jpeg_quality;}}).catch(()=>{});}"
"document.getElementById('cam-q-apply').addEventListener('click',async()=>{const v=camQ.value;try{await fetch('/camera/set?quality='+v);camStart();}catch(e){}});"
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

"async function calRender(){"
"  let chans=[];"
"  try{const r=await fetch('/sensors.json',{cache:'no-store'});if(r.ok){const j=await r.json();chans=j.channels||[];}}catch(e){}"
"  if(chans.length===0){chans=Array.from({length:6},(_,i)=>({ch:i,present:false,valid:false,raw_mm:0,filtered_mm:0,pill_count:0,full_dist_mm:50,pill_height_mm:15,max_pills:16}));}"
"  const wrap=document.getElementById('cal-cards');let h='';"
"  for(const s of chans){"
"    const status=s.present?(s.valid?'<span style=\\'color:#9ae8d0\\'>ออนไลน์</span>':'<span style=\\'color:#ffd166\\'>ไม่มีข้อมูล</span>'):'<span style=\\'color:#ff8a80\\'>ออฟไลน์</span>';"
"    h+=\"<div class='card'><div class='k'>VL53 ช่อง \"+s.ch+\" \"+status+\"</div>\""
"     +\"<div style='margin-top:6px;font-size:13px;color:#a7bdd7'>ระยะปัจจุบัน=<b style='color:#fff'>\"+(s.filtered_mm>=0?s.filtered_mm:'-')+\"mm</b> &middot; จำนวน=<b style='color:#fff'>\"+(s.pill_count>=0?s.pill_count:'-')+\" เม็ด</b></div>\""
"     +\"<div style='display:grid;grid-template-columns:1fr 1fr 1fr;gap:6px;margin-top:10px'>\""
"     +\"<label style='font-size:11px;color:#a7bdd7'>ระยะยาเต็ม(mm)<input id='cf\"+s.ch+\"' type='number' min='1' max='500' value='\"+s.full_dist_mm+\"' style='width:100%;margin-top:3px;padding:5px;border-radius:6px;border:1px solid #35567f;background:#06101e;color:#f4f8ff;font-size:13px'></label>\""
"     +\"<label style='font-size:11px;color:#a7bdd7'>ความสูงเม็ดยา(mm)<input id='ch\"+s.ch+\"' type='number' min='1' max='100' value='\"+s.pill_height_mm+\"' style='width:100%;margin-top:3px;padding:5px;border-radius:6px;border:1px solid #35567f;background:#06101e;color:#f4f8ff;font-size:13px'></label>\""
"     +\"<label style='font-size:11px;color:#a7bdd7'>จำนวนสูงสุด<input id='cm\"+s.ch+\"' type='number' min='1' max='99' value='\"+s.max_pills+\"' style='width:100%;margin-top:3px;padding:5px;border-radius:6px;border:1px solid #35567f;background:#06101e;color:#f4f8ff;font-size:13px'></label>\""
"     +\"</div>\""
"     +\"<button class='btn primary' data-cal='\"+s.ch+\"' style='margin-top:8px;min-height:36px;width:100%;font-size:13px'>บันทึก</button>\""
"     +\"<div style='display:flex;gap:6px;margin-top:6px;flex-wrap:wrap'>\""
"     +\"<button class='btn ghost' data-cap='\"+s.ch+\"' data-mode='full' style='flex:1;min-height:32px;font-size:12px'>📦 บันทึกขณะยาเต็ม</button>\""
"     +\"<button class='btn ghost' data-cap='\"+s.ch+\"' data-mode='empty' style='flex:1;min-height:32px;font-size:12px'>🪹 บันทึกขณะยาหมด</button>\""
"     +\"</div>\""
"     +\"<div style='display:flex;gap:6px;margin-top:6px;align-items:center;justify-content:space-between;background:#0e1a2e;padding:6px 10px;border-radius:8px'>\""
"     +\"<span style='font-size:11px;color:#a7bdd7'>ปรับ +/- เม็ด</span>\""
"     +\"<button class='btn ghost' data-off-dec='\"+s.ch+\"' style='min-height:28px;padding:0 12px;font-size:14px'>−</button>\""
"     +\"<span id='offVal\"+s.ch+\"' style='font-weight:800;color:\"+(s.count_offset>0?'#9ae8d0':s.count_offset<0?'#ff8a80':'#fff')+\";min-width:34px;text-align:center'>\"+(s.count_offset>0?'+':'')+s.count_offset+\"</span>\""
"     +\"<button class='btn ghost' data-off-inc='\"+s.ch+\"' style='min-height:28px;padding:0 12px;font-size:14px'>+</button>\""
"     +\"</div>\""
"     +\"<div id='capState\"+s.ch+\"' style='font-size:11px;color:#9ae8d0;margin-top:4px;min-height:14px'></div>\""
"     +\"</div>\";"
"  }wrap.innerHTML=h;"
"  wrap.querySelectorAll('button[data-cal]').forEach(b=>b.addEventListener('click',async()=>{"
"    const i=b.dataset.cal;const f=parseInt(document.getElementById('cf'+i).value);const ph=parseInt(document.getElementById('ch'+i).value);const mx=parseInt(document.getElementById('cm'+i).value);"
"    if(isNaN(f)||isNaN(ph)||isNaN(mx))return alert('ค่าไม่ถูกต้อง');"
"    b.textContent='กำลังบันทึก...';"
"    try{const r=await fetch('/sensors/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ch='+i+'&full_dist_mm='+f+'&pill_height_mm='+ph+'&max_pills='+mx});const j=await r.json();b.textContent=j.ok?'บันทึกแล้ว':'ผิดพลาด';}catch(e){b.textContent='ผิดพลาด';}"
"    setTimeout(()=>{b.textContent='บันทึก';},1500);"
"  }));"
"  wrap.querySelectorAll('button[data-cap]').forEach(b=>b.addEventListener('click',async()=>{"
"    const i=b.dataset.cap;const mode=b.dataset.mode;const st=document.getElementById('capState'+i);"
"    st.textContent='กำลังบันทึก...';"
"    try{const r=await fetch('/sensors/cal_capture',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ch='+i+'&mode='+mode});const j=await r.json();"
"    if(j.ok){st.textContent=(mode==='full'?'ยาเต็ม':'ยาหมด')+' → '+j.captured_mm+' mm ✓';calRender();}"
"    else{st.textContent='ผิดพลาด: '+(j.error||'?');st.style.color='#ff8a80';}"
"    }catch(e){st.textContent='ขัดข้อง';st.style.color='#ff8a80';}"
"  }));"
"  async function adjustOffset(ch,delta){"
"    const valEl=document.getElementById('offVal'+ch);const cur=parseInt(valEl.textContent.replace('+',''))||0;"
"    const next=Math.max(-50,Math.min(50,cur+delta));"
"    if(next===cur)return;"
"    try{await fetch('/sensors/offset',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ch='+ch+'&offset='+next});"
"      valEl.textContent=(next>0?'+':'')+next;valEl.style.color=next>0?'#9ae8d0':next<0?'#ff8a80':'#fff';"
"      const st=document.getElementById('capState'+ch);st.textContent='offset='+(next>0?'+':'')+next+' ✓';st.style.color='#9ae8d0';}"
"    catch(e){const st=document.getElementById('capState'+ch);st.textContent='ขัดข้อง';st.style.color='#ff8a80';}}"
"  wrap.querySelectorAll('button[data-off-dec]').forEach(b=>b.addEventListener('click',()=>adjustOffset(parseInt(b.dataset.offDec),-1)));"
"  wrap.querySelectorAll('button[data-off-inc]').forEach(b=>b.addEventListener('click',()=>adjustOffset(parseInt(b.dataset.offInc),+1)));"
"}"
"document.getElementById('cal-refresh').addEventListener('click',calRender);"
"(new MutationObserver(()=>{const p=document.querySelector('[data-panel=calibrate]');if(p&&p.classList.contains('active'))calRender();})).observe(document.querySelector('[data-panel=calibrate]'),{attributes:true,attributeFilter:['class']});"

/* Auto per-pill calibration session — single button, polled live */
"let cgPoll=null;"
"function cgUpdateProgress(j){const bar=document.getElementById('cg-progress-bar');"
"  if(!j||!j.max_pills){bar.style.width='0';return;}"
"  const total=j.max_pills+1;const cur=Math.min(j.steps?j.steps.length:0,total);"
"  bar.style.width=(100*cur/total)+'%';}"
"function cgStateText(j,extra){const st=document.getElementById('cg-state');"
"  if(!j||!j.ok){st.textContent=(extra||'')+' ผิดพลาด: '+(j&&j.error?j.error:'?');st.style.color='#ff8a80';return;}"
"  const steps=j.steps?j.steps.length:0;"
"  if(j.phase==='running'){st.textContent=(extra?extra+' · ':'')+'กำลังคาริเบรตช่อง '+j.ch+' · จ่ายแล้ว '+Math.max(0,steps-1)+'/'+j.max_pills+' เม็ด · บันทึก '+steps+'/'+(j.max_pills+1)+' จุด';st.style.color='#9ae8d0';}"
"  else if(j.phase==='done'){st.textContent='เสร็จสิ้น ✓ '+steps+' จุด → ระยะยาเต็ม '+j.full_dist_mm+' มม. (height '+j.pill_height_mm+' มม.) บันทึกแล้ว';st.style.color='#9ae8d0';}"
"  else if(j.phase==='error'){st.textContent='ผิดพลาด: '+(j.error||'?')+(steps?(' (เก็บ '+steps+' จุด)'):'');st.style.color='#ff8a80';}"
"  else{st.textContent=extra||'พร้อมเริ่มต้น';st.style.color='#a7bdd7';}}"
"function cgRenderTable(j){const t=document.getElementById('cg-table');"
"  if(!j||!j.steps||!j.steps.length){t.innerHTML='';return;}"
"  let h='<table style=\"border-collapse:collapse;width:100%;font-size:13px\"><thead><tr style=\"text-align:left;color:#a7bdd7\">"
"    <th style=padding:6px>ขั้นที่</th><th style=padding:6px>เหลือ (เม็ด)</th><th style=padding:6px>ระยะ (มม.)</th><th style=padding:6px>Δ จากเม็ดก่อน</th></tr></thead><tbody>';"
"  for(let i=0;i<j.steps.length;i++){const s=j.steps[i];const prev=i?j.steps[i-1]:null;const dd=prev?(s.dist-prev.dist):0;"
"    h+='<tr style=\"border-top:1px solid #1a2a44\"><td style=padding:6px>'+(i+1)+'</td><td style=padding:6px>'+s.count+'</td><td style=padding:6px>'+s.dist+'</td><td style=padding:6px;color:#9ae8d0>'+(prev?(dd>0?'+':'')+dd+' mm':'-')+'</td></tr>';}"
"  h+='</tbody></table>';t.innerHTML=h;}"
"function cgStopPoll(){if(cgPoll){clearInterval(cgPoll);cgPoll=null;}}"
"async function cgFetch(){const ch=document.getElementById('cg-ch').value;"
"  try{const r=await fetch('/sensors/cal_guide?ch='+ch);const j=await r.json();cgStateText(j,'');cgRenderTable(j);cgUpdateProgress(j);"
"    if(j.phase!=='running'){cgStopPoll();if(j.phase==='done')calRender();}"
"  }catch(e){}}"
"function cgStartPoll(){cgStopPoll();cgPoll=setInterval(cgFetch,1500);cgFetch();}"
"document.getElementById('cg-ch').addEventListener('change',cgFetch);"
"document.getElementById('cg-auto').addEventListener('click',async()=>{"
"  const ch=document.getElementById('cg-ch').value;const mx=document.getElementById('cg-max').value;"
"  if(!confirm('เริ่มออโต้คาริเบรตช่อง '+ch+' ที่ '+mx+' เม็ด?\\n• ต้องเติมยาเต็มก่อน\\n• ระบบจะจ่ายยาออกครบ '+mx+' เม็ด\\n• ใช้เวลา ~'+(mx*8)+' วิ'))return;"
"  cgStateText(null,'กำลังเริ่ม…');"
"  try{const r=await fetch('/sensors/cal_guide?ch='+ch+'&action=auto&max_pills='+mx,{method:'POST'});const j=await r.json();"
"    if(!j.ok){cgStateText(j,'ขัดข้อง');return;}"
"    cgStateText(j,'เริ่มแล้ว ✓');cgRenderTable(j);cgUpdateProgress(j);cgStartPoll();"
"  }catch(e){cgStateText(null,'ขัดข้อง');}});"
"document.getElementById('cg-reset').addEventListener('click',async()=>{"
"  const ch=document.getElementById('cg-ch').value;"
"  try{const r=await fetch('/sensors/cal_guide?ch='+ch+'&action=reset',{method:'POST'});const j=await r.json();cgStopPoll();cgStateText(j,'ยกเลิก');document.getElementById('cg-table').innerHTML='';cgUpdateProgress(null);}catch(e){}});"
"cgFetch();"

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

/* GET  /tech/ir          → returns current per-module IR enable mask
 * POST /tech/ir          body: m=<0..5>&v=<0|1>     → toggle one module
 * POST /tech/ir          body: mask=<0..63>          → set all six bits
 *
 * "IR enabled" means the dispense loop polls the PCF8574 IR sensor
 * for that module to count pills. "Disabled" switches that module to
 * servo-trust mode (each servo cycle counts as one pill, capped at
 * cartridge max for return-all). Useful when only some modules have
 * an IR beam wired or the IR is unreliable. */
esp_err_t tech_ir_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    if (req->method == HTTP_POST) {
        char body[80] = {0};
        int len = httpd_req_recv(req, body, sizeof(body) - 1);
        if (len < 0) return ESP_FAIL;
        body[len] = '\0';

        const char *p_mask = strstr(body, "mask=");
        const char *p_m    = strstr(body, "m=");
        const char *p_v    = strstr(body, "v=");
        if (p_mask) {
            int mask = atoi(p_mask + 5) & 0x3F;
            for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
                dispenser_ir_set_present(i, (mask & (1 << i)) != 0);
            }
        } else if (p_m && p_v) {
            int m = atoi(p_m + 2);
            int v = atoi(p_v + 2);
            if (m >= 0 && m < DISPENSER_MED_COUNT) {
                dispenser_ir_set_present(m, v != 0);
            }
        }
    }

    int mask = 0;
    for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
        if (dispenser_ir_present(i)) mask |= (1 << i);
    }
    char out[128];
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"mask\":%d,\"per_module\":[%d,%d,%d,%d,%d,%d]}",
             mask,
             dispenser_ir_present(0) ? 1 : 0,
             dispenser_ir_present(1) ? 1 : 0,
             dispenser_ir_present(2) ? 1 : 0,
             dispenser_ir_present(3) ? 1 : 0,
             dispenser_ir_present(4) ? 1 : 0,
             dispenser_ir_present(5) ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

/* ── IR calibration page ──────────────────────────────────────────── */
static const char IR_CAL_PAGE[] =
"<!doctype html><html lang='th'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>คาริเบรต IR · จับยาตอนไหน</title>"
"<style>"
"body{font-family:'Sarabun','Segoe UI',sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:18px;line-height:1.5}"
"h1{color:#38bdf8;margin:0 0 16px;font-size:22px}"
".panel{background:#1e293b;border:1px solid #334155;border-radius:8px;padding:16px;margin-bottom:14px}"
"button{background:#38bdf8;color:#0f172a;border:none;padding:10px 18px;border-radius:6px;font-size:15px;font-weight:bold;margin:4px;cursor:pointer}"
"button:disabled{background:#475569;color:#94a3b8;cursor:not-allowed}"
"button.mod{min-width:90px}"
".status{padding:8px 12px;border-radius:6px;background:#0f172a;border:1px solid #334155;font-family:monospace}"
".chart{background:#fff;border-radius:6px;margin-top:12px;overflow:hidden}"
"canvas{display:block;width:100%;height:140px;background:#fff}"
".legend{display:flex;gap:18px;margin-top:8px;font-size:14px}"
".legend span{display:inline-block;width:14px;height:14px;border-radius:3px;vertical-align:middle;margin-right:6px}"
".bar-low{background:#ef4444}.bar-high{background:#10b981}"
".summary{font-family:monospace;background:#0f172a;border:1px solid #334155;padding:10px;border-radius:6px;margin-top:10px;white-space:pre}"
"a{color:#38bdf8}"
"</style></head><body>"
"<h1>คาริเบรต IR — กดเลือกโมดูล แล้วดูว่าจับยาตอนไหน</h1>"
"<div class='panel'>"
"<div>เลือกโมดูลที่จะทดสอบ (servo จะหมุน 1 รอบ ~6 วินาที):</div>"
"<div style='margin-top:8px'>"
"<button class='mod' onclick='runCal(1)'>โมดูล 1</button>"
"<button class='mod' onclick='runCal(2)'>โมดูล 2</button>"
"<button class='mod' onclick='runCal(3)'>โมดูล 3</button>"
"<button class='mod' onclick='runCal(4)'>โมดูล 4</button>"
"<button class='mod' onclick='runCal(5)'>โมดูล 5</button>"
"<button class='mod' onclick='runCal(6)'>โมดูล 6</button>"
"</div>"
"<div id='status' class='status' style='margin-top:10px'>พร้อมทดสอบ</div>"
"</div>"
"<div class='panel'>"
"<div><b>Timeline (เวลา 0..5500 ms):</b></div>"
"<div class='chart'><canvas id='chart' width='1000' height='140'></canvas></div>"
"<div class='legend'><div><span class='bar-high'></span>คานสะอาด</div>"
"<div><span class='bar-low'></span>คานบัง (ยาผ่าน) ← pulse</div></div>"
"<div id='summary' class='summary'>—</div>"
"</div>"
"<div class='panel'><a href='/tech'>← กลับหน้า Tech</a></div>"
"<script>"
"const c=document.getElementById('chart');const ctx=c.getContext('2d');"
"function draw(samples){"
"  const W=c.width,H=c.height;ctx.clearRect(0,0,W,H);ctx.fillStyle='#fff';ctx.fillRect(0,0,W,H);"
"  if(!samples.length)return;"
"  const T=5500;"
"  for(const s of samples){"
"    const t=s[0],low=s[1];"
"    const x=(t/T)*W;"
"    if(low){ctx.fillStyle='#ef4444';ctx.fillRect(x-1,0,2,H);}"
"  }"
"  ctx.strokeStyle='#94a3b8';ctx.lineWidth=1;"
"  for(let i=0;i<=11;i++){const x=(i*500/T)*W;ctx.beginPath();ctx.moveTo(x,H-12);ctx.lineTo(x,H);ctx.stroke();ctx.fillStyle='#475569';ctx.font='10px monospace';ctx.fillText((i*500)+'',x+2,H-2);}"
"  ctx.strokeStyle='#0ea5e9';ctx.beginPath();ctx.moveTo((1500/T)*W,0);ctx.lineTo((1500/T)*W,H);ctx.stroke();"
"  ctx.fillStyle='#0ea5e9';ctx.font='11px sans-serif';ctx.fillText('servo home @1500ms',(1500/T)*W+4,12);"
"}"
"async function runCal(m){"
"  document.getElementById('status').textContent='กำลังจ่าย ม.'+m+' (รอ ~6 วินาที)…';"
"  document.querySelectorAll('button.mod').forEach(b=>b.disabled=true);"
"  try{"
"    const r=await fetch('/tech/ir_cal/run',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'m='+m});"
"    const j=await r.json();"
"    if(!j.ok){document.getElementById('status').textContent='ผิดพลาด: '+(j.error||'unknown');return;}"
"    draw(j.samples||[]);"
"    const pulses=j.pulses||0;"
"    const first=(j.samples||[]).find(s=>s[1]===1);"
"    document.getElementById('status').textContent='เสร็จ! โมดูล '+m+' ตรวจเจอ '+pulses+' pulse';"
"    document.getElementById('summary').textContent="
"      'samples: '+j.count+'\\n'+"
"      'pulses (pill events): '+pulses+'\\n'+"
"      'first pill at: '+(first?first[0]+' ms':'(ไม่เจอเลย)')+'\\n'+"
"      'IR module bit: P'+(m-1);"
"  }catch(e){"
"    document.getElementById('status').textContent='HTTP error: '+e;"
"  }finally{"
"    document.querySelectorAll('button.mod').forEach(b=>b.disabled=false);"
"  }"
"}"
"</script></body></html>";

esp_err_t tech_ir_cal_page_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_page_auth(req);
    if (auth != ESP_OK) return auth;
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    return httpd_resp_send(req, IR_CAL_PAGE, HTTPD_RESP_USE_STRLEN);
}

esp_err_t tech_ir_cal_run_handler(httpd_req_t *req)
{
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char body[32] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len < 0) return ESP_FAIL;
    body[len] = '\0';

    int m = 0;
    const char *p_m = strstr(body, "m=");
    if (p_m) m = atoi(p_m + 2);
    if (m < 1 || m > DISPENSER_MED_COUNT) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad_module\"}", HTTPD_RESP_USE_STRLEN);
    }

    const int MAX_SAMPLES = 1500;
    ir_cal_sample_t *samples = (ir_cal_sample_t *)malloc(MAX_SAMPLES * sizeof(ir_cal_sample_t));
    if (!samples) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
    }

    int count = 0;
    esp_err_t rc = dispenser_ir_calibrate(m - 1, samples, MAX_SAMPLES, &count);
    if (rc != ESP_OK) {
        free(samples);
        httpd_resp_set_type(req, "application/json");
        char err[64];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(rc));
        return httpd_resp_send(req, err, HTTPD_RESP_USE_STRLEN);
    }

    int pulses = 0;
    uint8_t prev = 1;
    for (int i = 0; i < count; ++i) {
        uint8_t cur = samples[i].bit_low;
        if (cur && !prev) pulses++;
        prev = cur;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    char buf[80];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":true,\"m\":%d,\"count\":%d,\"pulses\":%d,\"samples\":[",
                     m, count, pulses);
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
        free(samples);
        return ESP_FAIL;
    }
    for (int i = 0; i < count; ++i) {
        n = snprintf(buf, sizeof(buf), "%s[%lu,%u,%u]",
                     i ? "," : "",
                     (unsigned long)samples[i].time_ms,
                     samples[i].bit_low,
                     samples[i].raw_byte);
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            // Client disconnected — don't keep generating chunks.
            free(samples);
            return ESP_FAIL;
        }
    }
    (void)httpd_resp_send_chunk(req, "]}", 2);
    (void)httpd_resp_send_chunk(req, NULL, 0);

    free(samples);
    return ESP_OK;
}
