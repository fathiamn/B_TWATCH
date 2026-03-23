# Monterro: Hiking Tour Assistant

**ELEC-E8408 Embedded Systems Development · Aalto University · 2026**  
*Bintang Setya, Fathia Nugraha*

---

Monterro is a hiking tour assistant built around the LilyGo T-Watch 2020 V2. It tracks your steps, distance, duration, and calories in real time directly on your wrist, then synchronises the data to a Raspberry Pi over Bluetooth or WiFi. A live web dashboard displays everything as you hike.

The project was developed as part of the ELEC-E8408 Embedded Systems Development group project at Aalto University, commissioned in the context of a Helsinki City Council brief for Finnish hiking tourism.

---

## How It Works

When you start a session on the watch, FreeRTOS runs four concurrent tasks: one reads the BMA423 accelerometer and converts steps to distance, one drives the LVGL display, one sends live data to the Raspberry Pi over Bluetooth (BLE), and one sends the same data over WiFi as a fallback. On the Raspberry Pi, an Apache server with PHP scripts receives the WiFi data and writes it to a MySQL database, while a Python server handles the Bluetooth connection. Both paths broadcast to Supabase Realtime so the web dashboard updates live without a page refresh.

```
T-Watch (ESP32)
  ├── BLE  ──────────────► server.py ──► Supabase Realtime ──► Dashboard
  └── WiFi POST ──────────► post-live.php ──► MySQL
                                          └──► Supabase Realtime ──► Dashboard
                                          └──► Supabase DB (live_snapshot)
```

When you press STOP, the watch sends the completed session via BLE and WiFi. The Raspberry Pi saves it to MySQL and Supabase, and the dashboard shows the final summary.

---


## Getting Started

### 1.1 Hardware Requirements

- LilyGo T-Watch 2020 V2
- Raspberry Pi 400 with power adapter, SD card, and HDMI cable
- USB cable (USB-A to micro-USB)
- WiFi router: ensure both devices are connected to the same local network

---

### 1.2 Watch Firmware Setup

