# เอกสารส่งต่อทีม: การ Refactor `main.py`

เอกสารนี้สรุปการแก้ไข Backend ของระบบ Biofloc บน Raspberry Pi 5 เพื่อให้ทีมสามารถตรวจสอบ นำไปติดตั้ง และพัฒนาต่อได้

## ไฟล์ที่แก้ไข

- `python/main.py` คือไฟล์ที่ refactor แล้ว
- ไฟล์ต้นฉบับที่ Desktop ไม่ถูกเขียนทับ
- ไม่มีการแก้ไข `fish_pond.ino` ในรอบนี้
- ไม่มีการเพิ่มหรือเปลี่ยน schema ตารางฐานข้อมูลในรอบนี้

## เป้าหมายของรอบนี้

1. ทำให้การเริ่มและหยุด FastAPI บน Raspberry Pi 5 เป็นระเบียบ
2. แยกข้อมูลจาก Arduino Mega ออกจากข้อมูล ESP-NOW Gateway
3. รองรับสถานะกากน้ำตาลและปูนขาวจาก Gateway เวอร์ชันใหม่
4. เตรียมโครงสร้างข้อมูลสำหรับหลายบ่อ โดยยังรักษาหน้าเว็บเดิม
5. ลดปัญหา Thread, Serial และ SQLite ทำงานชนกัน
6. ไม่เขียนระบบวาล์วหรือสูตรเติมสารที่ยังไม่มีข้อกำหนดฮาร์ดแวร์ชัดเจน

## การเปลี่ยนแปลงสำคัญ

### 1. FastAPI lifecycle

เดิม `port_manager` และ `node_watchdog` เริ่มทำงานทันทีตอน import ไฟล์ และใช้ `while True` ซึ่งหยุดอย่างเป็นระเบียบไม่ได้

เวอร์ชันใหม่ใช้ FastAPI `lifespan`:

- สร้างฐานข้อมูล/ตรวจ schema ก่อนเริ่มงานเบื้องหลัง
- เริ่ม Serial port manager, database worker และ node watchdog หลัง FastAPI พร้อม
- ใช้ `threading.Event` ส่งสัญญาณหยุด
- ปิด Serial ทุกพอร์ตก่อนโปรแกรมจบ
- ยกเลิก mock task และรอ worker หยุด

ช่วยลดปัญหา Thread ซ้ำเมื่อ Uvicorn reload หรือ service restart

### 2. Path ที่เหมาะกับ Raspberry Pi

Path ต่อไปนี้อ้างอิงจากโฟลเดอร์ของ `main.py` ไม่ได้ขึ้นกับ working directory ของ `systemd`:

- `.env`
- `fishpond.db`
- `init_db.sql`
- `dist`

สามารถ override ด้วย environment variable:

```env
DB_FILE=/absolute/path/fishpond.db
INIT_DB_FILE=/absolute/path/init_db.sql
DIST_DIR=/absolute/path/dist
```

### 3. ตรวจฐานข้อมูลก่อนเปิดระบบ

Backend ตรวจว่ามีตารางเดิมครบ:

- `users`
- `devices`
- `sensor_logs`
- `system_events`

หากไม่มีฐานข้อมูลและไม่มี `init_db.sql` ระบบจะหยุดพร้อมข้อความ error แทนการสร้างไฟล์ฐานข้อมูลว่างแล้วทำงานผิดภายหลัง

ไม่มีตารางใหม่ถูกเพิ่มในรอบนี้ตามข้อกำหนดของโครงการ

### 4. Database worker หนึ่งตัว

เดิมสร้าง Thread ใหม่ทุกครั้งที่ได้รับข้อมูลเซนเซอร์

เวอร์ชันใหม่ใช้ `Queue` และ database worker หนึ่งตัว ทำให้:

- จำกัดจำนวนงานค้าง
- ลด SQLite lock
- ไม่สร้าง Thread เพิ่มทุก 2 วินาที
- บันทึกข้อมูลได้แม้ไม่มีผู้ใช้เปิดหน้า WebSocket

### 5. การค้นหา Serial บน Pi 5

ลำดับการเลือกพอร์ต:

1. ใช้ `SERIAL_PORT` จาก `.env` หากกำหนดไว้
2. ใช้ `/dev/serial/by-id/*` ซึ่งคงที่หลัง reboot
3. fallback ไป `/dev/ttyACM*`, `/dev/ttyUSB*` หรืออุปกรณ์ที่มี USB VID

กำหนดหลายพอร์ตได้ด้วย comma:

```env
SERIAL_PORT=/dev/serial/by-id/device-mega,/dev/serial/by-id/device-gateway
```

### 6. แยกชนิดอุปกรณ์ Serial

ระบบเรียนรู้ชนิดบอร์ดจากข้อมูลที่ได้รับ:

