#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#include <Wire.h>
#include <LiquidCrystal_I2C.h> 
#include <SPI.h>
#include <SD.h>
#include <TinyGPS++.h>
#include <WebServer.h> 
#include <time.h> 

// ============================================================
// CONFIGURARE - Într-un sistem de producție, aceste credențiale
// ar fi stocate în NVS (Non-Volatile Storage) sau configurate
// printr-un portal WiFiManager, NU hardcodate în cod.
// ============================================================
const char* ssid = "edge 40 neo_2166";
const char* pass = "mada6666";
const char* expoToken = "ExponentPushToken[E3YV42BoVz6x0UdFAbIxsa]";

const int pinBuzzer = 25;
const int chipSelectSD = 5;
const int RX_GPS = 16, TX_GPS = 17;

LiquidCrystal_I2C lcd(0x27, 16, 2); 
TinyGPSPlus gps;
HardwareSerial SerialGPS(2); 
WebServer server(80); 

const int ADRESA_MPU = 0x68;
float baselineG = 1.00; 
float baselineX = 0.00; 
float baselineY = 0.00; 
float baselineZ = 0.00; 

float fortaG = 0.00;          
float netX = 0.00; 
float netY = 0.00; 
float netZ = 0.00; 

// ============================================================
// FILTRU MOVING AVERAGE - Reduce zgomotul senzorului și
// minimizează fals pozitivele. Fereastră de 5 eșantioane.
// ============================================================
const int FILTER_SIZE = 5;
float filterBuffer[FILTER_SIZE] = {0};
int filterIndex = 0;
bool filterFull = false;

float applyMovingAverage(float newValue) {
  filterBuffer[filterIndex] = newValue;
  filterIndex = (filterIndex + 1) % FILTER_SIZE;
  if (filterIndex == 0) filterFull = true;

  int count = filterFull ? FILTER_SIZE : filterIndex;
  float sum = 0;
  for (int i = 0; i < count; i++) {
    sum += filterBuffer[i];
  }
  return sum / count;
}

// ============================================================
// PRAGURI DE DETECȚIE
// - PRAG_ZGOMOT (0.03 G): Dead-zone pentru eliminarea
//   zgomotului electronic al senzorului MPU6050 (±2g range,
//   sensibilitate 16384 LSB/g → zgomot tipic ~0.01-0.02 G).
// - PRAG_SEISM (0.10 G): Prag experimental pentru detecție.
//   Într-un sistem real, ar fi calibrat cu date de la INFP
//   (Institutul Național de Fizica Pământului).
//   Referință: 0.10 G ≈ intensitate MMI IV-V.
// ============================================================
const float PRAG_ZGOMOT = 0.03;
const float PRAG_SEISM  = 0.10;

unsigned long ultimaNotificare = 0;
unsigned long timpAflareCutremur = 0; 
bool sdCardActiv = false;
unsigned long numarInregistrare = 0; // global, persistent între alertări

// ============================================================
// CITIRE ULTIMUL NR. DIN CSV - La boot, parcurge fișierul CSV
// pentru a continua numerotarea de unde s-a rămas, evitând
// resetarea la fiecare repornire a ESP32.
// ============================================================
unsigned long citesteUltimulNrDinCSV() {
  if (!SD.exists("/seism_complet.csv")) return 0;

  File f = SD.open("/seism_complet.csv", FILE_READ);
  if (!f) return 0;

  unsigned long ultimulNr = 0;
  String linie = "";

  // Parcurgem tot fișierul linie cu linie
  while (f.available()) {
    char c = f.read();
    if (c == '\n' || c == '\r') {
      if (linie.length() > 0) {
        // Extragem primul câmp (Nr.) - tot ce e înainte de ";"
        int separatorPos = linie.indexOf(';');
        if (separatorPos > 0) {
          String nrStr = linie.substring(0, separatorPos);
          unsigned long nr = nrStr.toInt();
          if (nr > ultimulNr) {
            ultimulNr = nr;
          }
        }
      }
      linie = "";
    } else {
      linie += c;
    }
  }

  // Procesăm și ultima linie (dacă fișierul nu se termină cu \n)
  if (linie.length() > 0) {
    int separatorPos = linie.indexOf(';');
    if (separatorPos > 0) {
      unsigned long nr = linie.substring(0, separatorPos).toInt();
      if (nr > ultimulNr) ultimulNr = nr;
    }
  }

  f.close();
  return ultimulNr;
}

