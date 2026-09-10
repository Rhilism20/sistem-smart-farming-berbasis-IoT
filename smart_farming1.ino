#define DEBUG 1

/* ---- 1. KONFIGURASI BLYNK ---- */
#define BLYNK_TEMPLATE_ID "TMPL6BZ0f-G_r"
#define BLYNK_TEMPLATE_NAME "Smart Farming"
#define BLYNK_AUTH_TOKEN "7zdJ3azvCWMNVH7L_XY26IguBPNlqNo_"  
#if DEBUG
#define BLYNK_PRINT Serial      
#endif
/* ---- 2. LIBRARY ---- */
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <DHT.h>
#include <RTClib.h>
#include <time.h>
#include <HTTPClient.h>
#include <Preferences.h>

/* ---- 3. WIFI ---- */
char ssid[] = "BITVOLT";
char pass[] = "NoBugOnlyFeature";


#define JADWAL_PAGI_JAM     6
#define JADWAL_PAGI_MENIT   0
#define JADWAL_SORE_JAM    17
#define JADWAL_SORE_MENIT   0

/* ---- 4. PIN ---- */
#define PIN_DHT    25
#define PIN_RELAY  26
#define PIN_SDA    21
#define PIN_SCL    22
#define DHTTYPE    DHT22

#define CH_SOIL    1
#define CH_RAIN    0

#define RELAY_AKTIF_HIGH  1
#if RELAY_AKTIF_HIGH
  #define RELAY_ON   HIGH
  #define RELAY_OFF  LOW
#else
  #define RELAY_ON   LOW
  #define RELAY_OFF  HIGH
#endif

#define ADS_ALAMAT  0x48
#define ADS_GAIN    GAIN_TWOTHIRDS

/* ---- 5. INTERVAL ---- */
#define INTERVAL_BACA_SENSOR   3000L    
#define INTERVAL_LOGIKA        3000L
#define INTERVAL_NOTIF        60000L
#define INTERVAL_KONEKSI      15000L
#define INTERVAL_WATCHDOG      5000L
#define INTERVAL_SERIAL         200L
#define INTERVAL_DEBUG_JDW     5000L
#define INTERVAL_SYNC_NTP     (6UL  * 3600 * 1000)
#define INTERVAL_DETECT_TZ    (12UL * 3600 * 1000)

/* ---- 6. REM TRAFFIC BLYNK ---- */
#define KIRIM_MIN_MS      15000L         
#define KIRIM_PAKSA_MS   120000L        
/* ---- 7. ZONA WAKTU ---- */
#define TZ_DEFAULT_JAM   7
#define TZ_MIN_JAM       7
#define TZ_MAX_JAM       9
#define TZ_OFFSET_DETIK  (tzOffsetJam * 3600)

/* ---- 8. KALIBRASI TANAH  */
const int SOIL_KERING = 17000;
const int SOIL_BASAH  = 7000;
#define SOIL_RAW_MIN      1000           
#define SOIL_RAW_MAKS    26000

/* ---- 9. SENSOR HUJAN ---- */
#define RAIN_ON        11000
#define RAIN_OFF       13000
#define RAIN_BUTUH_N   3
#define RAIN_RAW_MIN     500             
#define RAIN_RAW_MAKS  26000

/* ---- 10. AMBANG PENYIRAMAN ---- */
#define AMBANG_KERING   40
#define SENSOR_NYALA    40               // mode 2: < ini -> pompa ON
#define SENSOR_MATI     55               // mode 2: > ini -> pompa OFF
#define SENSOR_TENGAH   ((SENSOR_NYALA + SENSOR_MATI) / 2)   // keputusan awal
#define NOTIF_GAP        8               // histeresis notifikasi tanah kering
#define SIRAM_CUKUP     40 

#define JENDELA_JADWAL_MENIT   5
const long DURASI_SIRAM_MS   = 10UL * 60 * 1000;
const long MAKS_POMPA_MS     = 20UL * 60 * 1000;

/* ---- 11. OBJEK ---- */
DHT dht(PIN_DHT, DHTTYPE);
Adafruit_ADS1115 ads;
RTC_DS3231 rtc;
BlynkTimer timer;
Preferences prefs;

/* ---- 12. KONFIG TERSIMPAN  */
int  tzOffsetJam = TZ_DEFAULT_JAM;
bool tzManual    = false;
char tzNama[32]  = "Asia/Jakarta";
char tzKota[32]  = "-";

/* ---- 13. JADWAL ---- */
int jadwalPagiJam   = JADWAL_PAGI_JAM;
int jadwalPagiMenit = JADWAL_PAGI_MENIT;
int jadwalSoreJam   = JADWAL_SORE_JAM;
int jadwalSoreMenit = JADWAL_SORE_MENIT;