- ข้อมูลที่มี `wat_t`, `ph`, `ec`, `level` ฯลฯ คือ `sensor`
- `action=node_status`, `action=weight` และ ESP-NOW acknowledgement คือ `gateway`

สามารถกำหนดเองได้ด้วย JSON ใน `.env`:

```env
SERIAL_ROLE_MAP={"/dev/serial/by-id/device-mega":"sensor","/dev/serial/by-id/device-gateway":"gateway"}
```

หากบอร์ดยังไม่ประกาศตัว ระบบจะ fallback ส่งแบบเดิมเพื่อรักษาความเข้ากันได้

ตรวจสถานะได้จาก:

```http
GET /api/serial/devices
```

### 7. Route คำสั่งไปยังบอร์ดที่ถูกต้อง

- `MUTE_ON` และ `MUTE_OFF` ส่งไป Arduino Mega (`sensor`)
- `scan`, `node_id`, `chemical`, `dispense` และ `tare` ส่งไป ESP-NOW Gateway
- ใช้ Lock ต่อพอร์ต ป้องกันคำสั่งจากหลาย WebSocket เขียน Serial ชนกัน

### 8. JSON เสียไม่ทำให้ Serial หลุด

หากได้รับ JSON ไม่ครบหนึ่งบรรทัด ระบบจะ log แล้วข้ามเฉพาะบรรทัดนั้น ไม่ปิดพอร์ตทั้งเส้น

### 9. รองรับสถานะสารสองชนิด

Gateway ส่งข้อมูลตัวอย่าง:

```json
{"action":"weight","node_id":1,"chemical":"lime","weight":850,"pump":false,"status":"idle","start_weight":0,"target_weight":0,"dispensed":0}
```

Backend จะไม่ส่งข้อมูลนี้เข้า `sensorData` หรือฐานข้อมูลเซนเซอร์บ่อ แต่ส่ง WebSocket ใหม่:

```json
{
  "type":"chemicalStatus",
  "nodeId":"1",
  "chemical":"lime",
  "weight":850,
  "pump":false,
  "status":"idle",
  "startWeight":0,
  "targetWeight":0,
  "dispensed":0
}
```

สถานะล่าสุดเรียกดูได้จาก:

```http
GET /api/chemicals/latest
```

### 10. เตรียมข้อมูลหลายบ่อ

WebSocket เดิมยังคงใช้รูปแบบ:

```json
{"type":"sensorData","data":{}}
```

หาก Arduino ส่ง `pond_id` หรือ `device_id` Backend จะเก็บค่าล่าสุดแยกตามบ่อ ไม่ให้ข้อมูลบ่อหนึ่งเขียนทับประวัติในหน่วยความจำของอีกบ่อ

เรียกดูได้จาก:

```http
GET /api/ponds/latest
```

ค่าเริ่มต้นรองรับสูงสุด 6 บ่อ และปรับได้ด้วย:

```env
MAX_PONDS=6
```

หมายเหตุ: `fish_pond.ino` ปัจจุบันยังไม่ได้ส่ง `pond_id` การแยกบ่อจริงจึงต้องเพิ่มที่ protocol ของชุดเก็บตัวอย่างในรอบถัดไป

### 11. รักษาความเข้ากันได้กับหน้าเว็บเดิม

สิ่งที่ยังคงเดิม:

- WebSocket path `/ws`
- `type=serverStatus`
- `type=sensorData`
- `type=nodeStatus`
- `/api/settings/mock`
- `/api/alerts`
- `/api/analytics`
- รูปแบบคำสั่ง `action=controlDevice`

Message และ API ที่เพิ่มเป็นแบบ additive หน้าเว็บเดิมสามารถเพิกเฉยได้จนกว่าจะเพิ่ม UI ใหม่

### 12. แก้ Node watchdog ใช้ฐานข้อมูลผิดไฟล์

เดิม watchdog เปิด `database.db` แต่ระบบหลักใช้ `fishpond.db`

เวอร์ชันใหม่ใช้ `DB_FILE` จุดเดียวกันทั้งระบบ

### 13. CORS และ Static file

- `CORS_ORIGINS` กำหนดผ่าน `.env` ได้
- เมื่อใช้ `*` จะไม่เปิด credential mode ที่ขัดกับ browser policy
- SPA fallback ตรวจว่าไฟล์อยู่ภายใน `dist` ก่อนส่ง ป้องกัน path traversal

ตัวอย่าง:

```env
CORS_ORIGINS=http://192.168.1.50:8000,http://biofloc.local
```

## สิ่งที่ตั้งใจยังไม่ทำ

รายการต่อไปนี้ยังไม่ควรเขียนจนกว่าฮาร์ดแวร์และ protocol จะชัดเจน:

