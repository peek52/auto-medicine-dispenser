# Unified Cam — Workflow Diagrams

ระบบจ่ายยาอัตโนมัติ ESP32-P4-Nano + กล้อง MIPI-CSI + จอสัมผัส + WiFi/MQTT/Telegram

> Diagrams ใช้ Mermaid syntax — render ได้ใน VS Code (พร้อม Markdown preview), GitHub, GitLab

---

## 1. Boot sequence

```mermaid
flowchart TD
    A[POWERON / Reset] --> B[Bootloader<br/>ESP-IDF v5.3.3 patched]
    B --> C[app_main]
    C --> D{Reset reason?}
    D -->|≥3 PANIC ติด| E[Ultra-safe mode<br/>skip ALL I2C peripherals]
    D -->|1 PANIC| F[Safe mode<br/>skip VL53]
    D -->|POWERON / SW| G[Normal boot]

    E --> H[NVS init + cloud_secrets_load]
    F --> H
    G --> H

    H --> I[i2c_manager_init<br/>SDA=7 SCL=8 @ 50 kHz]
    I --> J[i2c_unstick_gpio<br/>ปลด SDA ที่ค้าง]
    J --> K{ping peripherals}

    K -->|TCA9548A 0x70| K1[disable VL53 ถ้าไม่เจอ]
    K -->|PCA9685 0x40| K2[6-channel servo]
    K -->|PCF8574 0x20| K3[IR pill sensors]
    K -->|DS3231 0x68| K4[RTC]
    K -->|FT6336U 0x38| K5[touch chip-id<br/>fallback OK ถ้า ACK]

    K1 & K2 & K3 & K4 & K5 --> L[display_clock_init<br/>ST7796S SPI 40 MHz]
    L --> M[start_core_services]
    M --> M1[camera_init OV5647 MIPI-CSI]
    M --> M2[jpeg_encoder_init HW JPEG]
    M --> M3[start_webserver port 80]
    M --> M4[start_stream_server port 81]

    M1 & M2 & M3 & M4 --> N[net_stack_preinit]
    N --> O[wifi_sta_init<br/>ESP-Hosted SDIO]
    O --> P[Spawn tasks]

    P --> P1[clock_task — UI 30/67 Hz]
    P --> P2[dispenser_task — slot check 30s]
    P --> P3[telegram_task — poll 3s]
    P --> P4[netpie_task — MQTT]
    P --> P5[offline_sync_task — GSheets]
    P --> P6[i2c_watchdog_task — 15s]
    P --> P7[alive_tick — 60s]

    P1 & P2 & P3 & P4 & P5 & P6 & P7 --> Q[Keepalive loop<br/>monitor heap, panic recovery]
```

---

## 2. Dispense cycle timing (per pill, ~5.85 s)

```mermaid
gantt
    title One Dispense Cycle (~5.85 s)
    dateFormat  X
    axisFormat  %L ms

    section Servo
    go_work I2C cmd          :a1, 0, 10
    Servo at WORK position   :a2, after a1, 1490
    go_home I2C cmd          :a3, after a2, 10
    Servo returning HOME     :a4, after a3, 3990

    section IR polling
    Poll window 1 (1.5 s)    :crit, b1, 0, 1500
    Poll window 2 (4.0 s)    :crit, b2, 1500, 4000
    Settle pause (350 ms)    :b3, 5500, 350
    Final 8-sample jam check :crit, b4, 5850, 160
```

---

## 3. Manual dispense / return-all flow