/* ---- 14. STATUS RUNTIME ---- */
bool adsOK = false, rtcOK = false, dhtOK = false;
bool ntpPernahSukses = false, tzTerdeteksi = false;

int  mode = 3;
bool pompaNyala = false, manualOn = false, lockoutPompa = false;
unsigned long mulaiSiramMs = 0, pompaMulaiMs = 0;

int  kunciPagi = -1, kunciSore = -1;
bool keputusanSensorAda = false;         // FIX zona mati mode 2

int     kelembapanTanah = 0;
int16_t soilRaw = 0, rainRaw = 0;
bool    soilValid = false, rainValid = false;
float   suhuUdara = 0, kelembapanUdara = 0;
bool    sedangHujan = false;

#if DEBUG
  #define DBG(x)     Serial.print(x)
  #define DBGLN(x)   Serial.println(x)
  #define DBGF(...)  Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)
  #define DBGLN(x)
  #define DBGF(...)
#endif

/* ---- PROTOTYPE ---- */
bool cocokJadwal(int jamNow, int menitNow, int jamJadwal, int menitJadwal);
bool syncRTCdariNTP();
bool deteksiZonaWaktu();
void simpanKonfig();
void debugJadwal();

/* =====================================================================
   NVS - hanya zona waktu
   ===================================================================== */
void muatKonfig() {
  prefs.begin("smartfarm", false);
  tzOffsetJam = prefs.getInt ("tz",    TZ_DEFAULT_JAM);
  tzManual    = prefs.getBool("tzMan", false);
  prefs.getString("tzNama", tzNama, sizeof(tzNama));
  prefs.end();

  if (tzNama[0] == 0) strcpy(tzNama, "Asia/Jakarta");
  if (tzOffsetJam < TZ_MIN_JAM || tzOffsetJam > TZ_MAX_JAM) tzOffsetJam = TZ_DEFAULT_JAM;

  DBGF("[NVS] TZ=UTC+%d (%s) %s\n", tzOffsetJam, tzNama, tzManual ? "MANUAL" : "AUTO");
  DBGF("[FW]  Kalibrasi tanah: kering=%d basah=%d (bawaan)\n", SOIL_KERING, SOIL_BASAH);
  DBGF("[FW]  Jadwal: pagi %02d:%02d | sore %02d:%02d\n",
       jadwalPagiJam, jadwalPagiMenit, jadwalSoreJam, jadwalSoreMenit);
}

void simpanKonfig() {
  prefs.begin("smartfarm", false);
  prefs.putInt   ("tz",     tzOffsetJam);
  prefs.putBool  ("tzMan",  tzManual);
  prefs.putString("tzNama", tzNama);
  prefs.end();
}

/* =====================================================================
   PARSER JSON 
   ===================================================================== */
bool ambilJsonInt(const String &json, const char *key, long &out) {
  String k = "\"" + String(key) + "\":";
  int i = json.indexOf(k);
  if (i < 0) return false;
  i += k.length();
  while (i < (int)json.length() && json[i] == ' ') i++;
  int j = i;
  if (j < (int)json.length() && (json[j] == '-' || json[j] == '+')) j++;
  while (j < (int)json.length() && isDigit(json[j])) j++;
  if (j == i) return false;
  out = json.substring(i, j).toInt();
  return true;
}

bool ambilJsonStr(const String &json, const char *key, char *out, size_t maks) {
  String k = "\"" + String(key) + "\":\"";
  int i = json.indexOf(k);
  if (i < 0) return false;
  i += k.length();
  int j = json.indexOf('"', i);
  if (j < 0) return false;
  String v = json.substring(i, j);
  strncpy(out, v.c_str(), maks - 1);
  out[maks - 1] = 0;
  return true;
}

/* =====================================================================
   DETEKSI ZONA WAKTU
   ===================================================================== */
bool terapkanOffset(long offsetDetik, const char *asal,
                    const char *namaBaru, const char *kotaBaru, bool &sukses) {
  sukses = false;
  if (offsetDetik % 3600 != 0) {
    DBGF("[TZ] %s: offset %ld bukan kelipatan jam - DITOLAK\n", asal, offsetDetik);
    return false;
  }
  int jam = offsetDetik / 3600;
  if (jam < TZ_MIN_JAM || jam > TZ_MAX_JAM) {
    DBGF("[TZ] %s: UTC%+d di luar batas (+%d..+%d) - DITOLAK, tetap UTC+%d\n",
         asal, jam, TZ_MIN_JAM, TZ_MAX_JAM, tzOffsetJam);
    return false;
  }
  bool berubah = (jam != tzOffsetJam);
  tzOffsetJam  = jam;
  tzTerdeteksi = true;
  sukses       = true;
  if (namaBaru && namaBaru[0]) { strncpy(tzNama, namaBaru, sizeof(tzNama)-1); tzNama[sizeof(tzNama)-1] = 0; }
  if (kotaBaru && kotaBaru[0]) { strncpy(tzKota, kotaBaru, sizeof(tzKota)-1); tzKota[sizeof(tzKota)-1] = 0; }
  DBGF("[TZ] %s: UTC+%d (%s / %s)%s\n", asal, tzOffsetJam, tzNama, tzKota,
       berubah ? "  <<< BERUBAH" : "  (sama)");
  return berubah;
}

