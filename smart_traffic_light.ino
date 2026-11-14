#include <WiFi.h>
#include <WebServer.h>

// ===== Wi-Fi settings =====
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

WebServer server(80);

// ===== Traffic configuration =====
const int LANES = 4;

// Lanes with fewer waiting cars than this are skipped.
const unsigned int MIN_CARS_FOR_GREEN = 1;

// If every lane is empty: true = keep rotating, false = wait in all-red.
const bool ROTATE_WHEN_EMPTY = false;

// Adaptive green time: MIN + cars * PER_CAR, capped at MAX.
const unsigned long MIN_GREEN_MS = 3000;
const unsigned long PER_CAR_MS   = 1000;
const unsigned long MAX_GREEN_MS = 10000;

const unsigned long YELLOW_MS   = 2000;
const unsigned long ALL_RED_MS  = 2000;
const unsigned long DEBOUNCE_MS = 80;

// Starvation protection: each second of waiting adds this to a lane's score.
// A car-count is worth 1000, so 250 means 4 s of waiting ~ 1 extra car.
const unsigned long WAIT_SCORE_PER_SEC = 250;

// ===== LED GPIOs =====
const int redPins[LANES]    = {4, 18, 26, 12};
const int yellowPins[LANES] = {16, 19, 33, 14};
const int greenPins[LANES]  = {17, 21, 32, 27};

// ===== IR sensor GPIOs =====
// GPIO 35 is input-only and has no internal pull-up (fine: IR module drives it).
const int irPins[LANES] = {15, 22, 35, 13};

// Most digital IR obstacle sensors are active LOW.
const int DETECTED_LEVEL = LOW;

// ===== Traffic state =====
enum Phase { ALL_RED, GREEN, YELLOW };

Phase phase = ALL_RED;
int currentLane = -1;
int nextLane = 0;
unsigned long phaseStarted = 0;
unsigned long greenDuration = MIN_GREEN_MS;

// ===== Vehicle counting =====
unsigned int vehicleCount[LANES] = {0, 0, 0, 0};
unsigned long waitingSince[LANES] = {0, 0, 0, 0};  // 0 = not waiting

int lastRaw[LANES];
int stableReading[LANES];
unsigned long rawChangedAt[LANES];

// ===== Helper functions =====
const char* phaseName() {
  if (phase == GREEN) return "GREEN";
  if (phase == YELLOW) return "YELLOW";
  return "ALL RED";
}

bool laneHasRightOfWay(int lane) {
  return (phase == GREEN || phase == YELLOW) && lane == currentLane;
}

// Cars waiting in a lane: counted arrivals, or at least 1 if the sensor
// is currently blocked (a car sitting still in front of the sensor).
unsigned int waitingCars(int lane) {
  if (laneHasRightOfWay(lane)) return 0;

  unsigned int c = vehicleCount[lane];
  if (c == 0 && stableReading[lane] == DETECTED_LEVEL) c = 1;
  return c;
}

void allRed() {
  for (int i = 0; i < LANES; i++) {
    digitalWrite(redPins[i], HIGH);
    digitalWrite(yellowPins[i], LOW);
    digitalWrite(greenPins[i], LOW);
  }
}

void beginAllRed() {
  allRed();
  phase = ALL_RED;
  phaseStarted = millis();
  Serial.println("All lanes RED");
}

void beginGreen(int lane) {
  allRed();

  // Work out green time from the queue BEFORE clearing the count.
  unsigned long cars = waitingCars(lane);
  greenDuration = MIN_GREEN_MS + cars * PER_CAR_MS;
  if (greenDuration > MAX_GREEN_MS) greenDuration = MAX_GREEN_MS;

  currentLane = lane;
  vehicleCount[lane] = 0;
  waitingSince[lane] = 0;

  digitalWrite(redPins[lane], LOW);
  digitalWrite(greenPins[lane], HIGH);

  phase = GREEN;
  phaseStarted = millis();
  nextLane = (lane + 1) % LANES;

  Serial.printf("Lane %d GREEN for %lu ms (%lu cars)\n",
                lane + 1, greenDuration, cars);
}

void beginYellow() {
  if (currentLane >= 0) {
    digitalWrite(greenPins[currentLane], LOW);
    digitalWrite(yellowPins[currentLane], HIGH);
  }

  phase = YELLOW;
  phaseStarted = millis();

  Serial.printf("Lane %d YELLOW\n", currentLane + 1);
}

// Read and debounce all IR sensors.
// Count new arrivals in every lane except the one that currently has green/yellow.
void updateSensors() {
  unsigned long now = millis();

  for (int i = 0; i < LANES; i++) {
    int raw = digitalRead(irPins[i]);

    if (raw != lastRaw[i]) {
      lastRaw[i] = raw;
      rawChangedAt[i] = now;
    }

    if (now - rawChangedAt[i] >= DEBOUNCE_MS &&
        stableReading[i] != lastRaw[i]) {

      stableReading[i] = lastRaw[i];

      if (stableReading[i] == DETECTED_LEVEL && !laneHasRightOfWay(i)) {
        vehicleCount[i]++;
        Serial.printf("Vehicle detected in lane %d; count = %u\n",
                      i + 1, vehicleCount[i]);
      }
    }
  }

  // Track how long each lane has been waiting (for starvation protection).
  for (int i = 0; i < LANES; i++) {
    if (waitingCars(i) > 0) {
      if (waitingSince[i] == 0) waitingSince[i] = now ? now : 1;
    } else {
      waitingSince[i] = 0;
    }
  }
}