// Variabile pentru a gestiona afișajul LCD fără a-l face să pâlpâie
bool alarmaActivaPeLCD = false;
float maxGPeDurataAlarmei = 0.00;

float ultimulSeismG = 0.00;
String oraUltimuluiSeism = "-";

// Pornim cu coordonate 0.0 ca să știm sigur pe partea de web dacă avem sau nu semnal real
String curLat = "0.000000";
String curLng = "0.000000";

// --- TIMING NON-BLOCANT ---
unsigned long ultimulCicluSenzor = 0;
const unsigned long INTERVAL_SENZOR = 50; // 50ms → ~20 citiri/secundă

unsigned long ultimulUpdateLCD = 0;
const unsigned long INTERVAL_LCD = 250; // actualizare LCD la 250ms

// --- CACHE LCD PENTRU OPTIMIZARE ---
String lcdLinie0Veche = "";
String lcdLinie1Veche = "";

// --- MEMORIE PENTRU GRAFICUL ISTORIC (extins la 10 puncte) ---
const int HISTORY_SIZE = 10;
float istoricG[HISTORY_SIZE];
String istoricTimp[HISTORY_SIZE];

String obtineOraSimpla() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "-";
  }
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

String obtineDataOraCompleta() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return "N/A";
  }
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%d-%m-%Y %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

// --- Funcție helper pentru actualizarea LCD-ului doar când se schimbă textul ---
void updateLCDLine(int row, String text) {
  // Pad la 16 caractere ca să ștergem restul rândului
  while (text.length() < 16) text += " ";
  text = text.substring(0, 16);

  if (row == 0 && text != lcdLinie0Veche) {
    lcd.setCursor(0, 0);
    lcd.print(text);
    lcdLinie0Veche = text;
  } else if (row == 1 && text != lcdLinie1Veche) {
    lcd.setCursor(0, 1);
    lcd.print(text);
    lcdLinie1Veche = text;
  }
}

// --- Funcție pentru trimiterea notificării push ---
void trimiteNotificarePush(float intensitate, bool gpsReal) {
  if (WiFi.status() != WL_CONNECTED) return;

  // WiFiClientSecure pe stivă - fără risc de memory leak
  WiFiClientSecure client;
  // NOTĂ SECURITATE: setInsecure() dezactivează verificarea certificatului,
  // nu criptarea (conexiunea rămâne TLS). Acceptabil pentru prototip;
  // în producție se utilizează setCACert() cu certificat Root CA.
  client.setInsecure();

  HTTPClient http;
  http.begin(client, "https://exp.host/--/api/v2/push/send");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000); // timeout de 5 secunde

  // Trimitem doar intensitatea în body — aplicația mobilă va face
  // reverse geocoding pe coordonate și va afișa adresa reală.
  String payload = "{\"to\":\"" + String(expoToken) + 
                   "\",\"title\":\"🚨 SEISM DETECTAT!\", " +
                   "\"body\":\"Intensitate: " + String(intensitate, 2) + "G\", " +
                   "\"data\":{\"lat\":\"" + curLat + "\",\"lng\":\"" + curLng + 
                   "\",\"gpsValid\":\"" + (gpsReal ? "true" : "false") + "\"}}";
  
  http.POST(payload);
  http.end();
}