bool cobaIpApi(bool &sukses, bool &berubah) {
  sukses = false; berubah = false;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  http.begin("http://ip-api.com/json/?fields=status,offset,timezone,city");
  int kode = http.GET();
  if (kode != 200) { DBGF("[TZ] ip-api gagal (HTTP %d)\n", kode); http.end(); return false; }
  String body = http.getString();
  http.end();

  char status[16] = {0}, nama[32] = {0}, kota[32] = {0};
  ambilJsonStr(body, "status", status, sizeof(status));
  if (strcmp(status, "success") != 0) { DBGLN("[TZ] ip-api: status bukan success"); return false; }

  long offset;
  if (!ambilJsonInt(body, "offset", offset)) { DBGLN("[TZ] ip-api: offset hilang"); return false; }
  ambilJsonStr(body, "timezone", nama, sizeof(nama));
  ambilJsonStr(body, "city",     kota, sizeof(kota));
  berubah = terapkanOffset(offset, "ip-api", nama, kota, sukses);
  return sukses;
}

bool cobaWorldTime(bool &sukses, bool &berubah) {
  sukses = false; berubah = false;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  http.begin("http://worldtimeapi.org/api/ip");
  int kode = http.GET();
  if (kode != 200) { DBGF("[TZ] worldtime gagal (HTTP %d)\n", kode); http.end(); return false; }
  String body = http.getString();
  http.end();

  long raw = 0, dst = 0;
  char nama[32] = {0};
  if (!ambilJsonInt(body, "raw_offset", raw)) { DBGLN("[TZ] worldtime: raw_offset hilang"); return false; }
  ambilJsonInt(body, "dst_offset", dst);
  ambilJsonStr(body, "timezone", nama, sizeof(nama));
  berubah = terapkanOffset(raw + dst, "worldtime", nama, "-", sukses);
  return sukses;
}

bool deteksiZonaWaktu() {
  if (tzManual) {
    DBGF("[TZ] Mode MANUAL (UTC+%d) - auto-detect dilewati\n", tzOffsetJam);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) { DBGLN("[TZ] skip - WiFi belum konek"); return false; }

  bool sukses = false, berubah = false;
  cobaIpApi(sukses, berubah);
  if (!sukses) {
    DBGLN("[TZ] provider-1 gagal, coba provider-2...");
    cobaWorldTime(sukses, berubah);
  }
  if (!sukses) {
    DBGF("[TZ] Semua provider gagal - pakai nilai tersimpan UTC+%d\n", tzOffsetJam);
    return false;
  }
  simpanKonfig();
  return berubah;
}

/* =====================================================================
   NTP + RTC
   ===================================================================== */
bool syncRTCdariNTP() {
  if (WiFi.status() != WL_CONNECTED) { DBGLN("[NTP] skip - WiFi mati"); return false; }

  configTime(TZ_OFFSET_DETIK, 0, "pool.ntp.org", "time.google.com", "id.pool.ntp.org");
  struct tm ti;
  if (!getLocalTime(&ti, 5000)) {
    DBGLN("[NTP] GAGAL - pakai waktu RTC yang ada");
    return false;
  }
  ntpPernahSukses = true;

  if (rtcOK) {
    rtc.adjust(DateTime(ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                        ti.tm_hour, ti.tm_min, ti.tm_sec));
  }
  DBGF("[NTP] Waktu lokal -> %02d/%02d/%04d %02d:%02d:%02d  (UTC+%d, %s)%s\n",
       ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900,
       ti.tm_hour, ti.tm_min, ti.tm_sec, tzOffsetJam, tzNama,
       rtcOK ? "" : "  [RTC mati - jam internal ESP32]");
  return true;
}

void tugasZonaWaktu() {
  if (deteksiZonaWaktu()) {
    DBGLN("[TZ] Zona berubah -> resync jam");
    syncRTCdariNTP();
  }
}

bool ambilWaktu(int &jam, int &menit, int &detik, int &hari, int &bulan) {
  if (rtcOK) {
    DateTime now = rtc.now();
    if (now.year() >= 2024 && now.year() < 2100) {
      jam = now.hour(); menit = now.minute(); detik = now.second();
      hari = now.day(); bulan = now.month();
      return true;
    }
    DBGF("[WAKTU] RTC tahun ngaco (%d) - diabaikan\n", now.year());
  }
  if (ntpPernahSukses) {
    struct tm ti;
    if (getLocalTime(&ti, 100)) {
      jam = ti.tm_hour; menit = ti.tm_min; detik = ti.tm_sec;
      hari = ti.tm_mday; bulan = ti.tm_mon + 1;
      return true;
    }
  }
  return false;
}