**Step 1**: Install [Visual Studio Code](https://code.visualstudio.com/) along with the PlatformIO extension.

**Step 2**: Clone the repository:

```bash
git clone https://github.com/fathiamn/TTGO_TWATCH2
```

**Step 3**: Open the `TTGO_TWATCH2` folder in Visual Studio Code with PlatformIO.

**Step 4**: Open `platformio.ini` and set the upload port for your Raspberry Pi:

```ini
upload_port = /dev/ttyACM1
upload_speed = 115200
```

> The port may be `/dev/ttyACM0` or `/dev/ttyACM1` depending on which USB port you are using. Check with `ls /dev/ttyACM*` on the Raspberry Pi terminal.

**Step 5**: In `src/hardware/wifictl.cpp`, set your WiFi credentials and Raspberry Pi IP address:

```cpp
const char* WIFI_SSID = "your_wifi";
const char* WIFI_PASS = "your_password";
const char* RPI_URL   = "http://192.168.x.x:5000/update";
```

**Step 6**: Hold the watch's crown button with the USB plugged in, then build and upload:

```bash
pio run --target upload
```

---

### 1.3 Raspberry Pi Setup

**Step 1**: Install Python dependencies:

```bash
pip install flask flask-cors bleak realtime zeroconf --break-system-packages
```

**Step 2**: Install Apache, PHP, and MySQL:

```bash
sudo apt install apache2 mysql-server php php-mysql php-curl -y
```

**Step 3**: Set up the database:

```bash
sudo mysql < setup_db.sql
```

**Step 4**: Deploy the PHP files:

```bash
sudo mkdir -p /var/www/html/monterro
sudo cp post-live.php post-end.php get-current.php /var/www/html/monterro/
```

**Step 5**: Run the server:

```bash
python3 server.py
```

---

### 1.4 Supabase Setup

**Step 1**: Create an account or log in at [supabase.com](https://supabase.com).

**Step 2**: Create a new project. From **Settings → API**, copy your project's public URL and anon key.

**Step 3**: Create the required tables by running the following in the **SQL Editor**:

```sql
create table live_snapshot (
  id         int primary key default 1,
  steps      int default 0,
  distance   int default 0,
  duration   int default 0,
  calories   int default 0,
  updated_at timestamptz default now()
);

create table session_history (
  id         bigserial primary key,
  steps      int,
  distance   int,
  duration   int,
  calories   int,
  ended_at   timestamptz default now()
);

insert into live_snapshot (id) values (1);
```

**Step 4**: Enable Row Level Security by running in the SQL Editor:

```sql
alter table live_snapshot   enable row level security;
alter table session_history enable row level security;

create policy "public_all_live_snapshot"
  on live_snapshot for all
  using (true) with check (true);

create policy "public_all_session_history"
  on session_history for all
  using (true) with check (true);
```

Then enable Realtime for both tables:

```sql
alter publication supabase_realtime add table live_snapshot;
alter publication supabase_realtime add table session_history;
```



---

## Database Structure

### MySQL on Raspberry Pi

**`live_data`**: inserted every 5 seconds during an active session via `post-live.php`:

| Column | Type | Description |
|---|---|---|
| id | INT UNSIGNED AUTO_INCREMENT | Primary key |
| steps | INT UNSIGNED | Current step count |
| distance | INT UNSIGNED | Distance in metres |
| duration | INT UNSIGNED | Session time in seconds |
| calories | INT UNSIGNED | Estimated calories: steps × 4 ÷ 100 |
| source | VARCHAR(8) | `'wifi'` or `'ble'` |
| received_at | TIMESTAMP | Row insertion time |

**`sessions`**: one row per completed hike, inserted by `post-end.php` when STOP is pressed:

| Column | Type | Description |
|---|---|---|
| id | INT UNSIGNED AUTO_INCREMENT | Primary key |
| steps | INT UNSIGNED | Total steps for the session |
| distance | INT UNSIGNED | Total distance in metres |
| duration | INT UNSIGNED | Total duration in seconds |
| calories | INT UNSIGNED | Total calories burned |
| ended_at | TIMESTAMP | Session end time |

### Supabase DB

**`live_snapshot`**: single row (id=1), upserted on every WiFi tick by `post-live.php`. The dashboard reads this on page load because it is hosted on Vercel (HTTPS) and cannot fetch from the Raspberry Pi (HTTP) directly due to mixed content restrictions:

| Column | Type | Description |
|---|---|---|
| id | INT (fixed = 1) | Single row, always overwritten |
| steps | INT | Latest step count |
| distance | INT | Latest distance in metres |
| duration | INT | Latest session duration in seconds |
| calories | INT | Latest calories |
| updated_at | TIMESTAMPTZ | Last update time |

**`session_history`**: one row per completed session, inserted by `post-end.php`. Used by the dashboard to display past hike history:

| Column | Type | Description |
|---|---|---|
| id | BIGSERIAL | Primary key |
| steps | INT | Total steps |
| distance | INT | Total distance in metres |
| duration | INT | Total duration in seconds |
| calories | INT | Total calories |
| ended_at | TIMESTAMPTZ | Session end time |

### SPIFFS on Watch

**`/hike_log.jsonl`**: one JSON line appended per session to the watch's internal flash memory, regardless of network connectivity:

| Field | Description |
|---|---|
| ts | Unix timestamp of session end |
| stp | Total steps |
| dst | Distance in metres |
| dur | Duration in seconds |
| kcal | Calories burned |

---

## Communication Architecture

The system uses two parallel data channels:

**Bluetooth Low Energy (primary)**: the watch connects to the Raspberry Pi using `server.py` and the Bleak library, subscribing to the Nordic UART Service (NUS) TX characteristic (`6e400003-b5a3-f393-e0a9-e50e24dcca9e`). JSON messages are sent as newline-terminated strings.

Live message format:
```json
{"type": "live", "steps": 1234, "distance": 617, "duration": 480}
```

Session end message format:
```json
{"type": "end", "steps": 1234, "distance": 617, "duration": 480, "history": [...]}
```

**WiFi HTTP POST (bonus / fallback)**: the watch sends form-encoded POST requests to Apache on the Raspberry Pi every 5 seconds. An API key (`monterro2026`) is included with every request for basic authentication. The WiFi channel activates automatically when BLE is disconnected, ensuring data continues to reach the Raspberry Pi regardless of proximity.

- Live endpoint: `POST /monterro/post-live.php`
- Session end endpoint: `POST /monterro/post-end.php`

---

## Known Limitations

- The watch may reboot when BLE disconnects unexpectedly. Session data is preserved in SPIFFS on the watch so nothing is lost, but the reboot itself is a known issue to be addressed in future firmware versions.
- Calories are estimated using a simplified formula (`steps × 4 ÷ 100`) and do not account for user weight, height, or terrain.
- The `live_data` MySQL table grows indefinitely: older rows are kept for debugging but never read by the dashboard. A cleanup routine should be added for long-term use.
- Live dashboard updates require an internet connection to Supabase. If the network has no internet access, the dashboard will still show the last known state from `live_snapshot` on page load.

---

## License

This project was developed for academic purposes at Aalto University. Hardware was provided by the ELEC-E8408 course and must be returned after the project period.