// Score = busy lanes first, plus a bonus for lanes that have waited long.
unsigned long laneScore(int lane) {
  unsigned long score = (unsigned long)waitingCars(lane) * 1000UL;
  if (waitingSince[lane] != 0) {
    score += ((millis() - waitingSince[lane]) / 1000UL) * WAIT_SCORE_PER_SEC;
  }
  return score;
}

// Returns the lane that should get green next, or -1 to stay in all-red.
int chooseLane() {
  int best = -1;
  unsigned long bestScore = 0;

  // Start at nextLane so ties are broken by rotation order.
  for (int offset = 0; offset < LANES; offset++) {
    int lane = (nextLane + offset) % LANES;

    if (waitingCars(lane) < MIN_CARS_FOR_GREEN) continue;

    unsigned long s = laneScore(lane);
    if (best < 0 || s > bestScore) {
      best = lane;
      bestScore = s;
    }
  }

  if (best >= 0) return best;
  return ROTATE_WHEN_EMPTY ? nextLane : -1;
}

// ===== Web dashboard =====
const char DASHBOARD_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Traffic Management</title>
<style>
body{font-family:Arial;margin:20px;background:#f3f5f7;color:#202733}
h1{font-size:24px}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
.card{background:white;padding:16px;border-radius:12px}
.dot{display:inline-block;width:13px;height:13px;border-radius:50%;margin-right:4px;background:#d8dde3}
.dot.red{background:#e53935}.dot.yellow{background:#fbc02d}.dot.green{background:#43a047}
</style>
</head>
<body>
<h1>Smart Traffic Management</h1>
<p>Controller: <b id="status">Connecting...</b></p>
<div class="grid" id="grid"></div>
<p>Current phase: <b id="phase">-</b></p>
<script>
async function refresh(){
  try{
    const r = await fetch('/data');
    const d = await r.json();
    document.getElementById('status').textContent = 'Online';
    document.getElementById('phase').textContent = d.phase;
    let h = '';
    d.lanes.forEach((l,i)=>{
      h += '<div class="card"><h2>Lane '+(i+1)+'</h2>'
        + '<p>Vehicles waiting: <b>'+l.waiting+'</b></p><p>'
        + '<span class="dot '+(l.light=="RED"?"red":"")+'"></span>RED '
        + '<span class="dot '+(l.light=="YELLOW"?"yellow":"")+'"></span>YELLOW '
        + '<span class="dot '+(l.light=="GREEN"?"green":"")+'"></span>GREEN'
        + '</p></div>';
    });
    document.getElementById('grid').innerHTML = h;
  }catch(e){
    document.getElementById('status').textContent = 'Offline';
  }
}
refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
)rawliteral";

const char* laneLight(int lane) {
  if (laneHasRightOfWay(lane)) return (phase == GREEN) ? "GREEN" : "YELLOW";
  return "RED";
}

void handleRoot() {
  server.send_P(200, "text/html", DASHBOARD_PAGE);
}

void handleData() {
  String json = "{\"phase\":\"";
  json += phaseName();
  json += "\",\"lanes\":[";

  for (int i = 0; i < LANES; i++) {
    if (i > 0) json += ",";
    json += "{\"waiting\":";
    json += String(waitingCars(i));
    json += ",\"light\":\"";
    json += laneLight(i);
    json += "\"}";
  }

  json += "]}";
  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < LANES; i++) {
    pinMode(redPins[i], OUTPUT);
    pinMode(yellowPins[i], OUTPUT);
    pinMode(greenPins[i], OUTPUT);

    pinMode(irPins[i], INPUT);   // no INPUT_PULLUP: GPIO 35 has none

    lastRaw[i] = digitalRead(irPins[i]);
    stableReading[i] = lastRaw[i];
    rawChangedAt[i] = millis();
  }

  allRed();
  phase = ALL_RED;
  phaseStarted = millis();

  // Wi-Fi: auto-reconnect, and the server starts regardless of connection state.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();

  Serial.print("Connecting to Wi-Fi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected");
    Serial.print("Dashboard address: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWi-Fi unavailable; traffic control active, will keep retrying");
  }
}

void loop() {
  server.handleClient();
  updateSensors();

  unsigned long elapsed = millis() - phaseStarted;

  switch (phase) {
    case ALL_RED:
      if (elapsed >= ALL_RED_MS) {
        int lane = chooseLane();
        if (lane >= 0) beginGreen(lane);
        // else: stay red and keep checking each loop
      }
      break;

    case GREEN:
      if (elapsed >= greenDuration) beginYellow();
      break;

    case YELLOW:
      if (elapsed >= YELLOW_MS) beginAllRed();
      break;
  }
}