bool cocokJadwal(int jamNow, int menitNow, int jamJadwal, int menitJadwal) {
  int nowM  = jamNow * 60 + menitNow;
  int jdwM  = jamJadwal * 60 + menitJadwal;
  int delta = (nowM - jdwM + 1440) % 1440;
  return delta < JENDELA_JADWAL_MENIT;
}

int jadwalYangCocok(int jam, int menit) {
  if (cocokJadwal(jam, menit, jadwalPagiJam, jadwalPagiMenit)) return 1;
  if (cocokJadwal(jam, menit, jadwalSoreJam, jadwalSoreMenit)) return 2;
  return 0;
}
bool sudahSiramHariIni(int which, int hari) {
  return (which == 1 && kunciPagi == hari) || (which == 2 && kunciSore == hari);
}
void tandaiSudahSiram(int which, int hari) {
  if (which == 1) kunciPagi = hari; else if (which == 2) kunciSore = hari;
}

/* =====================================================================
   KONTROL POMPA
   ===================================================================== */
void setPompa(bool nyala) {
  if (nyala && lockoutPompa) {
    static unsigned long tsWarn = 0;
    if (millis() - tsWarn > 60000) {
      tsWarn = millis();
      DBGLN("[POMPA] Permintaan ON DIBLOKIR - lockout. Ketik 'unlock' atau ganti mode.");
      if (Blynk.connected())
        Blynk.logEvent("pompa_lockout", "Pompa terkunci - perlu direset");
    }
    nyala = false;
  }

  bool berubah = (nyala != pompaNyala);
  pompaNyala = nyala;

  if (berubah) {
    digitalWrite(PIN_RELAY, nyala ? RELAY_ON : RELAY_OFF);
    pompaMulaiMs = nyala ? millis() : 0;
    DBGF("[POMPA] %s\n", nyala ? "NYALA" : "MATI");
    if (Blynk.connected()) Blynk.virtualWrite(V7, nyala ? 1 : 0);
  }
}

void watchdogPompa() {
  if (pompaNyala && millis() - pompaMulaiMs > MAKS_POMPA_MS) {
    lockoutPompa = true;
    manualOn     = false;
    setPompa(false);
    DBGLN("[SAFETY] Pompa nyala >20 menit -> LOCKOUT");
    if (Blynk.connected()) {
      Blynk.logEvent("pompa_lockout", "Pompa dimatikan paksa (>20 menit)");
      Blynk.virtualWrite(V4, 0);
    }
  }
}

/* =====================================================================
   BACA SENSOR + KIRIM HEMAT
   ===================================================================== */
bool perluKirim(float baru, float &lama, float minDelta, unsigned long &ts) {
  unsigned long now = millis();
  if (now - ts < KIRIM_MIN_MS) return false;
  if (fabs(baru - lama) >= minDelta || now - ts > KIRIM_PAKSA_MS) {
    lama = baru; ts = now;
    return true;
  }
  return false;
}

void bacaSensor() {
  if (adsOK) {
    /* -- kelembapan tanah -- */
    soilRaw   = ads.readADC_SingleEnded(CH_SOIL);
    soilValid = (soilRaw > SOIL_RAW_MIN && soilRaw < SOIL_RAW_MAKS);
    if (soilValid) {
      kelembapanTanah = constrain(map(soilRaw, SOIL_KERING, SOIL_BASAH, 0, 100), 0, 100);
    } else {
      DBGF("[SENSOR] soilRaw=%d di luar batas - probe lepas / kabel putus?\n", soilRaw);
    }

    /* -- hujan: validasi  -- */
    rainRaw   = ads.readADC_SingleEnded(CH_RAIN);
    rainValid = (rainRaw > RAIN_RAW_MIN && rainRaw < RAIN_RAW_MAKS);
    if (rainValid) {
      static uint8_t cntHujan = 0;
      bool bacaan = sedangHujan ? (rainRaw < RAIN_OFF) : (rainRaw < RAIN_ON);
      if (bacaan != sedangHujan) {
        if (++cntHujan >= RAIN_BUTUH_N) {
          sedangHujan = bacaan; cntHujan = 0;
          DBGF("[SENSOR] Status hujan -> %s (raw=%d)\n",
               sedangHujan ? "HUJAN" : "KERING", rainRaw);
        }
      } else cntHujan = 0;
    } else {
      if (sedangHujan) DBGLN("[SENSOR] Modul hujan invalid - status direset KERING");
      sedangHujan = false;
      DBGF("[SENSOR] rainRaw=%d di luar batas - modul hujan lepas?\n", rainRaw);
    }
  } else {
    soilValid = rainValid = false;
    sedangHujan = false;
    DBGLN("[SENSOR] ADS1115 mati - data tanah/hujan TIDAK VALID");
  }

  /* -- DHT22 -- */
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) { kelembapanUdara = h; suhuUdara = t; dhtOK = true; }
  else { dhtOK = false; DBGLN("[SENSOR] DHT22 gagal (nan) - cek pull-up 10k"); }

  if (!Blynk.connected()) return;


  static float lastSoil = -999, lastSuhu = -999, lastRH = -999;
  static int   lastHujan = -1;
  static unsigned long tsSoil = 0, tsSuhu = 0, tsRH = 0, tsHujan = 0;

  if (soilValid && perluKirim(kelembapanTanah, lastSoil, 2.0f, tsSoil))
    Blynk.virtualWrite(V0, kelembapanTanah);
  if (dhtOK) {
    if (perluKirim(suhuUdara, lastSuhu, 0.3f, tsSuhu))    Blynk.virtualWrite(V1, suhuUdara);
    if (perluKirim(kelembapanUdara, lastRH, 1.0f, tsRH))  Blynk.virtualWrite(V2, kelembapanUdara);
  }
  if (rainValid) {
    int hj = sedangHujan ? 1 : 0;
    if (hj != lastHujan || millis() - tsHujan > KIRIM_PAKSA_MS) {
      Blynk.virtualWrite(V3, hj);
      lastHujan = hj; tsHujan = millis();
    }
  }
}

