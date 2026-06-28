/* ===========================================================================
 *  VIEWE UEDX48480021-MD80ET — Multi-page HA dashboard (MQTT + auto-discovery)
 *  v2 — networking moved to its own core so page changes are instant.
 *
 *  PAGES (rotate knob to flip; dots show position):
 *    0 Dashboard  - temperature, weather, alarm status
 *    1 Alarm      - Arm Away / Arm Home / Disarm  (tap by touch)
 *    2 Doorbell   - JPEG snapshot (knob press = refresh)
 *    3 Info       - IP, WiFi signal, MQTT status
 *
 *  WHY IT'S FASTER NOW: a dedicated FreeRTOS task on core 0 owns ALL network
 *  work (MQTT connect/loop/publish + the camera HTTP fetch). The Arduino
 *  loop() on core 1 only reads the encoder/button and flips pages, so it can
 *  never be blocked by a slow MQTT reconnect or a snapshot download.
 *
 *  Libraries: PubSubClient (Nick O'Leary), TJpg_Decoder (Bodmer), ESP32Encoder.
 * =========================================================================== */

#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include <ESP32Encoder.h>
#include <TJpg_Decoder.h>

using namespace esp_panel::drivers;
using namespace esp_panel::board;

/* ----------------------- USER CONFIG ------------------------------------- */
static const char *WIFI_SSID = "WIFI_SSID";
static const char *WIFI_PASS = "WIFI_PASSWORD";

static const char *MQTT_HOST = "192.168.10.XXX";
static const int   MQTT_PORT = 1883;
static const char *MQTT_USER = "YOUR_MQTT_USER";
static const char *MQTT_PASS = "YOUR_MQTT_PASSWORD";

static const char *HA_HOST    = "http://192.168.10.XXX:8123";
static const char *HA_TOKEN   = "XXXXXXXXXXXXXXXXXXXXXXXXXXX";
static const char *CAM_ENTITY = "ENTITY";

#define ENC_A_PIN    6
#define ENC_B_PIN    5
#define ENC_BTN_PIN  0
#define ENC_DIVISOR  2     // set 1 if one detent doesn't flip a page
/* ------------------------------------------------------------------------- */

static const char *NODE  = "knob";
static const char *AVAIL = "knob/status";
#define T_TEMP      "knob/set/temperature"
#define T_WEATHER   "knob/set/weather"
#define T_HUMID     "knob/set/humidity"
#define T_ALARM     "knob/set/alarm"
#define T_ALARM_CMD "knob/cmd/alarm"

WiFiClient   net;
PubSubClient mqtt(net);
ESP32Encoder encoder;

#define CAM_W 400
#define CAM_H 400
static lv_color_t *cam_buf = nullptr;

static const int PAGE_COUNT = 4;
static volatile int cur_page = 0;
static lv_obj_t *pages[PAGE_COUNT];
static lv_obj_t *dots[PAGE_COUNT];

static lv_obj_t *lbl_temp, *lbl_weather, *lbl_alarm_status;
static lv_obj_t *lbl_alarm_state2;
static lv_obj_t *cam_canvas, *lbl_cam_status;
static lv_obj_t *lbl_ip, *lbl_rssi, *lbl_mqtt;

// flags shared loop()(core1) -> netTask(core0). Only netTask touches mqtt/HTTP.
static volatile bool  cam_refresh_req = false;
static volatile bool  g_pub_page      = false;
static volatile bool  g_action_press  = false;
static volatile bool  g_alarm_pending = false;
static char           g_alarm_cmd[12] = "";

/* ===================== LVGL helpers ====================================== */
static void setLabel(lv_obj_t *l, const String &s) {
  lvgl_port_lock(-1);
  if (l) lv_label_set_text(l, s.c_str());
  lvgl_port_unlock();
}