```mermaid
flowchart TD
    Start([Trigger: Telegram /dispense or UI button]) --> Em{emergency_stop<br/>or quiet_hours?}
    Em -->|Yes| Reject[Reject + send reason]
    Em -->|No| Lock[Take s_dispense_mutex<br/>set busy=true]
    Lock --> Sound[Play track 32<br/>กำลังจ่าย…]
    Sound --> Init[Init counters<br/>actually_dropped=0<br/>streak=0]
    Init --> Loop{Loop iter<br/>i &lt; loops_cap?}

    Loop -->|Yes| W[T=0: pca9685_go_work]
    W --> P1[T=0..1500ms<br/>Poll IR every ~10ms]
    P1 --> H[T=1500: pca9685_go_home]
    H --> P2[T=1500..5500ms<br/>Poll IR ต่อ]
    P2 --> Settle[T=5500..5850ms<br/>Settle 350ms + 8 samples]
    Settle --> Result{IR saw LOW<br/>this cycle?}

    Result -->|Yes pill_detected| Inc[actually_dropped++<br/>shadow.count--<br/>streak=0]
    Result -->|No| Skip[streak++]

    Inc --> LowStock[Low-stock alert<br/>ถ้า remaining ≤ 2]
    LowStock --> Loop

    Skip --> Empty{streak ≥ 2?}
    Empty -->|No| Loop
    Empty -->|Yes| ForceEmpty[forced_empty=true<br/>shadow.count=0<br/>send refill alert]

    Loop -->|i = loops_cap| Done[Loop done]
    ForceEmpty --> Done

    Done --> Beep{actually_dropped == 0?}
    Beep -->|Yes| NoMeds[play 'ไม่พบยา']
    Beep -->|No, eject_all| Returned[play 'คืนยาแล้ว']
    Beep -->|No, partial| Disp[play 'จ่ายยาสำเร็จ']

    NoMeds & Returned & Disp --> Msg[Build short message]
    Msg --> Send["send_telegram_photo_or_text:<br/>โมดูล: M (name)<br/>เวลา: HH:MM<br/>จำนวน: N เม็ด"]
    Send --> GS[Google Sheets log]
    GS --> Unlock[Release mutex<br/>busy=false]
    Unlock --> End([Done])

    Reject --> End
```

---

## 4. Scheduled dispense (จ่ายตามตาราง)

```mermaid
flowchart TD
    Tick[dispenser_task tick<br/>ทุก 30s] --> ReadRTC[ds3231_get_time]
    ReadRTC --> Match{เวลาตรง<br/>shadow.slot_time<br/>±1 นาที?}
    Match -->|No| Tick
    Match -->|Yes| Enable{scheduleEnabled?}
    Enable -->|No| Tick
    Enable -->|Yes| Trig{slot trigger flag<br/>นาทีนี้แล้ว?}
    Trig -->|Yes| Tick
    Trig -->|No| Set[s_pending_slot_idx<br/>s_waiting_confirm=true]

    Set --> Alert[Telegram: ใกล้เวลากินยา<br/>+ play เสียงเตือน]
    Alert --> Wait[รอ user confirm]

    Wait --> Choice{User action<br/>หรือ timeout?}
    Choice -->|กดกินยา| Run[Loop med 1..6<br/>med slot bit set?<br/>→ manual dispense]
    Choice -->|กดเลื่อน| Snooze[Schedule retry +5 min]
    Choice -->|Timeout 15 min| Skip[Log 'Skipped Timeout'<br/>+ Telegram notify]

    Run --> Done[Done]
    Snooze --> Done
    Skip --> Done
    Done --> Clear[Clear pending<br/>set trigger flag<br/>นาทีนี้]
    Clear --> Tick
```

---

## 5. Touch input → UI state machine

```mermaid
stateDiagram-v2
    [*] --> Standby
    Standby --> Menu: tap

    Menu --> Settings: tap Settings
    Menu --> Meds: tap Meds
    Menu --> Schedule: tap Schedule
    Menu --> WiFi: tap WiFi
    Menu --> Cloud: tap Cloud
    Menu --> Tech: tap Tech
    Menu --> Standby: timeout / Back

    Settings --> Settings: adjust vol/lang/IR
    Settings --> Menu: Back

    Meds --> Keyboard: edit name
    Meds --> Meds: adjust count/slots
    Meds --> Menu: Back

    Schedule --> Schedule: edit slot time<br/>(popup4)
    Schedule --> Menu: Back

    WiFi --> Keyboard: enter SSID/password
    WiFi --> WiFi: scan / select
    WiFi --> Menu: Back

    Keyboard --> Caller: submit
    note right of Keyboard
        Thai/EN bitmap
        ส่งคืน source state
    end note

    Cloud --> Menu: Back
    Tech --> Menu: Back

    Standby --> Confirm: scheduled dose due
    Confirm --> Run: tap กิน
    Confirm --> Snooze: tap เลื่อน
    Confirm --> Standby: timeout 15 min
    Run --> Standby: dispense done
    Snooze --> Standby: scheduled +5 min
```

