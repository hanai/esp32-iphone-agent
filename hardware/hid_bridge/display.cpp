// Status screen for the M5 StickS3 (240x135 LCD, landscape). One task owns the panel and the two buttons;
// it only reads public getters of the other modules (ble_link.h, netcfg.h, video_link.h, ...), never their
// state, so merging upstream changes should not touch this file. See display.h.
//
//   KEY1  next page            KEY2  show / hide the Wi-Fi password (auto-hides after REVEAL_MS)
//   Page 0 "Link": BLE / Wi-Fi / Mirror / USB lights, who drives the phone, video rate, warnings
//   Page 1 "Wi-Fi": mode, SSID, IP, stations; password and a join-QR only while revealed
#include "display.h"

#if HAS_LCD
#include <M5Unified.h>
#include "ble_link.h"
#include "cli_link.h"
#include "netcfg.h"
#include "version.h"
#include "video_link.h"
#include "src/airplay/airplay.h"
#include "src/hid/arbiter.h"
#include "tusb.h"

namespace {
constexpr uint32_t REFRESH_MS = 500;
constexpr uint32_t REVEAL_MS = 10000;
constexpr uint8_t BRIGHTNESS = 80;  // the phone powers the board: keep the backlight modest
enum Page { PAGE_LINK, PAGE_WIFI, PAGE_COUNT };

const uint16_t C_OK = TFT_GREEN, C_WAIT = TFT_YELLOW, C_BAD = TFT_RED, C_OFF = 0x4208 /* dark grey */;

M5Canvas *cv;  // off-screen frame in PSRAM, pushed in one go: no flicker

// Value of a string field in net_json()'s flat JSON ("" if absent). net_json is the stable public API;
// netcfg.cpp's own parser is private, hence this small copy.
String jsonField(const String &j, const char *key) {
  String k = String("\"") + key + "\":\"";
  int i = j.indexOf(k);
  if (i < 0) return "";
  String out;
  for (i += k.length(); i < (int)j.length() && j[i] != '"'; i++) {
    if (j[i] == '\\' && i + 1 < (int)j.length()) i++;
    out += j[i];
  }
  return out;
}

// WIFI:T:WPA;S:<ssid>;P:<pass>;; the iPhone camera joins from it. \ ; , : " are escaped.
String wifiQr(const String &ssid, const String &pass) {
  auto esc = [](const String &s) {
    String o;
    for (char c : s) {
      if (strchr("\\;,:\"", c)) o += '\\';
      o += c;
    }
    return o;
  };
  return "WIFI:T:WPA;S:" + esc(ssid) + ";P:" + esc(pass) + ";;";
}

void light(int x, int y, uint16_t color, const char *label) {
  cv->fillCircle(x, y, 7, color);
  cv->setTextColor(TFT_WHITE);
  cv->setTextDatum(top_center);
  cv->drawString(label, x, y + 12);
}

void title(const char *name) {
  cv->setTextDatum(top_left);
  cv->setTextColor(TFT_CYAN);
  cv->setFont(&fonts::Font2);
  cv->drawString(name, 4, 2);
  cv->setTextDatum(top_right);
  cv->setTextColor(TFT_DARKGREY);
  cv->drawString("v" FW_VERSION, cv->width() - 4, 2);
  cv->drawFastHLine(0, 20, cv->width(), C_OFF);
}

void drawLink() {
  title("Link");
  bool ap = strcmp(net_mode(), "sta") != 0 && strcmp(net_mode(), "none") != 0;
  bool wifiOk = ap ? net_peers() > 0 : net_up();
  int air = airplay_state();
  bool usbOn = tud_mounted();
  uint16_t ble = ble_app_connected() ? C_OK : ble_connected() ? C_WAIT : C_OFF;
  uint16_t wifi = wifiOk ? C_OK : (ap || net_up()) ? C_WAIT : C_BAD;
  uint16_t mirror = air == AIRPLAY_STREAMING ? C_OK : air == AIRPLAY_CONNECTED ? C_WAIT : C_OFF;
  uint16_t usb = !usbOn ? C_BAD : tud_suspended() ? C_WAIT : C_OK;
  const int w = cv->width() / 4;
  cv->setFont(&fonts::Font2);
  light(w / 2, 40, ble, "BLE");
  light(w + w / 2, 40, wifi, ap ? "AP" : "WiFi");
  light(2 * w + w / 2, 40, mirror, "Mirror");
  light(3 * w + w / 2, 40, usb, "USB");

  VideoStats v = video_stats(FROM_APP);
  const char *owner = arb_owner_name(arb_owner(ble_app_active(), cli_slot() >= 0));
  cv->setTextDatum(top_left);
  cv->setTextColor(TFT_WHITE);
  cv->drawString(String("Driver: ") + owner, 4, 84);
  cv->drawString(String("Video: ") + String(v.fps, 1) + " fps  " + v.kbps + " kbps", 4, 102);
  if (v.frozen) {
    cv->setTextColor(C_BAD);
    cv->drawString("FROZEN: no new frame", 4, 118);
  } else if (ble_pair_window_open()) {
    cv->setTextColor(C_WAIT);
    cv->drawString("Pairing window open", 4, 118);
  }
}

void drawWifi(bool reveal) {
  title("Wi-Fi");
  String j = net_json(false);  // never with the video token (vtok)
  const char *mode = net_mode();
  cv->setFont(&fonts::Font2);
  cv->setTextDatum(top_left);
  cv->setTextColor(TFT_WHITE);
  cv->drawString(!strcmp(mode, "sta") ? "Mode: join Wi-Fi" : !strcmp(mode, "none") ? "Mode: joining..." : "Mode: hotspot",
                 4, 26);
  cv->drawString("SSID: " + jsonField(j, "ssid"), 4, 44);
  cv->drawString("IP:   " + net_ip() + "  (" + net_peers() + ")", 4, 62);
  if (!reveal) {
    cv->setTextColor(TFT_DARKGREY);
    cv->drawString("Pass: ********", 4, 80);
    cv->drawString("KEY2: show password", 4, 112);
    return;
  }
  String pass = jsonField(j, "pass");
  cv->setTextColor(TFT_YELLOW);
  cv->drawString("Pass: " + pass, 4, 80);
  if (!strcmp(mode, "ap")) {  // a QR for the hotspot only: in station mode the phone knows the network already
    cv->fillRect(cv->width() - 100, 24, 100, 100, TFT_WHITE);
    cv->qrcode(wifiQr(jsonField(j, "ssid"), pass).c_str(), cv->width() - 98, 26, 96, 1, true);
  }
  cv->setTextColor(TFT_DARKGREY);
  cv->drawString("KEY2: hide", 4, 112);
}

void task(void *) {
  auto cfg = M5.config();
  cfg.serial_baudrate = 0;  // the sketch owns USB serial
  cfg.output_power = false;  // no 5 V on the Grove/hat rail
  cfg.internal_imu = cfg.internal_rtc = cfg.internal_mic = cfg.internal_spk = false;
  M5.begin(cfg);
  M5.Display.setRotation(1);  // landscape; use 3 for the other way up if it is upside down on your board
  M5.Display.setBrightness(BRIGHTNESS);
  static M5Canvas canvas(&M5.Display);
  canvas.setPsram(true);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
  cv = &canvas;

  int page = PAGE_LINK;
  uint32_t revealUntil = 0, drawnAt = 0;
  bool dirty = true;
  for (;;) {
    M5.update();
    uint32_t now = millis();
    bool reveal = revealUntil && (int32_t)(revealUntil - now) > 0;
    if (revealUntil && !reveal) { revealUntil = 0; dirty = true; }
    if (M5.BtnA.wasClicked()) {
      page = (page + 1) % PAGE_COUNT;
      revealUntil = 0;
      dirty = true;
    }
    if (M5.BtnB.wasClicked()) {
      revealUntil = reveal ? 0 : now + REVEAL_MS;
      if (!reveal) page = PAGE_WIFI;
      dirty = true;
    }
    if (dirty || now - drawnAt >= REFRESH_MS) {
      cv->fillScreen(TFT_BLACK);
      if (page == PAGE_WIFI) drawWifi(revealUntil != 0);
      else drawLink();
      cv->pushSprite(0, 0);
      drawnAt = now;
      dirty = false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
}  // namespace

void display_begin() { xTaskCreatePinnedToCore(task, "lcd", 8192, nullptr, 1, nullptr, 1); }
#endif  // HAS_LCD