/* =====================================================================
   LOGIKA PENYIRAMAN
   ===================================================================== */
void logikaPenyiraman() {
  if (manualOn) { setPompa(true); return; }

  int jam = 0, menit = 0, detik = 0, hari = 0, bulan = 0;
  bool waktuValid = ambilWaktu(jam, menit, detik, hari, bulan);

  int modeEfektif = mode;
  if ((mode == 1 || mode == 3) && !waktuValid) {
    modeEfektif = 2;
    DBGLN("[LOGIKA] Waktu tidak valid -> mode jadwal dialihkan ke mode sensor");
  }

  /* ---- MODE 1: JADWAL MURNI ---- */
  if (modeEfektif == 1) {
    int which = jadwalYangCocok(jam, menit);
    if (which && !sudahSiramHariIni(which, hari)) {
      mulaiSiramMs = millis();
      tandaiSudahSiram(which, hari);
      DBGF("[LOGIKA] Mode1: jadwal %s (%02d:%02d) -> MULAI SIRAM\n",
           which == 1 ? "PAGI" : "SORE", jam, menit);
    }
    bool dalamDurasi = (mulaiSiramMs > 0 && millis() - mulaiSiramMs < DURASI_SIRAM_MS);
    setPompa(dalamDurasi);
    if (!dalamDurasi) mulaiSiramMs = 0;
  }

  /* ---- MODE 2: SENSOR ----
                             */
  else if (modeEfektif == 2) {
    if (!adsOK || !soilValid) {
      setPompa(false);
      keputusanSensorAda = false;
      DBGLN("[LOGIKA] Mode2: data tanah tidak valid -> pompa dipaksa MATI");
      return;
    }

   
    if (!keputusanSensorAda) {
      bool nyala = (kelembapanTanah < SENSOR_TENGAH);
      setPompa(nyala);
      keputusanSensorAda = true;
      DBGF("[LOGIKA] Mode2: keputusan awal tanah=%d%% (tengah=%d) -> pompa %s\n",
           kelembapanTanah, SENSOR_TENGAH, nyala ? "ON" : "OFF");
      return;
    }

    if (kelembapanTanah < SENSOR_NYALA)      setPompa(true);
    else if (kelembapanTanah > SENSOR_MATI)  setPompa(false);
    
  }

  /* ---- MODE 3: KOMBINASI ---- */
  else if (modeEfektif == 3) {
    int which = jadwalYangCocok(jam, menit);
    if (which && !sudahSiramHariIni(which, hari)) {
      if (!soilValid) {
        DBGLN("[LOGIKA] Mode3: sensor tanah invalid -> jadwal ditunda");
      } else if (kelembapanTanah < AMBANG_KERING && !sedangHujan) {
        mulaiSiramMs = millis();
        tandaiSudahSiram(which, hari);
        DBGF("[LOGIKA] Mode3: jadwal %s, tanah %d%% -> SIRAM\n",
             which == 1 ? "PAGI" : "SORE", kelembapanTanah);
        if (Blynk.connected())
          Blynk.logEvent("siram_mulai", "Penyiraman terjadwal dimulai");
      } else {
        DBGF("[LOGIKA] Mode3: jadwal %s ditunda (tanah %d%%, hujan %d) - retry dlm jendela\n",
             which == 1 ? "PAGI" : "SORE", kelembapanTanah, sedangHujan);
      }
    }
       bool dalamDurasi = (mulaiSiramMs > 0 && millis() - mulaiSiramMs < DURASI_SIRAM_MS);

        if (dalamDurasi && soilValid && kelembapanTanah >= SIRAM_CUKUP) {
            DBGF("[LOGIKA] Mode3: tanah %d%% >= %d%% -> siram DIJEDA (timer jalan)\n",
           kelembapanTanah, SIRAM_CUKUP);
      setPompa(false);
      return;
    }


    if (sedangHujan && dalamDurasi) {
      static bool sudahLapor = false;
      if (!sudahLapor) {
        DBGLN("[LOGIKA] Mode3: hujan datang -> siram DIJEDA (timer tetap jalan)");
        sudahLapor = true;
      }
      setPompa(false);
      return;                            
    }

    setPompa(dalamDurasi);
    if (!dalamDurasi) mulaiSiramMs = 0;
  }

  else {
    setPompa(false);
    DBGF("[LOGIKA] Mode %d tidak dikenal -> pompa MATI. Cek min/max datastream V6!\n", mode);
  }
}