---

## 6. FT6336U touch read pipeline

```mermaid
flowchart LR
    A[clock_task tick] --> B{s_touch_initialized?}
    B -->|No| C[retry init ทุก 5s]
    C --> Out1[return false]

    B -->|Yes| D{cache valid?<br/>idle 33ms / pressed 15ms}
    D -->|Yes| E[return cached state]

    D -->|No| F[i2c_manager_read_reg<br/>0x02, 6 bytes]
    F --> G{I2C OK?}
    G -->|No| H[mark fail<br/>2s cooldown]
    H --> Out2[return false]

    G -->|Yes| I[parse touches=data&0xF<br/>x = data1..2<br/>y = data3..4]
    I --> J{touches in 1-2<br/>AND not 0,0?}
    J -->|No| K[raw_press=false<br/>release_streak++]
    J -->|Yes| L[raw_press=true<br/>press_streak++]

    K --> M{release_streak ≥ 2?}
    M -->|Yes| N[pressed=false]
    M -->|No, was pressed| O[stay pressed]

    L --> P{press_streak ≥ 2?}
    P -->|Yes| Q[pressed=true<br/>save x,y]
    P -->|No| R[wait next sample]

    N & O & Q & R --> Out3[return state]
```

---

## 7. Cloud architecture

```mermaid
graph TB
    subgraph Device [ESP32-P4-Nano]
        Main[main loop]
        Web[web_server :80<br/>stream :81]
        Tg[telegram_task]
        Mq[netpie_task]
        Of[offline_sync_task]
    end

    subgraph C6 [ESP32-C6 ESP-Hosted]
        WiFi[WiFi STA]
    end

    subgraph Internet
        TGAPI[Telegram Bot API]
        NETPIE[NETPIE MQTT broker<br/>mqtt.netpie.io:1883]
        GAS[Google Apps Script<br/>HTTPS endpoint]
        Browser[Browser<br/>คอม / มือถือ]
    end

    Main -.SDIO.- C6
    C6 --> WiFi
    WiFi --> TGAPI
    WiFi --> NETPIE
    WiFi --> GAS
    Browser --> Web

    TGAPI -.poll 3s.-> Tg
    NETPIE -.subscribe shadow.-> Mq
    Mq -.publish updates.-> NETPIE
    Of -.queued events.-> GAS
    Tg -.snapshot + msg.-> TGAPI

    Web -. HTTP cookie auth .-> Browser
```

---

## 8. NETPIE shadow sync

```mermaid
sequenceDiagram
    participant ESP as ESP32-P4
    participant MQ as NETPIE Broker
    participant App as Mobile App / Web

    Note over ESP,MQ: Connect (TLS 1883)
    ESP->>MQ: SUBSCRIBE @shadow/data/get/response
    ESP->>MQ: SUBSCRIBE @shadow/data/updated
    ESP->>MQ: PUBLISH @shadow/data/get
    MQ-->>ESP: shadow current state

    Note over ESP: cache → s_shadow.med[]<br/>slot_time[7], scheduleEnabled

    App->>MQ: PUBLISH @shadow/data/update<br/>(med count change)
    MQ-->>ESP: @shadow/data/updated event
    ESP->>ESP: refresh s_shadow

    Note over ESP: ทุกครั้งที่ dispense
    ESP->>MQ: PUBLISH @shadow/data/update<br/>(new count)
    MQ-->>App: shadow updated
```

---

## 9. I2C watchdog & recovery