static void show_page(int i) {
  if (i < 0) i = PAGE_COUNT - 1;
  if (i >= PAGE_COUNT) i = 0;
  cur_page = i;
  lvgl_port_lock(-1);
  for (int p = 0; p < PAGE_COUNT; p++) {
    if (p == i) lv_obj_clear_flag(pages[p], LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(pages[p], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(dots[p],
        p == i ? lv_color_hex(0x0a84ff) : lv_color_hex(0x444444), 0);
  }
  lvgl_port_unlock();
  g_pub_page = true;                 // netTask publishes knob/page
  if (i == 2) cam_refresh_req = true;
}

/* ===================== MQTT ============================================== */
static String devBlock() {
  return String("\"dev\":{\"ids\":[\"knob_uedx\"],\"name\":\"Knob Display\","
                "\"mdl\":\"UEDX48480021-MD80ET\",\"mf\":\"VIEWE\"}");
}
static void pubDiscovery() {
  String base = String("\"avty_t\":\"") + AVAIL + "\"," + devBlock();
  String c;
  c = String("{\"name\":\"IP\",\"uniq_id\":\"knob_ip\",\"stat_t\":\"knob/ip\","
             "\"ent_cat\":\"diagnostic\",") + base + "}";
  mqtt.publish("homeassistant/sensor/knob/ip/config", c.c_str(), true);
  c = String("{\"name\":\"WiFi Signal\",\"uniq_id\":\"knob_rssi\",\"stat_t\":\"knob/rssi\","
             "\"dev_cla\":\"signal_strength\",\"unit_of_meas\":\"dBm\","
             "\"ent_cat\":\"diagnostic\",") + base + "}";
  mqtt.publish("homeassistant/sensor/knob/rssi/config", c.c_str(), true);
  c = String("{\"name\":\"Page\",\"uniq_id\":\"knob_page\",\"stat_t\":\"knob/page\","
             "\"ent_cat\":\"diagnostic\",") + base + "}";
  mqtt.publish("homeassistant/sensor/knob/page/config", c.c_str(), true);
  c = String("{\"name\":\"Last Action\",\"uniq_id\":\"knob_action\",\"stat_t\":\"knob/action\",")
             + base + "}";
  mqtt.publish("homeassistant/sensor/knob/action/config", c.c_str(), true);
}

static void onMqtt(char *topic, byte *payload, unsigned int len) {
  String t = topic, v; v.reserve(len);
  for (unsigned int i = 0; i < len; i++) v += (char)payload[i];
  if (t == T_TEMP)         setLabel(lbl_temp, v + "\xC2\xB0");
  else if (t == T_WEATHER) setLabel(lbl_weather, v);
  else if (t == T_ALARM) { setLabel(lbl_alarm_status, "Alarm: " + v);
                           setLabel(lbl_alarm_state2, v); }
}

static void mqttReconnect() {
  if (mqtt.connected()) return;
  static uint32_t last = 0;
  if (millis() - last < 4000) return;
  last = millis();
  mqtt.setBufferSize(1024);
  if (mqtt.connect(NODE, MQTT_USER, MQTT_PASS, AVAIL, 0, true, "offline")) {
    mqtt.publish(AVAIL, "online", true);
    mqtt.subscribe(T_TEMP); mqtt.subscribe(T_WEATHER);
    mqtt.subscribe(T_HUMID); mqtt.subscribe(T_ALARM);
    pubDiscovery();
    setLabel(lbl_mqtt, "MQTT: connected");
  } else {
    setLabel(lbl_mqtt, "MQTT: retrying...");
  }
}

/* ===================== Camera snapshot (runs in netTask) ================= */
static int tjpg_off_x, tjpg_off_y;
static bool tjpgOut(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
  for (uint16_t j = 0; j < h; j++) {
    int yy = y + j + tjpg_off_y; if (yy < 0 || yy >= CAM_H) continue;
    for (uint16_t i = 0; i < w; i++) {
      int xx = x + i + tjpg_off_x; if (xx < 0 || xx >= CAM_W) continue;
      cam_buf[yy * CAM_W + xx].full = bmp[j * w + i];
    }
  }
  return true;
}
static void fetchSnapshot() {
  setLabel(lbl_cam_status, "Loading...");
  if (WiFi.status() != WL_CONNECTED) { setLabel(lbl_cam_status, "No WiFi"); return; }
  HTTPClient http;
  http.begin(String(HA_HOST) + "/api/camera_proxy/" + CAM_ENTITY);
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  int code = http.GET();
  if (code != 200) { setLabel(lbl_cam_status, "HTTP " + String(code)); http.end(); return; }
  int len = http.getSize();
  if (len <= 0 || len > 250000) { setLabel(lbl_cam_status, "Bad size"); http.end(); return; }
  uint8_t *jpg = (uint8_t *)ps_malloc(len);
  if (!jpg) { setLabel(lbl_cam_status, "OOM"); http.end(); return; }
  WiFiClient *s = http.getStreamPtr();
  int got = 0; uint32_t t0 = millis();
  while (got < len && millis() - t0 < 6000) {
    if (s->available()) got += s->readBytes(jpg + got, len - got); else delay(1);
  }
  http.end();
  uint16_t jw, jh; TJpgDec.getJpgSize(&jw, &jh, jpg, got);
  uint8_t scale = 1;
  while ((jw / scale > CAM_W || jh / scale > CAM_H) && scale < 8) scale <<= 1;
  TJpgDec.setJpgScale(scale);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tjpgOut);
  tjpg_off_x = (CAM_W - jw / scale) / 2;
  tjpg_off_y = (CAM_H - jh / scale) / 2;
  lvgl_port_lock(-1);
  memset(cam_buf, 0, (size_t)CAM_W * CAM_H * sizeof(lv_color_t));
  TJpgDec.drawJpg(0, 0, jpg, got);
  lv_obj_invalidate(cam_canvas);
  lvgl_port_unlock();
  free(jpg);
  setLabel(lbl_cam_status, "");
}

/* ===================== Network task (core 0) ============================= */
static void netTask(void *) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setSocketTimeout(2);     // a failed connect can't hang us
  mqtt.setKeepAlive(20);
  uint32_t lastDiag = 0;
  for (;;) {
    mqttReconnect();
    mqtt.loop();
    if (g_pub_page)      { g_pub_page = false;
      if (mqtt.connected()) mqtt.publish("knob/page", String((int)cur_page).c_str()); }
    if (g_alarm_pending) { g_alarm_pending = false;
      if (mqtt.connected()) { mqtt.publish(T_ALARM_CMD, g_alarm_cmd);
                              mqtt.publish("knob/action", g_alarm_cmd); } }
    if (g_action_press)  { g_action_press = false;
      if (mqtt.connected()) mqtt.publish("knob/action", "press"); }
    if (cam_refresh_req) { cam_refresh_req = false; fetchSnapshot(); }
    if (millis() - lastDiag > 3000) {
      lastDiag = millis();
      String ip = WiFi.isConnected() ? WiFi.localIP().toString() : "—";
      int rssi  = WiFi.RSSI();
      setLabel(lbl_ip,   "IP: " + ip);
      setLabel(lbl_rssi, "RSSI: " + String(rssi) + " dBm");
      if (mqtt.connected()) { mqtt.publish("knob/ip", ip.c_str());
                              mqtt.publish("knob/rssi", String(rssi).c_str()); }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* ===================== UI build ========================================== */
static lv_obj_t *makePage() {
  lv_obj_t *p = lv_obj_create(lv_scr_act());
  lv_obj_set_size(p, 480, 480); lv_obj_center(p);
  lv_obj_set_style_bg_color(p, lv_color_black(), 0);
  lv_obj_set_style_border_width(p, 0, 0); lv_obj_set_style_radius(p, 0, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  return p;
}
static lv_obj_t *mkLabel(lv_obj_t *par, const char *txt, const lv_font_t *f,
                         lv_align_t a, int y, uint32_t col = 0xffffff) {
  lv_obj_t *l = lv_label_create(par);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_align(l, a, 0, y);
  return l;
}
static void alarmBtn(lv_obj_t *par, const char *txt, int y, const char *cmd) {
  lv_obj_t *b = lv_btn_create(par);
  lv_obj_set_size(b, 240, 56);
  lv_obj_align(b, LV_ALIGN_CENTER, 0, y);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, txt); lv_obj_center(l);
  lv_obj_add_event_cb(b, [](lv_event_t *e){
    const char *c = (const char *)lv_event_get_user_data(e);
    strncpy(g_alarm_cmd, c, sizeof(g_alarm_cmd) - 1);
    g_alarm_pending = true;          // netTask publishes it
  }, LV_EVENT_CLICKED, (void *)cmd);
}
static void buildUI() {
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

  pages[0] = makePage();
  mkLabel(pages[0], "HOME", &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 40, 0x8e8e93);
  lbl_temp    = mkLabel(pages[0], "--", &lv_font_montserrat_28, LV_ALIGN_CENTER, -40);
  lbl_weather = mkLabel(pages[0], "--", &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0xc0c0c0);
  lbl_alarm_status = mkLabel(pages[0], "Alarm: --", &lv_font_montserrat_14, LV_ALIGN_CENTER, 40, 0xc0c0c0);

  pages[1] = makePage();
  mkLabel(pages[1], "ALARM", &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 40, 0x8e8e93);
  lbl_alarm_state2 = mkLabel(pages[1], "--", &lv_font_montserrat_28, LV_ALIGN_TOP_MID, 80);
  alarmBtn(pages[1], "Arm Away", -10, "ARM_AWAY");
  alarmBtn(pages[1], "Arm Home", 56,  "ARM_HOME");
  alarmBtn(pages[1], "Disarm",   122, "DISARM");

  pages[2] = makePage();
  cam_buf = (lv_color_t *)ps_malloc((size_t)CAM_W * CAM_H * sizeof(lv_color_t));
  cam_canvas = lv_canvas_create(pages[2]);
  if (cam_buf) {
    memset(cam_buf, 0, (size_t)CAM_W * CAM_H * sizeof(lv_color_t));
    lv_canvas_set_buffer(cam_canvas, cam_buf, CAM_W, CAM_H, LV_IMG_CF_TRUE_COLOR);
  }
  lv_obj_center(cam_canvas);
  lbl_cam_status = mkLabel(pages[2], "Press knob to load", &lv_font_montserrat_14, LV_ALIGN_CENTER, 0, 0x8e8e93);

  pages[3] = makePage();
  mkLabel(pages[3], "INFO", &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 40, 0x8e8e93);
  lbl_ip   = mkLabel(pages[3], "IP: --",   &lv_font_montserrat_14, LV_ALIGN_CENTER, -30, 0xc0c0c0);
  lbl_rssi = mkLabel(pages[3], "RSSI: --", &lv_font_montserrat_14, LV_ALIGN_CENTER, 0,   0xc0c0c0);
  lbl_mqtt = mkLabel(pages[3], "MQTT: --", &lv_font_montserrat_14, LV_ALIGN_CENTER, 30,  0xc0c0c0);

  for (int i = 0; i < PAGE_COUNT; i++) {
    dots[i] = lv_obj_create(lv_scr_act());
    lv_obj_set_size(dots[i], 10, 10);
    lv_obj_set_style_radius(dots[i], 5, 0);
    lv_obj_set_style_border_width(dots[i], 0, 0);
    lv_obj_align(dots[i], LV_ALIGN_BOTTOM_MID, (i - (PAGE_COUNT - 1) / 2.0) * 20, -34);
  }
}

/* ===================== Setup / loop ====================================== */
void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(250); Serial.print('.'); }
  Serial.println(WiFi.isConnected() ? WiFi.localIP().toString() : "WiFi timeout");

  Board *board = new Board();
  board->init(); board->begin();
  lvgl_port_init(board->getLCD(), board->getTouch());

  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachHalfQuad(ENC_A_PIN, ENC_B_PIN);
  encoder.clearCount();
  pinMode(ENC_BTN_PIN, INPUT_PULLUP);

  lvgl_port_lock(-1);
  buildUI();
  lvgl_port_unlock();
  show_page(0);

  // all networking on core 0 so the input loop (core 1) never blocks
  xTaskCreatePinnedToCore(netTask, "net", 12288, NULL, 1, NULL, 0);
}

void loop() {
  static int64_t last_enc = 0;
  int64_t e = encoder.getCount() / ENC_DIVISOR;
  if (e != last_enc) {
    int dir = (e > last_enc) ? 1 : -1;
    last_enc = e;
    show_page(cur_page + dir);
  }

  static bool last_btn = HIGH;
  static uint32_t btn_t = 0;
  bool b = digitalRead(ENC_BTN_PIN);
  if (b == LOW && last_btn == HIGH && millis() - btn_t > 250) {
    btn_t = millis();
    if (cur_page == 2) cam_refresh_req = true;
    g_action_press = true;
  }
  last_btn = b;

  delay(5);   // input only; networking lives on the other core
}