/* =====================================================================
   NOTIFIKASI
   ===================================================================== */
void cekNotifikasi() {
  static bool sudahNotifKering = false;
  if (!soilValid) return;
  if (kelembapanTanah < AMBANG_KERING && !sudahNotifKering) {
    DBGLN("[NOTIF] tanah kering - kirim notifikasi");
    if (Blynk.connected()) Blynk.logEvent("tanah_kering", "Kelembapan tanah rendah!");
    sudahNotifKering = true;
  }
  if (kelembapanTanah >= AMBANG_KERING + NOTIF_GAP) sudahNotifKering = false;
}

/* =====================================================================
   JAGA KONEKSI
   ===================================================================== */
void jagaKoneksi() {
  if (!adsOK && ads.begin(ADS_ALAMAT)) {
    ads.setGain(ADS_GAIN);
    adsOK = true;
    DBGLN("[RECOVER] ADS1115 hidup lagi");
  }
  if (!rtcOK && rtc.begin()) {
    rtcOK = true;
    DBGLN("[RECOVER] RTC hidup lagi - resync NTP");
    syncRTCdariNTP();
  }

  if (WiFi.status() != WL_CONNECTED) {
    DBGLN("[KONEKSI] WiFi putus - menyambung ulang...");
    WiFi.disconnect();
    WiFi.begin(ssid, pass);
    return;
  }
  if (!Blynk.connected()) {
    DBGLN("[KONEKSI] WiFi OK, Blynk putus - sambung ulang...");
    Blynk.connect(3000);
  }
  if (!ntpPernahSukses) syncRTCdariNTP();
}

/* =====================================================================
   DEBUG
   ===================================================================== */
void debugJadwal() {
#if DEBUG
  int jam, menit, detik, hari, bulan;
  if (!ambilWaktu(jam, menit, detik, hari, bulan)) {
    DBGLN("[JADWAL] TIDAK ADA WAKTU VALID - RTC mati & NTP belum sukses");
    return;
  }
  int which = jadwalYangCocok(jam, menit);
  DBGF("[JADWAL] %02d/%02d %02d:%02d:%02d UTC+%d%s | pagi=%02d:%02d sore=%02d:%02d | cocok=%s "
       "| tanah=%d%%(raw=%d,ok=%d) hujan=%d(raw=%d,ok=%d) | mode=%d pompa=%d lock=%d\n",
       hari, bulan, jam, menit, detik, tzOffsetJam,
       tzManual ? "[M]" : (tzTerdeteksi ? "[A]" : "[?]"),
       jadwalPagiJam, jadwalPagiMenit, jadwalSoreJam, jadwalSoreMenit,
       which == 0 ? "-" : (which == 1 ? "PAGI" : "SORE"),
       kelembapanTanah, soilRaw, soilValid,
       sedangHujan, rainRaw, rainValid,
       mode, pompaNyala, lockoutPompa);
#endif
}


void cekPerintahSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim(); cmd.toLowerCase();
  if (cmd.length() == 0) return;

  if (cmd == "tz auto") {
    tzManual = false; simpanKonfig();
    DBGLN("[CMD] Timezone -> AUTO, deteksi ulang...");
    deteksiZonaWaktu(); syncRTCdariNTP();

  } else if (cmd.startsWith("tz ")) {
    int jam = cmd.substring(3).toInt();
    if (jam < TZ_MIN_JAM || jam > TZ_MAX_JAM) {
      DBGF("[CMD] Nilai harus %d..%d. Contoh: tz 9\n", TZ_MIN_JAM, TZ_MAX_JAM);
      return;
    }
    tzOffsetJam = jam; tzManual = true;
    strcpy(tzNama, jam == 7 ? "Asia/Jakarta" : (jam == 8 ? "Asia/Makassar" : "Asia/Jayapura"));
    simpanKonfig();
    DBGF("[CMD] Timezone -> MANUAL UTC+%d (%s)\n", tzOffsetJam, tzNama);
    syncRTCdariNTP();

  } else if (cmd == "jadwal reset") {
    jadwalPagiJam = JADWAL_PAGI_JAM; jadwalPagiMenit = JADWAL_PAGI_MENIT;
    jadwalSoreJam = JADWAL_SORE_JAM; jadwalSoreMenit = JADWAL_SORE_MENIT;
    kunciPagi = kunciSore = -1;
    DBGF("[CMD] Jadwal kembali ke firmware: pagi %02d:%02d | sore %02d:%02d\n",
         jadwalPagiJam, jadwalPagiMenit, jadwalSoreJam, jadwalSoreMenit);

  } else if (cmd.startsWith("jadwal ")) {
    char sesi; int j, m;
    if (sscanf(cmd.c_str(), "jadwal %c %d %d", &sesi, &j, &m) == 3 &&
        j >= 0 && j <= 23 && m >= 0 && m <= 59) {
      if (sesi == 'p')      { jadwalPagiJam = j; jadwalPagiMenit = m; kunciPagi = -1; }
      else if (sesi == 's') { jadwalSoreJam = j; jadwalSoreMenit = m; kunciSore = -1; }
      else { DBGLN("[CMD] Sesi harus 'p' atau 's'"); return; }
      DBGF("[CMD] Jadwal %s -> %02d:%02d (SEMENTARA, hilang saat reboot)\n",
           sesi == 'p' ? "pagi" : "sore", j, m);
    } else {
      DBGLN("[CMD] Format: jadwal p 6 30 | jadwal s 17 0 | jadwal reset");
    }

  } else if (cmd == "sync") {
    DBGLN("[CMD] Paksa sync NTP...");
    syncRTCdariNTP();

  } else if (cmd == "unlock") {
    lockoutPompa = false;
    keputusanSensorAda = false;
    DBGLN("[CMD] Lockout pompa direset");

  } else if (cmd == "info") {
    DBGLN("---------------------------------------");
    DBGF("TZ        : UTC+%d (%s) %s\n", tzOffsetJam, tzNama, tzManual ? "MANUAL" : "AUTO");
    DBGF("Kota (IP) : %s\n", tzKota);
    DBGF("Jadwal    : pagi %02d:%02d | sore %02d:%02d\n",
         jadwalPagiJam, jadwalPagiMenit, jadwalSoreJam, jadwalSoreMenit);
    DBGF("Kalibrasi : kering=%d basah=%d (bawaan firmware)\n", SOIL_KERING, SOIL_BASAH);
    DBGF("Tanah     : %d%% (raw=%d valid=%d)\n", kelembapanTanah, soilRaw, soilValid);
    DBGF("Hujan     : %d (raw=%d valid=%d)\n", sedangHujan, rainRaw, rainValid);
    DBGF("Ambang    : ON<%d  OFF>%d  (zona mati %d-%d)\n",
         SENSOR_NYALA, SENSOR_MATI, SENSOR_NYALA, SENSOR_MATI);
    DBGF("Sensor    : ADS=%d RTC=%d DHT=%d NTP=%d\n", adsOK, rtcOK, dhtOK, ntpPernahSukses);
    DBGF("Mode      : %d | pompa=%d | lockout=%d\n", mode, pompaNyala, lockoutPompa);
    DBGF("WiFi      : %s | Blynk: %d\n",
         WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "putus",
         Blynk.connected());
    DBGLN("---------------------------------------");

  } else {
    DBGLN("[CMD] tz auto | tz 7|8|9 | jadwal p/s <jam> <menit> | jadwal reset |");
    DBGLN("      sync | unlock | info");
  }
}

/* =====================================================================
   HANDLER BLYNK
   ===================================================================== */
BLYNK_WRITE(V4) {
  manualOn = param.asInt();
  DBGF("[HP] tombol manual: %s\n", manualOn ? "ON" : "OFF");
  if (!manualOn) { setPompa(false); keputusanSensorAda = false; }
}

BLYNK_WRITE(V6) {
  mode = param.asInt();
  mulaiSiramMs = 0;
  kunciPagi = kunciSore = -1;
  lockoutPompa = false;
  keputusanSensorAda = false;             
  DBGF("[HP] ganti mode -> %d (lockout & keputusan sensor direset)\n", mode);
  if (mode < 1 || mode > 3)
    DBGF("[HP] PERINGATAN: mode %d di luar 1-3. Cek min/max datastream V6!\n", mode);
}

BLYNK_CONNECTED() {
  DBGLN("[Blynk] terhubung - sinkron status");
  Blynk.syncVirtual(V4, V6);
  Blynk.virtualWrite(V7, pompaNyala ? 1 : 0);
}

/* =====================================================================
   I2C SCANNER
   ===================================================================== */