1. State machine เปิดวาล์วตรวจน้ำ 6 บ่อ
2. คำสั่งควบคุม ESP32 Sampler
3. เกณฑ์ตรวจว่าค่าเซนเซอร์นิ่ง
4. การแปลงระดับ Ultrasonic เป็นลิตร
5. สูตรคำนวณปูนขาวจาก pH
6. สูตรคำนวณกากน้ำตาล
7. การคำนวณสารคงเหลือในบ่อพักกลาง
8. Auto sampling schedule
9. Auto dosing และ Auto distribution
10. ตารางฐานข้อมูล `ponds`, `sampling_jobs`, `dosing_jobs`, `system_faults`

เหตุผลคือระบบยังไม่ทราบรูปทรง/ปริมาตรบ่อ ไม่มี Flow meter และยังไม่มีสูตรสารที่ผ่านการทดสอบจริง การใส่ค่าคาดเดาลงในระบบอัตโนมัติมีความเสี่ยงต่อปลา

## ข้อกำหนดก่อนเปิดการเติมสารอัตโนมัติ

- กำหนดปริมาตรหรือ calibration table ของทุกบ่อ
- กำหนด pH เป้าหมายและ dose limit ต่อรอบ/ต่อวัน
- ทดสอบปริมาณปูนขาวจริงทีละน้อยในแต่ละบ่อ
- กำหนดวิธีจัดการน้ำผสมที่เหลือในบ่อพักหลัง Pi restart
- เพิ่ม timeout, emergency stop และ local failsafe ใน ESP32
- แยกกากน้ำตาลกับปูนขาว ไม่เติมพร้อมกันในหนึ่งรอบ

## Environment variables ที่รองรับ

```env
BAUD_RATE=115200
TARGET_DEVICE_ID=UNO_001
MAX_PONDS=6
DB_FILE=/absolute/path/fishpond.db
INIT_DB_FILE=/absolute/path/init_db.sql
DIST_DIR=/absolute/path/dist
SERIAL_PORT=/dev/serial/by-id/device-mega,/dev/serial/by-id/device-gateway
SERIAL_ROLE_MAP={"/dev/serial/by-id/device-mega":"sensor","/dev/serial/by-id/device-gateway":"gateway"}
LINE_ACCESS_TOKEN=...
DEFAULT_LINE_USER_ID=...
DEFAULT_ADMIN_PASSWORD_HASH=...
NODE_OFFLINE_UI_SECONDS=25
NODE_OFFLINE_LINE_SECONDS=86400
CORS_ORIGINS=http://biofloc.local
```

## การตรวจสอบที่ทำแล้ว

- Python AST parse ผ่าน
- `py_compile` ผ่าน
- ตรวจว่า background threads ไม่เริ่มตอน import module
- ตรวจเส้นทางแยก `sensorData`, `nodeStatus` และ `chemicalStatus`

ยังไม่ได้ทดสอบกับ USB/Serial จริง, ฐานข้อมูลจริง หรือ Uvicorn บน Raspberry Pi 5 ในเครื่องนี้ ต้องทดสอบบน Pi ก่อนแทนไฟล์ production

## ขั้นตอนทดสอบบน Raspberry Pi 5

1. สำรอง `main.py`, `.env` และ `fishpond.db`
2. วาง `main.py` ใหม่ในโฟลเดอร์เดิม
3. ตรวจว่ามี `fishpond.db` ที่สมบูรณ์ หรือมี `init_db.sql`
4. ตรวจว่า user ที่รัน service อยู่ในกลุ่ม `dialout`
5. ตั้ง `SERIAL_PORT` และ `SERIAL_ROLE_MAP` ด้วย path จาก `/dev/serial/by-id/`
6. เริ่ม Uvicorn แบบ worker เดียว เพราะ Serial hardware ควรมี process เจ้าของเพียงหนึ่ง process
7. เปิด `GET /api/serial/devices` และตรวจว่า Mega เป็น `sensor`, Gateway เป็น `gateway`
8. ตรวจว่า `sensorData` เดิมยังขึ้นหน้าเว็บ
9. ส่ง `scan` และตรวจ `nodeStatus`
10. สั่งอ่าน/จ่ายสารปริมาณทดสอบโดยไม่ต่อปั๊มจริง แล้วตรวจ `chemicalStatus`
11. หยุด service และตรวจว่า Serial ถูกปิด จากนั้น start ใหม่โดยไม่มีพอร์ตค้าง

## ข้อแนะนำการรัน Uvicorn

ใช้ worker เดียว:

```bash
uvicorn main:app --host 0.0.0.0 --port 8000 --workers 1
```

ไม่ควรใช้หลาย worker กับโปรแกรมนี้ เพราะแต่ละ process จะพยายามเปิด Serial port ชุดเดียวกัน