```mermaid
stateDiagram-v2
    [*] --> Healthy

    Healthy --> Healthy: peripherals ping OK
    Healthy --> Probe: tick 15s

    Probe --> Healthy: all 3 of TCA/PCA/PCF respond
    Probe --> Miss1: 1+ missing
    Miss1 --> Healthy: next tick OK
    Miss1 --> Miss2: still missing

    Miss2 --> Miss3: still missing
    Miss3 --> Recover: 3 consecutive misses

    Recover --> RmDevs[Remove all dev handles]
    RmDevs --> DelBus[i2c_del_master_bus]
    DelBus --> Unstick[GPIO unstick SDA<br/>4 rounds × 32 pulses]
    Unstick --> NewBus[i2c_new_master_bus]
    NewBus --> ReProbe: retry probe

    ReProbe --> Healthy: peripheral back
    ReProbe --> Degraded: still missing<br/>log 'recovery failed'

    Degraded --> Probe: keep trying
    Degraded --> Restart: ≥2 panics in 10 min
    Restart --> [*]: esp_restart
```

---

## 10. Hardware connection diagram

```mermaid
flowchart LR
    subgraph P4 [ESP32-P4-Nano]
        SDA[SDA GPIO7]
        SCL[SCL GPIO8]
        XCLK[XCLK GPIO33<br/>LEDC 24MHz]
        SPI[SPI 40MHz<br/>32/36/26/24/25]
        UART[UART1 37/38]
        SDIO[SDIO 14-19]
    end

    subgraph I2C [I2C bus 50 kHz]
        TCA[TCA9548A 0x70<br/>I2C mux]
        PCA[PCA9685 0x40<br/>servo PWM]
        PCF[PCF8574 0x20<br/>IR sensors x6]
        DS[DS3231 0x68<br/>RTC]
        FT[FT6336U 0x38<br/>touch]
    end

    subgraph Peripherals
        VL[VL53L0X x6<br/>pill TOF]
        SERVO[Servo x6]
        IR[IR beam x6]
        LCD[ST7796S<br/>480×320]
        AUDIO[DY-HV20T<br/>audio]
        CAM[OV5647<br/>MIPI-CSI]
        C6[ESP32-C6<br/>ESP-Hosted WiFi]
    end

    SDA & SCL --> TCA & PCA & PCF & DS & FT
    TCA -.mux ch.-> VL
    PCA --> SERVO
    PCF --> IR
    FT --> LCD
    SPI --> LCD
    UART --> AUDIO
    XCLK --> CAM
    SDIO --> C6
```

---

## 11. Build / flash workflow

```mermaid
flowchart LR
    A[Edit code] --> B[idf.py -B build build]
    B --> C{Build OK?}
    C -->|Compile error| A
    C -->|OK| D[idf.py -B build -p COM6 flash]
    D --> E{Flash OK?}
    E -->|Could not open COM6| F[Wait, retry]
    F --> D
    E -->|chip stopped responding| G[Hold EN button<br/>retry flash]
    G --> D
    E -->|OK + Hard reset| H[Test on device]

    H --> I{Behavior OK?}
    I -->|No| J[Check serial logs<br/>via /logs/tail]
    J --> A
    I -->|No, panic| K[idf.py coredump-info]
    K --> A
    I -->|Yes| L[Commit + push]
```

---

## 12. Telegram command map

```mermaid
mindmap
  root((Telegram bot))
    Camera
      /photo
        snapshot + JPEG
    Dispense
      /dispense N M
        manual N pills from module M
      /return M
        return all from module M
    Status
      /status
        battery heap uptime IP
      /schedule
        today's slot times
    Settings
      /lang th
      /lang en
    Help
      /help
        command list
```

---

## 13. Logs / TAG quick reference

```mermaid
flowchart LR
    Boot[unified_cam] -.alive ticks 60s.-> Log[/logs/tail<br/>64 KB ring]
    I2C[i2c_mgr] -.bus init recovery.-> Log
    Disp[dispenser] -.Drop X/N events.-> Log
    Touch[FT6336U] -.touch state.-> Log
    Cam[camera_init / ov5647] -.frame timeout.-> Log
    WiFi[wifi_sta / RPC_WRAP / transport] -.connect events.-> Log
    Mqtt[netpie_mqtt] -.shadow sync.-> Log
    Tg[tg_poll / tg_text_wrk] -.bot commands.-> Log
    Log -.HTTP authenticated.-> User[Tech panel<br/>/tech → Logs tab]
```