void handleData() {
  String json = "{";
  json += "\"liveG\":" + String(fortaG, 2) + ",";
  json += "\"ultimG\":" + String(ultimulSeismG, 2) + ",";
  json += "\"ultimTimp\":\"" + oraUltimuluiSeism + "\",";
  json += "\"lat\":\"" + curLat + "\",";
  json += "\"lng\":\"" + curLng + "\",";
  
  // Array-uri dinamice pentru istoric
  json += "\"histG\":[";
  for (int i = 0; i < HISTORY_SIZE; i++) {
    json += String(istoricG[i], 2);
    if (i < HISTORY_SIZE - 1) json += ",";
  }
  json += "],";
  
  json += "\"histT\":[";
  for (int i = 0; i < HISTORY_SIZE; i++) {
    json += "\"" + istoricTimp[i] + "\"";
    if (i < HISTORY_SIZE - 1) json += ",";
  }
  json += "]";
  
  json += "}";
  server.send(200, "application/json", json);
}

void handleRoot() {
  String html = R"raw(
<!DOCTYPE html><html lang="ro"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<title>Detector Inteligent de Cutremure</title>
<style>
*, *::before, *::after { box-sizing: border-box; }
body { background-color: #0f172a; color: #cbd5e1; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; margin: 0; padding: 0; min-height: 100vh; display: flex; flex-direction: column; }
.dashboard { display: flex; flex-direction: column; flex-grow: 1; width: 100%; }
.header { background: #1e293b; border-bottom: 1px solid #334155; padding: 25px 40px; display: flex; align-items: center; justify-content: center; box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1); }
.header h2 { margin: 0; color: #f8fafc; font-size: 36px; font-weight: 700; letter-spacing: 1px; }
.status-row { display: flex; justify-content: space-between; align-items: center; padding: 20px 50px; background: #0f172a; font-size: 20px; font-weight: 500; border-bottom: 1px solid #1e293b; }
.status-badge { background: rgba(16, 185, 129, 0.15); color: #34d399; padding: 6px 16px; border-radius: 20px; font-weight: 700; font-size: 16px; border: 1px solid rgba(16, 185, 129, 0.3); letter-spacing: 1px; }
.btn-download { background: rgba(56, 189, 248, 0.15); color: #38bdf8; padding: 8px 18px; border-radius: 10px; font-weight: 700; font-size: 14px; border: 1px solid rgba(56, 189, 248, 0.3); cursor: pointer; text-decoration: none; transition: background 0.3s; }
.btn-download:hover { background: rgba(56, 189, 248, 0.3); }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(400px, 1fr)); gap: 40px; padding: 50px; flex-grow: 1; }
.card { background: #1e293b; padding: 40px; border-radius: 20px; border: 1px solid #334155; display: flex; flex-direction: column; justify-content: center; align-items: center; text-align: center; box-shadow: 0 10px 15px -3px rgba(0,0,0,0.2); }
.card.full { grid-column: 1 / -1; }
.card-title { font-size: 18px; text-transform: uppercase; letter-spacing: 2px; color: #94a3b8; margin: 0 0 15px 0; font-weight: 600; }
.val-live { font-size: 80px; font-weight: 800; color: #38bdf8; margin: 0; line-height: 1; transition: color 0.3s; }
.val-alert { font-size: 70px; font-weight: 800; color: #f87171; margin: 0; line-height: 1; }
.val-text { font-size: 30px; font-weight: 600; color: #f8fafc; margin: 0; }
.meta { font-size: 16px; color: #64748b; margin-top: 15px; font-weight: 500; }
.chart-container { width: 100%; height: 350px; margin-top: 20px; position: relative; padding: 10px;}
#map { height: 500px; width: 100%; border-radius: 15px; border: 2px solid #38bdf8; margin-top: 20px; }
.filter-badge { background: rgba(56, 189, 248, 0.15); color: #38bdf8; padding: 4px 12px; border-radius: 12px; font-size: 12px; margin-top: 10px; }
.event-counter { background: rgba(248, 113, 113, 0.2); color: #f87171; padding: 3px 10px; border-radius: 10px; font-size: 14px; font-weight: 700; margin-left: 10px; border: 1px solid rgba(248, 113, 113, 0.4); }
@media (max-width: 768px) {
.header h2 { font-size: 24px; }
.status-row { flex-direction: column; gap: 10px; text-align: center; padding: 15px; font-size: 16px;}
.grid { padding: 20px; grid-template-columns: 1fr; gap: 20px; }
.card { padding: 25px 15px; }
.val-live { font-size: 60px; }
.val-alert { font-size: 50px; }
.val-text { font-size: 22px; }
.chart-container { height: 250px; padding: 5px; }
#map { height: 300px; }
}
</style></head><body>
<div class="dashboard">
<div class="header"><h2>📊 Detector Inteligent de Cutremure</h2></div>
<div class="status-row">
<span>Sistem: <span class="status-badge">● ONLINE</span></span>
<a href="/download" class="btn-download">📥 Descarcă CSV</a>
<span id="ora-curenta">Se incarca ora...</span>
</div>
<div class="grid">
<div class="card">
<p class="card-title">Vibrație Curentă (3D)</p>
<p class="val-live" id="live-text"><span id="g-live">0.00</span> <span style="font-size:40px;">G</span></p>
<span class="filter-badge">Filtru: Moving Average (5 eșantioane)</span>
</div>
<div class="card full" style="border-color: rgba(248, 113, 113, 0.4); background: linear-gradient(145deg, rgba(248, 113, 113, 0.05) 0%, rgba(248, 113, 113, 0.1) 100%);">
<p class="card-title" style="color: #fca5a5;">⚠️ Ultima Alertă Majoră</p>
<p class="val-alert"><span id="ultim-g">0.00</span> <span style="font-size:35px; color: #fca5a5;">G</span></p>
<p class="meta">Înregistrat la: <span id="ultim-timp">-</span></p>
</div>
<div class="card full">
<p class="card-title">📍 Locație Activă (Live Map)</p>
<div id="map"></div>
</div>
<div class="card full" style="padding: 20px;">
<p class="card-title">💥 Ultimele 10 Vibratii Seismice</p>
<div class="chart-container" style="height:400px;">
<canvas id="historyChart"></canvas>
</div>
</div>
</div></div>
<script>
var map = L.map('map').setView([45.9432, 24.9668], 6);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png').addTo(map);
var marker = L.marker([0, 0]).bindPopup("Asteptare semnal GPS...");
var hartaCentrataInitial = false;

// === GRAFIC BAR — ultimele 10 cutremure cu data+ora ===
let allLabels = [];
let allValues = [];
let allTimes = [];  // ora completa pentru tooltip
let lastKnownEvent = '';

const ctx = document.getElementById('historyChart').getContext('2d');

function getBarColor(val) {
    if (val >= 0.30) return 'rgba(248, 113, 113, 0.9)';
    if (val >= 0.20) return 'rgba(251, 146, 60, 0.9)';
    if (val >= 0.15) return 'rgba(251, 191, 36, 0.9)';
    return 'rgba(56, 189, 248, 0.9)';
}

let gradientFill = ctx.createLinearGradient(0, 0, 0, 350);
gradientFill.addColorStop(0, 'rgba(56, 189, 248, 0.6)');
gradientFill.addColorStop(1, 'rgba(56, 189, 248, 0.0)');

const config = {
    type: 'line',
    data: {
        labels: [],
        datasets: [{
            label: 'Intensitate (G)',
            data: [],
            backgroundColor: gradientFill,
            borderColor: '#38bdf8',
            borderWidth: 3,
            fill: true,
            tension: 0.4,
            pointBackgroundColor: '#0f172a',
            pointBorderColor: '#38bdf8',
            pointBorderWidth: 2,
            pointRadius: 5,
            pointHoverRadius: 8
        }]
    },
    options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
            y: {
                beginAtZero: true,
                suggestedMax: 0.40,
                ticks: { color: '#94a3b8', font: { size: 13 },
                    callback: function(v) { return v.toFixed(2); }
                },
                grid: { color: '#1e293b', drawBorder: false }
            },
            x: {
                ticks: { color: '#cbd5e1', font: { size: 12, weight: 'bold' }, maxRotation: 0, minRotation: 0 },
                grid: { display: false }
            }
        },
        plugins: {
            legend: { display: false },
            tooltip: {
                titleFont: { size: 14 },
                bodyFont: { size: 14, weight: 'bold' },
                displayColors: false,
                callbacks: {
                    title: function(items) {
                        var i = items[0].dataIndex;
                        var offset = Math.max(0, allLabels.length - 10);
                        var t = allTimes[offset + i] || '';
                        return items[0].label + ' ' + t;
                    },
                    label: function(ctx) {
                        var v = ctx.parsed.y;
                        var cls = v >= 0.30 ? 'Puternic' : v >= 0.20 ? 'Moderat' : v >= 0.15 ? 'Slab+' : 'Slab';
                        return v.toFixed(2) + ' G — ' + cls;
                    }
                }
            }
        }
    }
};
const historyChart = new Chart(ctx, config);

function updateChartData() {
    var last10Labels = allLabels.slice(-10);
    var last10Values = allValues.slice(-10);
    historyChart.data.labels = last10Labels;
    historyChart.data.datasets[0].data = last10Values;
    historyChart.update();
}

// Incarca evenimentele din CSV la deschiderea paginii
function loadHistoryFromCSV() {
    fetch('/download')
        .then(r => r.text())
        .then(csv => {
            var lines = csv.split('\n');
            for (var i = 1; i < lines.length; i++) {
                var cols = lines[i].split(';');
                if (cols.length >= 4) {
                    var data = cols[1] || '';
                    var ora = cols[2] || '';
                    var dp = data.split('.');
                    var label = (dp.length >= 2 ? dp[0] + '.' + dp[1] : data);
                    var gVal = parseFloat(cols[3].replace(',', '.'));
                    if (!isNaN(gVal) && gVal > 0) {
                        allLabels.push(label.trim());
                        allValues.push(gVal);
                        allTimes.push(ora);
                    }
                }
            }
            if (allLabels.length > 0) {
                lastKnownEvent = allLabels[allLabels.length - 1];
            }
            updateChartData();
        })
        .catch(function() {});
}
loadHistoryFromCSV();

function updateDashboard() {
    fetch('/data')
        .then(response => response.json())
        .then(data => {
            document.getElementById('g-live').innerText = data.liveG.toFixed(2);
            document.getElementById('ultim-g').innerText = data.ultimG.toFixed(2);
            document.getElementById('ultim-timp').innerText = data.ultimTimp;

            // Detectam eveniment NOU comparand ultimTimp cu lastKnownEvent
            if (data.ultimTimp !== '-' && data.ultimTimp !== lastKnownEvent && data.ultimG > 0) {
                var now = new Date();
                var now2 = new Date();
                allLabels.push(now2.getDate().toString().padStart(2,'0') + '.' + (now2.getMonth()+1).toString().padStart(2,'0'));
                allTimes.push(data.ultimTimp);
                allValues.push(data.ultimG);
                lastKnownEvent = data.ultimTimp;
                updateChartData();
            }

            var lat = parseFloat(data.lat);
            var lng = parseFloat(data.lng);

            if (lat !== 0 && lng !== 0) {
                var pos = [lat, lng];
                if(!map.hasLayer(marker)) { marker.addTo(map); }
                marker.setLatLng(pos);
                if (!hartaCentrataInitial) {
                    map.setView(pos, 16);
                    hartaCentrataInitial = true;
                }
                if(data.liveG >= 0.10) {
                    document.getElementById('live-text').style.color = '#f87171';
                    marker.getPopup().setContent("<div style='text-align:center; color:#f87171;'><b>SEISM AICI!</b><br>Forta: " + data.liveG.toFixed(2) + " G</div>");
                    if (!marker.isPopupOpen()) marker.openPopup();
                } else {
                    document.getElementById('live-text').style.color = '#38bdf8';
                    marker.getPopup().setContent("<b>Senzor Activ</b><br>Lat: " + lat + "<br>Lng: " + lng);
                }
            }
        });
}
setInterval(updateDashboard, 2000);

function updateTime() {
    var now = new Date();
    document.getElementById('ora-curenta').innerText = now.toLocaleString('ro-RO');
}
updateTime();
setInterval(updateTime, 1000);
</script>
</body></html>
)raw";

  server.send(200, "text/html", html);
}

void setup() {
  Serial.begin(115200);
  SerialGPS.begin(9600, SERIAL_8N1, RX_GPS, TX_GPS);
  Wire.begin(21, 22);
  
  pinMode(pinBuzzer, OUTPUT);
  digitalWrite(pinBuzzer, LOW);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(2, 0);
  lcd.print("Detector de");
  lcd.setCursor(3, 1);
  lcd.print("cutremure");
  delay(2500); 
  lcd.clear();

  WiFi.begin(ssid, pass);
  int incercari = 0;
  // Maxim 5s conectare; dacă WiFi lipsește, sistemul funcționează local
  while (WiFi.status() != WL_CONNECTED && incercari < 10) {
    delay(500);
    incercari++;
  }
  
  configTzTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org");

  if (SD.begin(chipSelectSD)) {
    sdCardActiv = true;
    // Citim ultimul Nr. din CSV pentru a continua numerotarea
    numarInregistrare = citesteUltimulNrDinCSV();
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  
  // Endpoint pentru descărcarea CSV-ului direct din browser
  server.on("/download", []() {
    if (sdCardActiv) {
      File dataFile = SD.open("/seism_complet.csv", FILE_READ);
      if (dataFile) {
        server.sendHeader("Content-Disposition", "attachment; filename=seism_complet.csv");
        server.streamFile(dataFile, "text/csv");
        dataFile.close();
        return;
      }
    }
    server.send(404, "text/plain", "Fisierul CSV nu exista pe cardul SD.");
  });
  
  server.begin();

  Wire.beginTransmission(ADRESA_MPU);
  Wire.write(0x6B); Wire.write(0); 
  Wire.endTransmission(true);
  delay(100);

  // Inițializare istorice
  for (int i = 0; i < HISTORY_SIZE; i++) {
    istoricG[i] = 0.00;
    istoricTimp[i] = "-";
  }

  lcd.print("Calibrare 3D...");
  float sumaG = 0, sumX = 0, sumY = 0, sumZ = 0;
  for(int i = 0; i < 20; i++) {
      Wire.beginTransmission(ADRESA_MPU);
      Wire.write(0x3B);
      Wire.endTransmission(false);
      Wire.requestFrom(ADRESA_MPU, 6, true);
      if (Wire.available() == 6) {
          int16_t ax = Wire.read()<<8|Wire.read();
          int16_t ay = Wire.read()<<8|Wire.read();
          int16_t az = Wire.read()<<8|Wire.read();
          float gx = ax / 16384.0;
          float gy = ay / 16384.0;
          float gz = az / 16384.0;
          
          sumaG += sqrt(gx*gx + gy*gy + gz*gz); 
          sumX += gx;
          sumY += gy;
          sumZ += gz;
      }
      delay(50);
  }
  baselineG = sumaG / 20.0; 
  baselineX = sumX / 20.0;
  baselineY = sumY / 20.0;
  baselineZ = sumZ / 20.0;
  lcd.clear();
}

void loop() {
  // Web server-ul rulează mereu, fără blocare
  server.handleClient();
  
  // Citire GPS continuă (non-blocant)
  while (SerialGPS.available() > 0) {
      char c = SerialGPS.read();
      gps.encode(c);
  }

  unsigned long acum = millis();

  // --- CITIRE SENZOR LA INTERVAL FIX (non-blocant) ---
  if (acum - ultimulCicluSenzor >= INTERVAL_SENZOR) {
    ultimulCicluSenzor = acum;

    Wire.beginTransmission(ADRESA_MPU);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom(ADRESA_MPU, 6, true);

    if (Wire.available() == 6) {
      int16_t ax = Wire.read()<<8|Wire.read();
      int16_t ay = Wire.read()<<8|Wire.read();
      int16_t az = Wire.read()<<8|Wire.read();
      
      float gx = ax / 16384.0;
      float gy = ay / 16384.0;
      float gz = az / 16384.0;
      
      netX = gx - baselineX;
      netY = gy - baselineY;
      netZ = gz - baselineZ;
      
      float fortaTotalaCurenta = sqrt(gx*gx + gy*gy + gz*gz);
      float fortaBruta = abs(fortaTotalaCurenta - baselineG);
      
      // Aplicăm filtrul Moving Average
      fortaG = applyMovingAverage(fortaBruta);
      
      if(fortaG < PRAG_ZGOMOT) fortaG = 0.00;
    }
  }

  // --- ACTUALIZARE GPS ---
  bool gpsReal = false;
  if (gps.location.isValid()) {
      curLat = String(gps.location.lat(), 6);
      curLng = String(gps.location.lng(), 6);
      gpsReal = true;
  }

  // --- LOGICA ALARMĂ ȘI NOTIFICĂRI ---
  if(fortaG >= PRAG_SEISM) {
      timpAflareCutremur = millis(); 
      
      if (fortaG > maxGPeDurataAlarmei) {
          maxGPeDurataAlarmei = fortaG;
      }

      if (!alarmaActivaPeLCD) {
          alarmaActivaPeLCD = true;
          lcdLinie0Veche = ""; // forțăm refresh
          lcdLinie1Veche = "";
      }

      updateLCDLine(0, "SEISM DETECTAT!");
      updateLCDLine(1, "Forta: " + String(maxGPeDurataAlarmei, 2) + " G");

      if(millis() - ultimaNotificare > 10000) {
          digitalWrite(pinBuzzer, HIGH);
          ultimulSeismG = maxGPeDurataAlarmei; 
          oraUltimuluiSeism = obtineOraSimpla(); 
          
          // Shift istoric
          for(int i = 0; i < HISTORY_SIZE - 1; i++) {
              istoricG[i] = istoricG[i+1];
              istoricTimp[i] = istoricTimp[i+1];
          }
          istoricG[HISTORY_SIZE - 1] = ultimulSeismG;
          istoricTimp[HISTORY_SIZE - 1] = oraUltimuluiSeism; 
          
          // Trimite notificare push cu locația GPS reală
          trimiteNotificarePush(maxGPeDurataAlarmei, gpsReal);

          if (sdCardActiv) {
              // La prima scriere după boot, creăm fișier nou cu header
              static bool headerScris = false;
              
              if (!headerScris) {
                  // Verificăm dacă fișierul există deja și are date
                  File checkFile = SD.open("/seism_complet.csv", FILE_READ);
                  bool fisierExistent = (checkFile && checkFile.size() > 0);
                  if (checkFile) checkFile.close();

                  if (!fisierExistent) {
                      // Fișier nou → scriem headerul
                      File newFile = SD.open("/seism_complet.csv", FILE_WRITE);
                      if (newFile) {
                          newFile.println("Nr.;Data;Ora;Total (G);Axa X (G);Axa Y (G);Axa Z (G);Latitudine;Longitudine;Sursa GPS;Clasificare");
                          newFile.close();
                      }
                  }
                  headerScris = true;
              }

              File dataFile = SD.open("/seism_complet.csv", FILE_APPEND);
              if (dataFile) {
                  // Clasificare automată a intensității
                  String clasificare;
                  if (maxGPeDurataAlarmei >= 0.80) {
                      clasificare = "Puternic";
                  } else if (maxGPeDurataAlarmei >= 0.40) {
                      clasificare = "Moderat";
                  } else {
                      clasificare = "Slab";
                  }

                  // Formatare valori pentru Excel RO (virgulă ca separator zecimal)
                  String fortaExcel = String(maxGPeDurataAlarmei, 2); fortaExcel.replace(".", ",");
                  String xExcel = String(netX, 2); xExcel.replace(".", ",");
                  String yExcel = String(netY, 2); yExcel.replace(".", ",");
                  String zExcel = String(netZ, 2); zExcel.replace(".", ",");

                  // Coordonate GPS
                  String latExcel = "0";
                  String lngExcel = "0";
                  if (gpsReal) {
                      float latVal = gps.location.lat();
                      float lngVal = gps.location.lng();
                      latExcel = String(latVal, 6); latExcel.replace(".", ",");
                      lngExcel = String(lngVal, 6); lngExcel.replace(".", ",");
                  }

                  numarInregistrare++;

                  // Scriere: Nr;Data;Ora;Total;X;Y;Z;Lat;Lng;Sursa;Clasificare
                  dataFile.print(numarInregistrare); dataFile.print(";");
                  
                  struct tm timeinfo;
                  if (getLocalTime(&timeinfo)) {
                      char dateBuf[12], timeBuf[10];
                      strftime(dateBuf, sizeof(dateBuf), "%d.%m.%Y", &timeinfo);
                      strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);
                      dataFile.print(dateBuf); dataFile.print(";");
                      dataFile.print(timeBuf); dataFile.print(";");
                  } else {
                      dataFile.print("N/A;N/A;");
                  }

                  dataFile.print(fortaExcel); dataFile.print(";");
                  dataFile.print(xExcel); dataFile.print(";");
                  dataFile.print(yExcel); dataFile.print(";");
                  dataFile.print(zExcel); dataFile.print(";");
                  dataFile.print(latExcel); dataFile.print(";");
                  dataFile.print(lngExcel); dataFile.print(";");
                  
                  if (gpsReal) {
                      dataFile.print("GPS Real");
                  } else {
                      dataFile.print("Fara Semnal");
                  }
                  dataFile.print(";");
                  dataFile.println(clasificare);
                  
                  dataFile.close();
              }
          }
          
          ultimaNotificare = millis();
          // Buzzer scurt non-blocant: îl oprim după 200ms în ciclul următor
          // (vezi mai jos)
      }
  }

  // --- Oprire buzzer după 200ms (non-blocant) ---
  if (digitalRead(pinBuzzer) == HIGH && millis() - ultimaNotificare >= 200) {
      digitalWrite(pinBuzzer, LOW);
  }

  // --- Reset afișaj alarmă după 4 secunde de liniște ---
  if (alarmaActivaPeLCD && (millis() - timpAflareCutremur > 4000)) {
      alarmaActivaPeLCD = false; 
      maxGPeDurataAlarmei = 0.00; 
      lcdLinie0Veche = "";
      lcdLinie1Veche = "";
      lcd.clear(); 
  }

  // --- AFIȘAREA NORMALĂ PENTRU LCD (la interval fix) ---
  if (!alarmaActivaPeLCD && (acum - ultimulUpdateLCD >= INTERVAL_LCD)) {
      ultimulUpdateLCD = acum;

      String linie0 = "G:" + String(fortaG, 2);
      if (gpsReal) {
          linie0 += "  GPS:ON";
      } else {
          linie0 += "  GPS:OFF";
      }

      String linie1;
      if (WiFi.status() == WL_CONNECTED) {
          linie1 = WiFi.localIP().toString();
      } else {
          linie1 = "WiFi: Cautare...";
      }

      updateLCDLine(0, linie0);
      updateLCDLine(1, linie1);
  }

  // NU MAI AVEM delay(50) — totul e non-blocant!
}