void scanI2C() {
#if DEBUG
  DBGLN("[I2C] Memindai bus...");
  byte jumlah = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      DBGF("      ditemukan perangkat di 0x%02X", addr);
      if (addr == 0x48)      DBGLN("  <- ADS1115");
      else if (addr == 0x68) DBGLN("  <- RTC DS3231");
      else                   DBGLN("  <- (tidak dikenal)");
      jumlah++;
    }
  }
  if (jumlah == 0) DBGLN("[I2C] TIDAK ADA perangkat! Cek SDA/SCL & power.");
  else             DBGF("[I2C] Total %d perangkat.\n", jumlah);
#endif
}

/* =====================================================================
   SETUP
   ===================================================================== */
void setup() {
  digitalWrite(PIN_RELAY, RELAY_OFF);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_OFF);

  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(1000);
  DBGLN();
  DBGLN("======================================");
  DBGLN("  SMART FARMING KACANG PANJANG v2.3");
  DBGF ("  Debug: %s\n", DEBUG ? "ON" : "OFF");
  DBGLN("======================================");
  DBGLN("[BOOT] Relay siap - pompa dipastikan MATI");

  muatKonfig();

  Wire.begin(PIN_SDA, PIN_SCL);
  DBGF("[BOOT] I2C mulai (SDA=%d, SCL=%d)\n", PIN_SDA, PIN_SCL);
  scanI2C();

  dht.begin();
  delay(200);
  {
    float t = dht.readTemperature();
    if (isnan(t)) DBGLN("[BOOT] DHT22 [GAGAL] - cek wiring & pull-up 10k");
    else { dhtOK = true; DBGF("[BOOT] DHT22 [OK] - suhu awal %.1fC\n", t); }
  }

  if (ads.begin(ADS_ALAMAT)) {
    ads.setGain(ADS_GAIN);
    adsOK = true;
    DBGLN("[BOOT] ADS1115 [OK]");
  } else {
    DBGLN("[BOOT] ADS1115 [GAGAL] - cek wiring I2C & ADDR->GND");
  }

  if (rtc.begin()) {
    rtcOK = true;
    DateTime now = rtc.now();
    DBGF("[BOOT] RTC [OK] - %02d/%02d/%04d %02d:%02d:%02d\n",
         now.day(), now.month(), now.year(), now.hour(), now.minute(), now.second());
    if (rtc.lostPower())   DBGLN("[BOOT] RTC kehilangan daya - tunggu sync NTP");
    if (now.year() < 2024) DBGLN("[BOOT] RTC tahun ngaco - tunggu sync NTP");
  } else {
    DBGLN("[BOOT] RTC [GAGAL] - fallback jam internal ESP32 via NTP");
  }

  DBGF("[BOOT] Menyambung WiFi '%s'", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  {
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(500); DBG("."); }
    DBGLN();
  }

  if (WiFi.status() == WL_CONNECTED) {
    DBGF("[BOOT] WiFi [OK] - IP:%s RSSI:%d dBm\n",
         WiFi.localIP().toString().c_str(), WiFi.RSSI());
    deteksiZonaWaktu();
    syncRTCdariNTP();
    Blynk.config(BLYNK_AUTH_TOKEN);
    if (Blynk.connect(10000)) DBGLN("[BOOT] Blynk [OK]");
    else                      DBGLN("[BOOT] Blynk [GAGAL] - coba ulang otomatis");
  } else {
    DBGLN("[BOOT] WiFi [GAGAL] - sistem jalan OFFLINE.");
    DBGF ("       Zona pakai nilai tersimpan: UTC+%d (%s)\n", tzOffsetJam, tzNama);
  }

  timer.setInterval(INTERVAL_BACA_SENSOR, bacaSensor);
  timer.setInterval(INTERVAL_LOGIKA,      logikaPenyiraman);
  timer.setInterval(INTERVAL_NOTIF,       cekNotifikasi);
  timer.setInterval(INTERVAL_KONEKSI,     jagaKoneksi);
  timer.setInterval(INTERVAL_WATCHDOG,    watchdogPompa);
  timer.setInterval(INTERVAL_SERIAL,      cekPerintahSerial);
  timer.setInterval(INTERVAL_SYNC_NTP,    syncRTCdariNTP);
  timer.setInterval(INTERVAL_DETECT_TZ,   tugasZonaWaktu);
#if DEBUG
  timer.setInterval(INTERVAL_DEBUG_JDW,   debugJadwal);
#endif

  DBGLN("======================================");
  DBGLN("  SETUP SELESAI - SISTEM BERJALAN");
  DBGLN("  Ketik 'info' di Serial Monitor");
  DBGLN("======================================");
}

/* =====================================================================
   LOOP
   ===================================================================== */
void loop() {
  if (Blynk.connected()) Blynk.run();
  timer.run();
}