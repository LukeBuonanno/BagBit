#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>
#include <math.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

#define TFT_CS   11
#define TFT_DC   10
#define TFT_MOSI 35
#define TFT_SCLK 36
#define TFT_RST  12
#define BTN_PIN  45
#define X_OFFSET 0
#define Y_OFFSET 26
#define DISP_W   160
#define DISP_H   80
#define TX(x) ((x)+X_OFFSET)
#define TY(y) ((y)+Y_OFFSET)

// WiFi credentials 
const char *WIFI_SSID     = "CHANGEME";
const char *WIFI_PASS     = "CHANGEME";

// Weather target
#define WX_LAT   "CHANGEME"
#define WX_LON   "CHANGEME"

#define WX_URL \
  "https://api.open-meteo.com/v1/forecast" \
  "?latitude=" WX_LAT "&longitude=" WX_LON \
  "&current=temperature_2m,apparent_temperature,weather_code," \
             "windspeed_10m,relative_humidity_2m" \
  "&daily=precipitation_probability_max" \
  "&temperature_unit=fahrenheit&wind_speed_unit=mph" \
  "&forecast_days=1&timezone=America%2FNew_York"

// NTP 
#define NTP_SERVER1  "pool.ntp.org"
#define NTP_SERVER2  "time.nist.gov"

// Change this to -4*3600 for EDT or -5*3600 for EST as appropriate.
#define GMT_OFFSET   (-4 * 3600)   // UTC-4  (EDT)

// Fish / Bubble structs
struct Fish {
  float x, y, vx, vy;
  float px, py;
  uint8_t type;
  uint8_t hue;
  uint8_t radius;
};
struct Bubble    { float x,y; bool active; uint8_t radius; };
struct Blob      { float x,y,vx,vy; uint8_t hue; };
struct MatrixCol { int16_t head; uint8_t speed,len; };
struct DVDBox    { float x,y,px,py,vx,vy; uint8_t hue,w,h; bool pValid; };
struct StarPt    { float x,y,z,pz; int16_t sx,sy,osx,osy; };

Adafruit_ST7735 tft=Adafruit_ST7735(TFT_CS,TFT_DC,TFT_MOSI,TFT_SCLK,TFT_RST);
inline uint16_t C(uint8_t r,uint8_t g,uint8_t b){return tft.color565(b,g,r);}

Preferences prefs;
uint8_t W=DISP_W,H=DISP_H;

// =============================================================================
// STATE MACHINE
// =============================================================================
enum AppState{
  STATE_SCREENSAVER,STATE_MENU,STATE_CLOCK,STATE_SETTINGS,
  STATE_GAMES_MENU,STATE_PONG,STATE_CLICKER,STATE_WEATHER
};
AppState appState=STATE_SCREENSAVER;
#define TOTAL_ANIMS   9
#define ANIM_IDX_CLOCK   7
#define ANIM_IDX_WEATHER 8
uint8_t  currentAnim=0;
uint32_t animStart=0;
const uint32_t ANIM_DURATION=25000;
uint8_t cycleEnabled = 1;
uint8_t savedAnim    = 0;
bool     ssRunning=false;

const char *menuItems[]={"Clock","Screensaver","Games","Settings","Exit"};
const uint8_t MENU_COUNT=5;
uint8_t  menuIndex=0;
uint8_t  menuPrevIndex=255;
uint32_t menuIdleTime=0;
const uint32_t MENU_TIMEOUT=30000;

static volatile uint32_t isrDownAt  = 0;
static volatile bool     isrGotDown = false;
static bool     btnActive    = false;
static uint32_t btnPressedAt = 0;
static bool     btnLongFired = false;
const uint32_t DEBOUNCE_MS        = 20;
const uint32_t LONG_PRESS_MS      = 600;
const uint32_t SHOP_NEXT_MS       = 1000;
const uint32_t SHOP_TAB_MS        = 3000;
const uint32_t CLICKER_EXIT_MS    = 3000;

static bool shopNextFired = false;

void IRAM_ATTR btnISR() {
    uint32_t now = millis();
    if (now - isrDownAt >= 20) { isrDownAt=now; isrGotDown=true; }
}

// ─── Clock state ─────────────────────────────────────────────────────────────
uint32_t clockBaseMillis=0;
uint8_t  clockHour=12,clockMin=0,clockSec=0;

#define BAT_PIN     4
#define BAT_FULL_MV  2000
#define BAT_EMPTY_MV 1190
uint8_t  batPercent=100;
uint32_t batLastRead=100;
#define BAT_READ_INTERVAL 10000

uint8_t settingIndex=0;
const uint8_t N_SETTINGS=3;
const char *settingNames[]={"Set Hour","Set Minute","Auto-Cycle"};
uint8_t settingVals[3]={12,0,0};

const char *animNames[]={
  "Aquarium","Lava Lamp","Plasma","Starfield","Rain","Bounce","Matrix","Halogen Clock","Weather"
};

// =============================================================================
// ─── WEATHER DATA ────────────────────────────────────────────────────────────
// =============================================================================
struct WeatherData {
  float   tempF;
  float   feelsLikeF;
  float   windMph;
  uint8_t humidity;
  uint8_t precipPct;
  int16_t weatherCode;
  bool    valid;
  uint32_t fetchedAt;
};
static WeatherData wxData = {0,0,0,0,0,0,false,0};

const char* wmoDescription(int16_t code){
  if(code==0)  return "Clear Sky";
  if(code==1)  return "Mainly Clear";
  if(code==2)  return "Partly Cloudy";
  if(code==3)  return "Overcast";
  if(code<=49) return "Foggy";
  if(code<=59) return "Drizzle";
  if(code<=69) return "Rainy";
  if(code<=79) return "Snowy";
  if(code<=82) return "Rain Showers";
  if(code<=84) return "Snow Showers";
  if(code<=99) return "Thunderstorm";
  return "Unknown";
}
uint8_t wmoIcon(int16_t code){
  if(code==0||code==1) return 0;
  if(code==2||code==3) return 1;
  if(code<=49)         return 5;
  if(code<=69)         return 2;
  if(code<=79)         return 3;
  if(code<=82)         return 2;
  if(code<=84)         return 3;
  return 4;
}

// =============================================================================
// ─── WiFi / Time / Weather functions ─────────────────────────────────────────
// =============================================================================
bool getTimeViaHTTP(){
  HTTPClient h;
  h.begin("http://detectportal.firefox.com/");
  h.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char* keys[]={ "Date" };
  h.collectHeaders(keys,1);
  h.GET();
  String date=h.header("Date");
  h.end();
  if(date.length()<25) return false;
  const char *months="JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4]; int d,y,hh,mm,ss;
  sscanf(date.c_str(),"%*s %d %3s %d %d:%d:%d",&d,mon,&y,&hh,&mm,&ss);
  struct tm t={};
  t.tm_mday=(int)d; t.tm_mon=(int)((strstr(months,mon)-months)/3);
  t.tm_year=y-1900; t.tm_hour=hh; t.tm_min=mm; t.tm_sec=ss;
  t.tm_isdst=0;
  time_t epoch=mktime(&t);
  epoch += GMT_OFFSET;
  struct tm *lt=localtime(&epoch);
  if(!lt) return false;
  clockHour=(uint8_t)lt->tm_hour;
  clockMin =(uint8_t)lt->tm_min;
  clockSec =(uint8_t)lt->tm_sec;
  clockBaseMillis=millis();
  Serial.printf("HTTP time: %02d:%02d:%02d\n",clockHour,clockMin,clockSec);
  return true;
}

// FIX: Parse weather_code (not weathercode) from the "current" block.
bool fetchWeather(){
  WiFiClientSecure sc; sc.setInsecure();
  HTTPClient h;
  h.begin(sc, WX_URL);
  h.setTimeout(8000);
  int code=h.GET();
  if(code!=200){
    Serial.printf("Weather HTTP error: %d\n", code);
    h.end(); return false;
  }
  String payload=h.getString();
  h.end();

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc,payload);
  if(err){
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return false;
  }

  // All fields are under "current" when using &current= parameter
  JsonObject cur = doc["current"];
  wxData.tempF      = cur["temperature_2m"]      | 0.0f;
  wxData.feelsLikeF = cur["apparent_temperature"] | 0.0f;
  wxData.windMph    = cur["windspeed_10m"]         | 0.0f;
  wxData.humidity   = (uint8_t)(cur["relative_humidity_2m"] | 0);
  // Open-Meteo v1 uses "weather_code" in the current block
  wxData.weatherCode = cur["weather_code"] | 0;

  JsonObject daily = doc["daily"];
  if(daily && daily["precipitation_probability_max"].is<JsonArray>()){
    wxData.precipPct = (uint8_t)(daily["precipitation_probability_max"][0] | 0);
  } else {
    wxData.precipPct = 0;
  }
  wxData.valid = true;
  wxData.fetchedAt = millis();
  Serial.printf("WX: %.1fF feels %.1fF wind %.1fmph hum %d%% precip %d%% code %d\n",
    wxData.tempF, wxData.feelsLikeF, wxData.windMph,
    wxData.humidity, wxData.precipPct, wxData.weatherCode);
  return true;
}

void wifiSync(){
  Serial.println("WiFi sync start...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASS);
  uint8_t retry=0;
  while(WiFi.status()!=WL_CONNECTED && retry<40){
    delay(400); Serial.print('.'); retry++;
  }
  if(WiFi.status()!=WL_CONNECTED){
    Serial.println("\nWiFi failed");
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    return;
  }
  Serial.println("\nWiFi OK");
  configTime(GMT_OFFSET,0,NTP_SERVER1,NTP_SERVER2);
  delay(4000);
  struct tm ti={};
  bool ntpOK=getLocalTime(&ti,1000);
  if(ntpOK){
    clockHour=(uint8_t)ti.tm_hour;
    clockMin =(uint8_t)ti.tm_min;
    clockSec =(uint8_t)ti.tm_sec;
    clockBaseMillis=millis();
    Serial.printf("NTP time: %02d:%02d:%02d\n",clockHour,clockMin,clockSec);
  } else {
    Serial.println("NTP blocked, trying HTTP Date...");
    getTimeViaHTTP();
  }
  if(!fetchWeather()) Serial.println("Weather fetch failed");
  WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  Serial.println("WiFi off");
}

// =============================================================================
// UTILITY
// =============================================================================
uint16_t hsv(uint8_t h,uint8_t s,uint8_t v){
  uint8_t r,g,b;
  if(!s){r=g=b=v;}
  else{
    uint8_t reg=h/43,rem=(h-reg*43)*6;
    uint8_t p=(v*(255-s))>>8;
    uint8_t q=(v*(255-((s*rem)>>8)))>>8;
    uint8_t t_=(v*(255-((s*(255-rem))>>8)))>>8;
    switch(reg){
      case 0:r=v;g=t_;b=p;break; case 1:r=q;g=v;b=p;break;
      case 2:r=p;g=v;b=t_;break; case 3:r=p;g=q;b=v;break;
      case 4:r=t_;g=p;b=v;break; default:r=v;g=p;b=q;break;
    }
  }
  return C(r,g,b);
}
uint32_t lcg_state=12345;
uint32_t lcg(){lcg_state=lcg_state*1664525+1013904223;return lcg_state;}
int16_t  rnd(int16_t lo,int16_t hi){return lo+(int16_t)(lcg()%(uint32_t)(hi-lo+1));}
int8_t isin(uint8_t a){
  static const int8_t tbl[64]={
    0,13,25,37,49,60,71,81,90,98,106,112,117,122,125,126,
    127,126,125,122,117,112,106,98,90,81,71,60,49,37,25,13,
    0,-13,-25,-37,-49,-60,-71,-81,-90,-98,-106,-112,-117,-122,-125,-126,
    -127,-126,-125,-122,-117,-112,-106,-98,-90,-81,-71,-60,-49,-37,-25,-13
  };
  return tbl[a&63];
}

#define SPI_BEGIN() tft.startWrite()
#define SPI_END()   tft.endWrite()

inline void drawPixelC(int16_t x,int16_t y,uint16_t col){
  if((uint16_t)x>=W||(uint16_t)y>=H)return;
  tft.writePixel(TX(x),TY(y),col);
}
inline void drawBigPixel(int16_t x,int16_t y,uint16_t col){
  if(x<0||y<0||x+1>=W||y+1>=H)return;
  tft.writeFillRect(TX(x),TY(y),2,2,col);
}
inline void fillTriangleC(int16_t x0,int16_t y0,int16_t x1,int16_t y1,int16_t x2,int16_t y2,uint16_t col){
  tft.fillTriangle(TX(x0),TY(y0),TX(x1),TY(y1),TX(x2),TY(y2),col);
}
inline void fillRectC(int16_t x,int16_t y,int16_t w,int16_t h,uint16_t col){
  if(x>=(int16_t)W||y>=(int16_t)H||x+w<=0||y+h<=0)return;
  int16_t x2=x+w<(int16_t)W?x+w:(int16_t)W;
  int16_t y2=y+h<(int16_t)H?y+h:(int16_t)H;
  if(x<0)x=0;if(y<0)y=0;
  if(x2>x&&y2>y)tft.writeFillRect(TX(x),TY(y),x2-x,y2-y,col);
}
inline void fillCircleC(int16_t x,int16_t y,uint8_t r,uint16_t col){tft.fillCircle(TX(x),TY(y),r,col);}
inline void drawCircleC(int16_t x,int16_t y,uint8_t r,uint16_t col){tft.drawCircle(TX(x),TY(y),r,col);}
inline void drawLineC(int16_t x0,int16_t y0,int16_t x1,int16_t y1,uint16_t col){tft.writeLine(TX(x0),TY(y0),TX(x1),TY(y1),col);}
inline void drawFastVLineC(int16_t x,int16_t y,int16_t h,uint16_t col){
  if((uint16_t)x>=W)return;
  int16_t y2=y+h<(int16_t)H?y+h:(int16_t)H;
  if(y<0)y=0;if(y2>y)tft.writeFastVLine(TX(x),TY(y),y2-y,col);
}
inline void drawFastHLineC(int16_t x,int16_t y,int16_t w,uint16_t col){
  if((uint16_t)y>=H)return;
  int16_t x2=x+w<(int16_t)W?x+w:(int16_t)W;
  if(x<0)x=0;if(x2>x)tft.writeFastHLine(TX(x),TY(y),x2-x,col);}
inline void fillScreenC(uint16_t col){tft.fillRect(TX(0),TY(0),W,H,col);}

void batterySetup(){
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);
}
uint8_t readBattery(){
  uint32_t sum=0;
  for(uint8_t i=0;i<32;i++) sum+=analogRead(BAT_PIN);
  uint32_t raw=sum/32;
  uint32_t mv=(raw*3300)/4095;
  return (uint8_t)constrain(map(mv, BAT_EMPTY_MV, BAT_FULL_MV, 0, 100), 0, 100);
}

void drawBattery(uint8_t pct, uint16_t bgCol){
  const int16_t BX = 2;
  const int16_t BY = 2;
  const int16_t BW = 20;
  const int16_t BH = 9;
  const int16_t NUB_W = 2;
  const int16_t NUB_H = 4;
  uint16_t col;
  if(pct > 50)      col = C(80, 220, 80);
  else if(pct > 20) col = C(255, 180, 0);
  else              col = C(255, 40, 40);
  SPI_BEGIN();
  tft.writeFillRect(TX(BX - 1), TY(BY - 1), BW + NUB_W + 26, BH + 2, bgCol);
  tft.drawRect(TX(BX), TY(BY), BW, BH, col);
  tft.writeFillRect(TX(BX + BW), TY(BY + (BH - NUB_H)/2), NUB_W, NUB_H, col);
  tft.writeFillRect(TX(BX + 1), TY(BY + 1), BW - 2, BH - 2, C(0, 0, 0));
  uint8_t fw = (uint8_t)(((uint16_t)(BW - 2) * pct) / 100);
  if(fw > 0) tft.writeFillRect(TX(BX + 1), TY(BY + 1), fw, BH - 2, col);
  char buf[5];
  sprintf(buf, "%d%%", pct);
  tft.setTextSize(1);
  tft.setTextColor(col);
  tft.setCursor(TX(BX + BW + NUB_W + 3), TY(BY + 1));
  tft.print(buf);
  SPI_END();
}

void loadPrefs(){
  prefs.begin("device",true);
  settingVals[0] = prefs.getUChar("hr",  12);
  settingVals[1] = prefs.getUChar("mn",   0);
  settingVals[2] = prefs.getUChar("cyc",  1);
  savedAnim      = prefs.getUChar("sa",   0);
  prefs.end();
  cycleEnabled = settingVals[2];
}
void savePrefs(){
  prefs.begin("device",false);
  prefs.putUChar("hr",  settingVals[0]);
  prefs.putUChar("mn",  settingVals[1]);
  prefs.putUChar("cyc", settingVals[2]);
  prefs.putUChar("sa",  savedAnim);
  prefs.end();
  cycleEnabled = settingVals[2];
}

// =============================================================================
// 0: AQUARIUM
// =============================================================================
#define N_FISH    5
#define N_BUBBLES 7
Fish   fish[N_FISH];
Bubble bubbles[N_BUBBLES];
uint32_t aquariumLast = 0;
#define AQUA_BG C(0,10,40)

void drawFish(Fish &f, uint16_t col) {
  int16_t bx = (int16_t)f.x;
  int16_t by = (int16_t)f.y;
  uint8_t r  = f.radius;
  uint8_t tr = r + 2;
  fillCircleC(bx, by, r, col);
  if (f.vx >= 0) {
    int16_t tx = bx - r;
    fillTriangleC(tx, by, tx - tr, by - tr, tx - tr, by + tr, col);
    fillTriangleC(bx - r/2, by - r, bx + r/2, by - r, bx, by - r - r, col);
  } else {
    int16_t tx = bx + r;
    fillTriangleC(tx, by, tx + tr, by - tr, tx + tr, by + tr, col);
    fillTriangleC(bx - r/2, by - r, bx + r/2, by - r, bx, by - r - r, col);
  }
  if (col != AQUA_BG) {
    int16_t ex = (f.vx >= 0) ? bx + r - 1 : bx - r + 1;
    drawPixelC(ex, by - 1, C(255,255,255));
  }
}

void aquariumSetup() {
  fillScreenC(AQUA_BG);
  SPI_BEGIN();
  for (uint8_t x = 0; x < W; x++) {
    uint8_t sandH = 5 + (isin(x*11)+127)/35;
    drawFastVLineC(x, H-sandH, sandH, C(160,120,20));
  }
  SPI_END();
  uint8_t radii[N_FISH] = {4, 4, 4, 6, 3};
  for (uint8_t i = 0; i < N_FISH; i++) {
    fish[i].radius = radii[i];
    uint8_t margin = radii[i] + 4;
    fish[i].x   = rnd(margin, W - margin);
    fish[i].y   = rnd(margin + 4, H - margin - 10);
    fish[i].vx  = rnd(0,1) ? 0.5f : -0.5f;
    fish[i].vy  = 0;
    fish[i].hue = i * 50 + 15;
  }
  uint8_t bradii[N_BUBBLES] = {2, 1, 3, 1, 2, 3, 1};
  for (uint8_t i = 0; i < N_BUBBLES; i++) {
    bubbles[i].x      = rnd(4, W-4);
    bubbles[i].y      = rnd(H/2, H-10);
    bubbles[i].active = true;
    bubbles[i].radius = bradii[i];
  }
}

void aquariumLoop() {
  if (millis()-aquariumLast < 80) return;
  aquariumLast = millis();
  SPI_BEGIN();
  for (uint8_t i = 0; i < N_FISH; i++) {
    drawFish(fish[i], AQUA_BG);
    fish[i].vy += sinf(millis()*0.0008f + i*2.1f)*0.08f;
    fish[i].vy *= 0.85f;
    fish[i].x  += fish[i].vx;
    fish[i].y  += fish[i].vy;
    uint8_t margin = fish[i].radius + 4;
    float minX = margin, maxX = W - margin;
    float minY = margin + 2, maxY = H - margin - 8;
    if (fish[i].x < minX || fish[i].x > maxX) fish[i].vx *= -1;
    if (fish[i].y < minY || fish[i].y > maxY) fish[i].vy *= -1;
    fish[i].x = constrain(fish[i].x, minX, maxX);
    fish[i].y = constrain(fish[i].y, minY, maxY);
    drawFish(fish[i], hsv(fish[i].hue, 255, 255));
  }
  for (uint8_t i = 0; i < N_BUBBLES; i++) {
    drawCircleC(bubbles[i].x, (int16_t)bubbles[i].y, bubbles[i].radius, AQUA_BG);
    float rise = (bubbles[i].radius == 3) ? 0.4f : (bubbles[i].radius == 1) ? 0.8f : 0.6f;
    bubbles[i].y -= rise;
    if (bubbles[i].y < 4) {
      bubbles[i].y = H - 12;
      bubbles[i].x = rnd(4, W-4);
    }
    drawCircleC(bubbles[i].x, (int16_t)bubbles[i].y, bubbles[i].radius, C(100,200,255));
  }
  SPI_END();
}

// =============================================================================
// 1: LAVA LAMP
// =============================================================================
#define N_BLOBS    4
#define LAVA_SCALE 4
#define LAVA_GW    (DISP_W/LAVA_SCALE)
#define LAVA_GH    (DISP_H/LAVA_SCALE)
Blob     blobs[N_BLOBS];
uint32_t lavaLast=0;
static uint16_t lavaPrev[LAVA_GW*LAVA_GH];
struct LavaTheme{uint8_t hueBase,hueRange,sat,valMin,valMax;};
static const LavaTheme lavaThemes[]={
  {  0,30,255,120,255},{140,40,240,100,255},{ 70,35,255,100,230},{200,50,230,110,255},
};
static uint8_t lavaThemeIdx=0;
void lavaSetup(){
  fillScreenC(C(0,0,0)); lavaThemeIdx=(lavaThemeIdx+1)%4;
  for(uint8_t i=0;i<N_BLOBS;i++){
    blobs[i].x=rnd(20,W-20); blobs[i].y=rnd(20,H-20);
    blobs[i].vx=(rnd(0,1)?1:-1)*(0.8f+rnd(0,5)*0.15f);
    blobs[i].vy=(rnd(0,1)?1:-1)*(0.6f+rnd(0,5)*0.12f);
    blobs[i].hue=i*(lavaThemes[lavaThemeIdx].hueRange/N_BLOBS);
  }
  memset(lavaPrev,0xFF,sizeof(lavaPrev));
}
void lavaLoop(){
  if(millis()-lavaLast<40)return; lavaLast=millis();
  const LavaTheme &th=lavaThemes[lavaThemeIdx];
  float t=millis()*0.0005f;
  for(uint8_t b=0;b<N_BLOBS;b++){
    blobs[b].vx+=cosf(t+b*1.3f)*0.06f; blobs[b].vy+=sinf(t*1.1f+b*1.7f)*0.07f;
    float spd=sqrtf(blobs[b].vx*blobs[b].vx+blobs[b].vy*blobs[b].vy);
    if(spd>2.0f){blobs[b].vx*=2.0f/spd;blobs[b].vy*=2.0f/spd;}
    if(spd>0&&spd<0.4f){blobs[b].vx*=0.4f/spd;blobs[b].vy*=0.4f/spd;}
    blobs[b].x+=blobs[b].vx; blobs[b].y+=blobs[b].vy;
    if(blobs[b].x<12||blobs[b].x>W-12)blobs[b].vx*=-1;
    if(blobs[b].y<12||blobs[b].y>H-12)blobs[b].vy*=-1;
    blobs[b].x=constrain(blobs[b].x,12.0f,(float)(W-12));
    blobs[b].y=constrain(blobs[b].y,12.0f,(float)(H-12));
  }
  const float BLOB_R2=400.0f,THRESHOLD=1.0f;
  SPI_BEGIN();
  for(int16_t gy=0;gy<H;gy+=LAVA_SCALE) for(int16_t gx=0;gx<W;gx+=LAVA_SCALE){
    float cx=gx+LAVA_SCALE/2.0f,cy=gy+LAVA_SCALE/2.0f;
    float sum=0,maxInf=0; uint8_t dom=0;
    for(uint8_t b=0;b<N_BLOBS;b++){
      float dx=cx-blobs[b].x,dy=cy-blobs[b].y;
      float d2=dx*dx+dy*dy; if(d2<1)d2=1;
      float inf=BLOB_R2/d2; sum+=inf;
      if(inf>maxInf){maxInf=inf;dom=b;}
    }
    uint16_t col;
    if(sum>=THRESHOLD){
      float blend=constrain(sum-THRESHOLD,0.0f,2.0f)/2.0f;
      col=hsv(th.hueBase+blobs[dom].hue,th.sat,(uint8_t)(th.valMin+blend*(th.valMax-th.valMin)));
    }else if(sum>0.35f) col=hsv(th.hueBase,th.sat,(uint8_t)(((sum-0.35f)/0.65f)*60));
    else col=C(0,0,0);
    uint16_t idx=(gy/LAVA_SCALE)*LAVA_GW+(gx/LAVA_SCALE);
    if(col!=lavaPrev[idx]){tft.writeFillRect(TX(gx),TY(gy),LAVA_SCALE,LAVA_SCALE,col);lavaPrev[idx]=col;}
  }
  SPI_END();
}

// =============================================================================
// 2: PLASMA
// =============================================================================
#define PLASMA_PX        2
#define PLASMA_GW (DISP_W/PLASMA_PX)
#define PLASMA_GH (DISP_H/PLASMA_PX)
uint8_t  plasmaT=0;
uint32_t plasmaLast=0;
static uint16_t plasmaPrev[PLASMA_GW*PLASMA_GH];
static uint8_t  plasmaRowY=0;
void plasmaSetup(){
  plasmaT=0; plasmaRowY=0; fillScreenC(C(0,0,0));
  memset(plasmaPrev,0xFF,sizeof(plasmaPrev));
}
void plasmaLoop(){
  uint32_t now=millis();
  if(plasmaRowY==0){if(now-plasmaLast<30)return;plasmaLast=now;plasmaT+=2;}
  uint8_t y=plasmaRowY;
  SPI_BEGIN();
  for(uint8_t x=0;x<W;x+=PLASMA_PX){
    int16_t v=(int16_t)isin((uint8_t)(x*3+plasmaT))
             +(int16_t)isin((uint8_t)(y*4-plasmaT))
             +(int16_t)isin((uint8_t)((x+y)*2+plasmaT));
    uint16_t col=hsv((uint8_t)((v+381)/3),240,(uint8_t)(180+isin((uint8_t)(x*2-y*3+plasmaT*2))/3));
    uint16_t idx=(y/PLASMA_PX)*PLASMA_GW+(x/PLASMA_PX);
    if(col!=plasmaPrev[idx]){tft.writeFillRect(TX(x),TY(y),PLASMA_PX,PLASMA_PX,col);plasmaPrev[idx]=col;}
  }
  SPI_END();
  plasmaRowY+=PLASMA_PX; if(plasmaRowY>=H)plasmaRowY=0;
}

// =============================================================================
// 3: STARFIELD
// =============================================================================
#define N_STARS 55
StarPt   stars[N_STARS];
uint32_t starLast=0;
void starfieldSetup(){
  fillScreenC(C(0,0,0));
  for(uint8_t i=0;i<N_STARS;i++){
    stars[i].x=rnd(-W/2,W/2);stars[i].y=rnd(-H/2,H/2);
    stars[i].z=rnd(1,W);stars[i].pz=stars[i].z;
    stars[i].sx=-1;stars[i].sy=-1;stars[i].osx=-1;stars[i].osy=-1;
  }
}
void starfieldLoop(){
  if(millis()-starLast<20)return;starLast=millis();
  SPI_BEGIN();
  for(uint8_t i=0;i<N_STARS;i++){
    if(stars[i].osx>=0) drawLineC(stars[i].osx,stars[i].osy,stars[i].sx,stars[i].sy,C(0,0,0));
    if(stars[i].sx>=0)  drawPixelC(stars[i].sx,stars[i].sy,C(0,0,0));
    stars[i].pz=stars[i].z;stars[i].z-=2.5f;
    if(stars[i].z<=0){
      stars[i].x=rnd(-W/2,W/2);stars[i].y=rnd(-H/2,H/2);
      stars[i].z=W;stars[i].pz=W;
      stars[i].sx=-1;stars[i].sy=-1;stars[i].osx=-1;stars[i].osy=-1;
      continue;
    }
    int16_t nx=(int16_t)(stars[i].x/stars[i].z*W)+W/2;
    int16_t ny=(int16_t)(stars[i].y/stars[i].z*W)+H/2;
    int16_t ox=(int16_t)(stars[i].x/stars[i].pz*W)+W/2;
    int16_t oy=(int16_t)(stars[i].y/stars[i].pz*W)+H/2;
    stars[i].osx=stars[i].sx;stars[i].osy=stars[i].sy;
    stars[i].sx=nx;stars[i].sy=ny;
    if(nx>=0&&nx<W&&ny>=0&&ny<H){
      uint8_t bri=(uint8_t)map(stars[i].z,0,W,255,40);
      drawLineC(ox,oy,nx,ny,C(bri,bri,bri));
      drawPixelC(nx,ny,C(255,255,255));
    }
  }
  SPI_END();
}

// =============================================================================
// 4: RAIN + CLOUD
// =============================================================================
#define RAIN_CLOUD_H 16
#define RAIN_SKY_COL C(28,35,48)
#define RAIN_GND_COL C(18,22,30)
#define RCOL_W  3
#define RAIN_NCOLS (DISP_W/RCOL_W)
static MatrixCol rainCols[RAIN_NCOLS];
static uint32_t  rainLast=0;
#define RAIN_TOP    RAIN_CLOUD_H
#define RAIN_BOT    (H-4)
inline uint16_t rainSkyAt(int16_t y){
  if(y<RAIN_TOP) return C(22,26,34);
  if(y>=RAIN_BOT) return RAIN_GND_COL;
  uint8_t t=(uint8_t)((y-RAIN_TOP)*255/(RAIN_BOT-RAIN_TOP));
  return C((uint8_t)(28-(uint16_t)10*t/255),(uint8_t)(35-(uint16_t)13*t/255),(uint8_t)(48-(uint16_t)18*t/255));
}
static void drawCloud(){
  SPI_BEGIN();
  for(uint8_t y=0;y<RAIN_CLOUD_H;y++) tft.writeFastHLine(TX(0),TY(y),W,C(22,26,34));
  SPI_END();
  SPI_BEGIN();
  static const uint8_t CX[]={12,28,45,60,75,92,108,122,138,152};
  static const uint8_t CR[]={ 7, 9, 8,10, 7,  9,  8,  7,  9,  6};
  static const uint8_t CY[]={ 9, 7,10, 8,11,  9, 10,  8,  7, 10};
  for(uint8_t i=0;i<10;i++) tft.fillCircle(TX(CX[i]),TY(CY[i]),CR[i],C(38,42,52));
  for(uint8_t i=0;i<10;i++) tft.fillCircle(TX(CX[i]),TY(CY[i]-2),(CR[i]*2)/3,C(55,60,72));
  tft.writeFillRect(TX(0),TY(RAIN_CLOUD_H),W,2,RAIN_SKY_COL);
  SPI_END();
}
void rainSetup(){
  SPI_BEGIN();
  for(uint8_t y=RAIN_TOP;y<RAIN_BOT;y++) tft.writeFastHLine(TX(0),TY(y),W,rainSkyAt(y));
  for(uint8_t y=RAIN_BOT;y<H;y++) tft.writeFastHLine(TX(0),TY(y),W,RAIN_GND_COL);
  SPI_END();
  drawCloud();
  for(uint8_t i=0;i<RAIN_NCOLS;i++){
    rainCols[i].head=(int16_t)rnd(RAIN_TOP,RAIN_BOT);
    rainCols[i].speed=2+rnd(0,4); rainCols[i].len=4+rnd(0,8);
  }
}
void rainLoop(){
  if(millis()-rainLast<35)return;rainLast=millis();
  SPI_BEGIN();
  for(uint8_t i=0;i<RAIN_NCOLS;i++){
    int16_t cx=i*RCOL_W,head=rainCols[i].head;uint8_t len=rainCols[i].len;
    int16_t ey=head-len*RCOL_W;
    if(ey>=RAIN_TOP&&ey<RAIN_BOT) tft.writeFillRect(TX(cx),TY(ey),RCOL_W-1,RCOL_W-1,rainSkyAt(ey));
    else if(ey>=RAIN_BOT&&ey<H) tft.writeFillRect(TX(cx),TY(ey),RCOL_W-1,RCOL_W-1,RAIN_GND_COL);
    for(uint8_t t=1;t<len;t++){
      int16_t ty=head-t*RCOL_W;
      if(ty<RAIN_TOP||ty>=RAIN_BOT) continue;
      uint8_t bright=(uint8_t)map(t,0,len,160,20);
      tft.writeFillRect(TX(cx),TY(ty),RCOL_W-1,RCOL_W-1,C(bright/6,bright/3,bright));
    }
    if(head>=RAIN_TOP&&head<RAIN_BOT) tft.writeFillRect(TX(cx),TY(head),RCOL_W-1,RCOL_W-1,C(200,220,255));
    else if(head>=RAIN_BOT&&head<H) tft.writeFillRect(TX(cx),TY(head),RCOL_W-1,RCOL_W-1,C(60,80,110));
    rainCols[i].head+=rainCols[i].speed;
    if(rainCols[i].head>RAIN_BOT+len*RCOL_W){
      rainCols[i].head=RAIN_TOP-rnd(0,30);
      rainCols[i].len=4+rnd(0,8); rainCols[i].speed=2+rnd(0,4);
    }
  }
  SPI_END();
}

// =============================================================================
// 5: BOUNCING BOXES
// =============================================================================
#define N_DVDS 5
DVDBox   dvds[N_DVDS];
uint32_t dvdLast=0;
void writeRectC(int16_t x,int16_t y,uint8_t w,uint8_t h,uint16_t col){
  tft.writeFastHLine(TX(x),TY(y),w,col);tft.writeFastHLine(TX(x),TY(y+h-1),w,col);
  tft.writeFastVLine(TX(x),TY(y),h,col);tft.writeFastVLine(TX(x+w-1),TY(y),h,col);
}
void dvdSetup(){
  fillScreenC(C(0,0,0));
  uint8_t ws[5]={22,18,14,26,20},hs[5]={14,12,10,16,13};
  for(uint8_t i=0;i<N_DVDS;i++){
    dvds[i].w=ws[i];dvds[i].h=hs[i];
    dvds[i].x=rnd(0,W-dvds[i].w);dvds[i].y=rnd(0,H-dvds[i].h);
    dvds[i].px=dvds[i].x;dvds[i].py=dvds[i].y;
    float spd=0.7f+i*0.15f;
    dvds[i].vx=rnd(0,1)?spd:-spd;dvds[i].vy=rnd(0,1)?spd*0.8f:-spd*0.8f;
    dvds[i].hue=i*51;dvds[i].pValid=true;
  }
}
void dvdLoop(){
  if(millis()-dvdLast<25)return;dvdLast=millis();
  SPI_BEGIN();
  for(uint8_t i=0;i<N_DVDS;i++){
    if(dvds[i].pValid) tft.writeFillRect(TX((int16_t)floorf(dvds[i].px)),TY((int16_t)floorf(dvds[i].py)),dvds[i].w,dvds[i].h,C(0,0,0));
    dvds[i].px=dvds[i].x;dvds[i].py=dvds[i].y;dvds[i].pValid=true;
    dvds[i].x+=dvds[i].vx;dvds[i].y+=dvds[i].vy;
    bool b=false;
    if(dvds[i].x<=0||dvds[i].x>=W-dvds[i].w){dvds[i].vx*=-1;b=true;}
    if(dvds[i].y<=0||dvds[i].y>=H-dvds[i].h){dvds[i].vy*=-1;b=true;}
    if(b)dvds[i].hue+=55;
    dvds[i].x=constrain(dvds[i].x,0.0f,(float)(W-dvds[i].w));
    dvds[i].y=constrain(dvds[i].y,0.0f,(float)(H-dvds[i].h));
    int16_t nx=(int16_t)dvds[i].x,ny=(int16_t)dvds[i].y;
    uint16_t col=hsv(dvds[i].hue,255,255),col2=hsv(dvds[i].hue+40,255,200);
    writeRectC(nx,ny,dvds[i].w,dvds[i].h,col);writeRectC(nx+1,ny+1,dvds[i].w-2,dvds[i].h-2,col2);
    tft.writeFastHLine(TX(nx+2),TY(ny+dvds[i].h/2),dvds[i].w-4,col);
    tft.writeFastVLine(TX(nx+dvds[i].w/2),TY(ny+2),dvds[i].h-4,col);
  }
  SPI_END();
}

// =============================================================================
// 6: MATRIX RAIN
// =============================================================================
#define COL_W 4
MatrixCol matCols[DISP_W/COL_W];
uint32_t  matLast=0;
void matrixSetup(){
  fillScreenC(C(0,0,0));
  for(uint8_t i=0;i<W/COL_W;i++){
    matCols[i].head=rnd(-30,H);matCols[i].speed=2+rnd(0,3);matCols[i].len=10+rnd(0,15);
  }
}
void matrixLoop(){
  if(millis()-matLast<40)return;matLast=millis();
  SPI_BEGIN();
  for(uint8_t i=0;i<W/COL_W;i++){
    uint8_t cx=i*COL_W;int16_t head=matCols[i].head;
    if(head>=0&&head<H) tft.writeFillRect(TX(cx),TY(head),COL_W-1,COL_W-1,C(180,255,180));
    for(uint8_t t=1;t<matCols[i].len;t++){
      int16_t ty=head-t*COL_W;
      if(ty>=0&&ty<H) tft.writeFillRect(TX(cx),TY(ty),COL_W-1,COL_W-1,C(0,(uint8_t)map(t,0,matCols[i].len,220,20),0));
    }
    int16_t ey=head-matCols[i].len*COL_W;
    if(ey>=0&&ey<H) tft.writeFillRect(TX(cx),TY(ey),COL_W-1,COL_W-1,C(0,0,0));
    matCols[i].head+=matCols[i].speed;
    if(matCols[i].head>H+matCols[i].len*COL_W){matCols[i].head=-rnd(5,30);matCols[i].len=10+rnd(0,15);}
  }
  SPI_END();
}

// =============================================================================
// 7: HALOGEN BULB CLOCK — REDESIGNED
// =============================================================================
//
//  Each digit is one large glass bulb (circle) that fills most of its cell.
//  Inside the bulb, the digit number is drawn as a glowing filament:
//    - thick amber strokes to suggest heated tungsten wire
//    - coil/bounce pattern per digit so it looks hand-wound
//  The two colon dots between HH and MM are just two glowing bulb dots.
//  Background: deep black with faint amber scanlines.
//
//  Layout: 4 digit bulbs + 1 colon gap, centred in 160×80
//  Bulb radius: 16px  → bulb diameter 32px
//  Cell width: 34px, colon gap: 12px
//  Total: 4*34 + 12 = 148 → startX = (160-148)/2 = 6
//  startY = (80-32)/2 = 24

#define HB_R        15    // bulb outer radius
#define HB_CELL     34    // spacing between digit centres
#define HB_COLON_W  12    // extra gap between digit 1 and 2 (HH:MM)
#define HB_START_X  ((DISP_W - (4*HB_CELL + HB_COLON_W)) / 2 + HB_R)
#define HB_START_Y  (DISP_H / 2)

// Filament pixel coords for each digit (relative to bulb centre, scale 0-10 grid → -9..+9 px)
// Each digit is a series of (x,y) pairs forming connected strokes.
// Coords are in the range [-8,+8] to fit inside radius 10 with some margin.
// Format: {x0,y0, x1,y1, ...} terminated by {-99,-99}
// We draw lines between successive pairs.

static const int8_t fil_0[] = {
  -4,-7, 4,-7,  4,-7, 6,-4,  6,-4, 6,4,  6,4, 4,7,
   4,7,-4,7,  -4,7,-6,4,  -6,4,-6,-4,  -6,-4,-4,-7,
  -99,-99
};
static const int8_t fil_1[] = {
  0,-8, 0,8,
  -3,8, 3,8,
  -99,-99
};
static const int8_t fil_2[] = {
  -5,-6, 5,-6,  5,-6, 6,-3,  6,-3, 1,0,
   1,0,-5,5,  -5,5, 6,5,
  -99,-99
};
static const int8_t fil_3[] = {
  -5,-6, 5,-6,  5,-6, 2,0,
   2,0, 5,0,   5,0, 6,4,  6,4, 3,7,  3,7,-5,7,
  -99,-99
};
static const int8_t fil_4[] = {
  -5,-7, -5,1,  -5,1, 6,1,
   6,1,  6,-7,  6,-7, 6,7,
  -99,-99
};
static const int8_t fil_5[] = {
   5,-7, -5,-7,  -5,-7, -5,0,  -5,0, 4,0,
   4,0,  6,3,    6,3,   4,7,   4,7,-5,7,
  -99,-99
};
static const int8_t fil_6[] = {
   5,-7, -4,-7,  -4,-7, -6,-3,  -6,-3, -6,4,  -6,4, -4,7,
  -4,7,  4,7,    4,7,   6,4,    6,4,   6,0,   6,0, -4,0,
  -4,0, -6,-3,
  -99,-99
};
static const int8_t fil_7[] = {
  -6,-7, 6,-7,  6,-7, 1,0,  1,0, 0,8,
  -99,-99
};
static const int8_t fil_8[] = {
  -4,-7, 4,-7,  4,-7, 6,-4,  6,-4, 4,-1,  4,-1,-4,-1,
  -4,-1,-6,-4, -6,-4,-4,-7,
  -4,-1,-6,2,  -6,2,-4,7,  -4,7, 4,7,  4,7, 6,2,  6,2, 4,-1,
  -99,-99
};
static const int8_t fil_9[] = {
   4,7,  5,4,    5,4,  6,0,    6,0,  4,-3,   4,-3,  0,-1,
   0,-1,-5,0,   -5,-3, -4,-6,  -4,-6, 4,-6,   4,-6,  6,-3,
   6,-3, 6,0,
  -99,-99
};

static const int8_t *FILAMENTS[10] = {
  fil_0, fil_1, fil_2, fil_3, fil_4,
  fil_5, fil_6, fil_7, fil_8, fil_9
};

static uint8_t hbLastDigits[4] = {0xFF,0xFF,0xFF,0xFF};
static bool    hbLastColon     = false;
static uint32_t hbLast         = 0;

// Draw one digit bulb. cx,cy = bulb centre.
void drawHaloBulb(int16_t cx, int16_t cy, uint8_t digit, bool erase){
  // Bulb glass outer ring — dark amber when off, warm amber when on
  uint16_t glassEdge = erase ? C(0,0,0)     : C(60,28,0);
  uint16_t glassFill = erase ? C(0,0,0)     : C(10,5,0);
  uint16_t glow1     = erase ? C(0,0,0)     : C(80,32,0);    // outer glow ring
  uint16_t glow2     = erase ? C(0,0,0)     : C(130,55,4);   // inner glow
  uint16_t filCol1   = erase ? C(0,0,0)     : C(255,160,30); // bright filament
  uint16_t filCol2   = erase ? C(0,0,0)     : C(255,210,90); // hot core

  SPI_BEGIN();

  // Erase / base fill
  fillCircleC(cx, cy, HB_R + 1, C(0,0,0));

  if(!erase){
    // Outer glow (large dim circle)
    fillCircleC(cx, cy, HB_R + 1, C(25,10,0));
    fillCircleC(cx, cy, HB_R,     C(40,18,0));
    // Glass body
    fillCircleC(cx, cy, HB_R - 1, glassFill);
    // Glass edge ring
    drawCircleC(cx, cy, HB_R - 1, glassEdge);
    drawCircleC(cx, cy, HB_R - 2, glow1);
    drawCircleC(cx, cy, HB_R - 3, glow2);
  }

  // Draw filament strokes
  const int8_t *f = FILAMENTS[digit];
  uint8_t idx = 0;
  while(true){
    int8_t x0 = f[idx]; int8_t y0 = f[idx+1];
    if(x0 == -99) break;
    int8_t x1 = f[idx+2]; int8_t y1 = f[idx+3];
    if(x1 == -99){ idx+=2; continue; }
    // Draw thick filament (3 offset passes for thickness)
    for(int8_t dx=-1;dx<=1;dx++){
      tft.writeLine(TX(cx+x0+dx), TY(cy+y0),
                    TX(cx+x1+dx), TY(cy+y1), filCol1);
    }
    for(int8_t dy=-1;dy<=1;dy++){
      tft.writeLine(TX(cx+x0), TY(cy+y0+dy),
                    TX(cx+x1), TY(cy+y1+dy), filCol1);
    }
    // Bright core line
    tft.writeLine(TX(cx+x0), TY(cy+y0), TX(cx+x1), TY(cy+y1), filCol2);
    idx += 2;
  }

  // Base pin nub at bottom of bulb
  if(!erase){
    tft.writeFillRect(TX(cx-2), TY(cy+HB_R-1), 5, 3, C(30,14,2));
    tft.writeFillRect(TX(cx-1), TY(cy+HB_R+1), 3, 2, C(20,10,0));
  }

  SPI_END();
}

// Draw colon — two glowing bulb dots between digit 1 and 2
void drawHaloColon(bool on){
  // Colon centre X = start of digit 2 centre - HB_COLON_W/2
  // digit centres: d0=HB_START_X, d1=HB_START_X+HB_CELL, d2=HB_START_X+2*HB_CELL+HB_COLON_W
  int16_t cx = HB_START_X + HB_CELL + HB_COLON_W/2 + 1;
  int16_t y1 = HB_START_Y - 6;
  int16_t y2 = HB_START_Y + 6;

  SPI_BEGIN();
  if(on){
    fillCircleC(cx, y1, 3, C(40,18,0));
    fillCircleC(cx, y2, 3, C(40,18,0));
    fillCircleC(cx, y1, 2, C(180,80,10));
    fillCircleC(cx, y2, 2, C(180,80,10));
    fillCircleC(cx, y1, 1, C(255,200,80));
    fillCircleC(cx, y2, 1, C(255,200,80));
    drawPixelC(cx, y1, C(255,240,180));
    drawPixelC(cx, y2, C(255,240,180));
  } else {
    fillCircleC(cx, y1, 3, C(0,0,0));
    fillCircleC(cx, y2, 3, C(0,0,0));
    fillCircleC(cx, y1, 2, C(12,5,0));
    fillCircleC(cx, y2, 2, C(12,5,0));
  }
  SPI_END();
}

// Digit centre X for a given digit index (0=H tens, 1=H units, 2=M tens, 3=M units)
int16_t hbDigitCX(uint8_t idx){
  if(idx < 2) return HB_START_X + idx * HB_CELL;
  // After colon gap
  return HB_START_X + idx * HB_CELL + HB_COLON_W;
}

void haloClockSetup(){
  fillScreenC(C(0,0,0));
  // Subtle amber scanlines for atmosphere
  SPI_BEGIN();
  for(int16_t y=0; y<H; y+=5){
    tft.writeFastHLine(TX(0), TY(y), W, C(5,2,0));
  }
  SPI_END();
  // Force full redraw
  for(uint8_t i=0;i<4;i++) hbLastDigits[i]=0xFF;
  hbLastColon=false; hbLast=0;
}

void getTime(uint8_t &h,uint8_t &m,uint8_t &s);  // forward decl

void haloClockLoop(){
  if(millis()-hbLast < 250) return;
  hbLast = millis();
  uint8_t h,m,s; getTime(h,m,s);
  uint8_t h12 = h%12; if(h12==0) h12=12;
  uint8_t digits[4]={
    (uint8_t)(h12/10),
    (uint8_t)(h12%10),
    (uint8_t)(m/10),
    (uint8_t)(m%10)
  };
  for(uint8_t i=0;i<4;i++){
    if(digits[i] != hbLastDigits[i]){
      drawHaloBulb(hbDigitCX(i), HB_START_Y, digits[i], false);
      hbLastDigits[i] = digits[i];
    }
  }
  bool colonOn = (s & 1)==0;
  if(colonOn != hbLastColon){
    drawHaloColon(colonOn);
    hbLastColon = colonOn;
  }
}

// =============================================================================
// 8: WEATHER SCREEN — REDESIGNED
// =============================================================================
//
//  Clean single-panel layout on deep slate background.
//  Split into two zones:
//    LEFT (x 0..95):  big temperature, description, animated icon cluster
//    RIGHT (x 96..159): 3 stat pills stacked vertically
//
//  Color palette (warm amber + cool slate theme):
//    bg:        #040810  deep navy-black
//    temp:      amber  255,185,40
//    desc:      ice    140,200,255
//    stat bg:   pill   12,20,40
//    wind:      cyan   60,210,200
//    humidity:  green  80,210,120
//    precip:    violet 170,100,240
//    divider:   18,30,55
//
//  Animated icon: 16×16 sprite at top-right of left zone, pulses via wxIconPhase

#define WX_BG          C(4,8,16)
#define WX_DIV         C(18,30,55)
#define WX_TEMP_COL    C(255,185,40)
#define WX_DESC_COL    C(140,200,255)
#define WX_WIND_COL    C(60,210,200)
#define WX_HUM_COL     C(80,210,120)
#define WX_PREC_COL    C(170,100,240)
#define WX_PILL_BG     C(12,20,42)
#define WX_LABEL_COL   C(60,80,110)

static uint32_t wxAnimLast2  = 0;
static uint8_t  wxIconPhase2 = 0;
static bool     wxDrawnOnce2 = false;

void drawWxIcon2(int16_t ix, int16_t iy, uint8_t iconType, uint8_t phase){
  SPI_BEGIN();
  tft.writeFillRect(TX(ix), TY(iy), 16, 16, WX_BG);
  SPI_END();
  uint8_t bri = 150 + (uint8_t)(isin(phase)*70/127);

  switch(iconType){
    case 0: { // Sun
      uint16_t sc = C(bri, (uint8_t)(bri*195/255), 0);
      fillCircleC(ix+8, iy+8, 4, sc);
      // 8 rays
      for(uint8_t a=0;a<8;a++){
        float ang = a * M_PI / 4.0f;
        int16_t x1=(int16_t)(ix+8+cosf(ang)*6);
        int16_t y1=(int16_t)(iy+8+sinf(ang)*6);
        int16_t x2=(int16_t)(ix+8+cosf(ang)*8);
        int16_t y2=(int16_t)(iy+8+sinf(ang)*8);
        SPI_BEGIN(); tft.writeLine(TX(x1),TY(y1),TX(x2),TY(y2),sc); SPI_END();
      }
      break;
    }
    case 1: { // Cloud
      uint16_t cc = C((uint8_t)(bri*150/255),(uint8_t)(bri*160/255),(uint8_t)(bri*175/255));
      fillCircleC(ix+5, iy+9, 4, cc);
      fillCircleC(ix+10,iy+8, 4, cc);
      fillCircleC(ix+8, iy+7, 3, cc);
      SPI_BEGIN(); tft.writeFillRect(TX(ix+3),TY(iy+9),9,4,cc); SPI_END();
      break;
    }
    case 2: { // Rain
      uint16_t cc = C(70,90,110);
      fillCircleC(ix+5,iy+5,3,cc); fillCircleC(ix+10,iy+4,3,cc);
      SPI_BEGIN(); tft.writeFillRect(TX(ix+3),TY(iy+5),8,3,cc); SPI_END();
      uint16_t dc = C((uint8_t)(bri/5),(uint8_t)(bri/2),bri);
      int16_t dxs[]={ix+3,ix+7,ix+11};
      for(uint8_t d=0;d<3;d++){
        int16_t dy=iy+10+((phase/32+d)&1);
        SPI_BEGIN(); tft.writeFastVLine(TX(dxs[d]),TY(dy),4,dc); SPI_END();
      }
      break;
    }
    case 3: { // Snow
      uint16_t fc = C(bri,bri,(uint8_t)min(255,(int)bri+50));
      int16_t fxs[]={ix+3,ix+8,ix+13,ix+3,ix+8,ix+13};
      int16_t fys[]={iy+5,iy+5,iy+5,iy+11,iy+11,iy+11};
      for(uint8_t f=0;f<6;f++){
        SPI_BEGIN();
        tft.writeLine(TX(fxs[f]-1),TY(fys[f]),TX(fxs[f]+1),TY(fys[f]),fc);
        tft.writeLine(TX(fxs[f]),TY(fys[f]-1),TX(fxs[f]),TY(fys[f]+1),fc);
        SPI_END();
      }
      break;
    }
    case 4: { // Thunder
      uint16_t cc=C(55,55,75);
      fillCircleC(ix+5,iy+4,3,cc); fillCircleC(ix+10,iy+3,3,cc);
      SPI_BEGIN(); tft.writeFillRect(TX(ix+3),TY(iy+4),8,3,cc); SPI_END();
      uint16_t bc=C(bri,(uint8_t)(bri*200/255),0);
      SPI_BEGIN();
      tft.writeLine(TX(ix+9),TY(iy+8),TX(ix+6),TY(iy+12),bc);
      tft.writeLine(TX(ix+6),TY(iy+12),TX(ix+9),TY(iy+12),bc);
      tft.writeLine(TX(ix+9),TY(iy+12),TX(ix+6),TY(iy+16),bc);
      SPI_END();
      break;
    }
    case 5: { // Fog
      uint16_t fc=C((uint8_t)(bri*175/255),(uint8_t)(bri*180/255),(uint8_t)(bri*190/255));
      SPI_BEGIN();
      for(uint8_t f=0;f<5;f++){
        uint8_t lw=(f&1)?12:9;
        tft.writeFastHLine(TX(ix+1+(f&1)),TY(iy+2+f*3),lw,fc);
      }
      SPI_END();
      break;
    }
  }
}

// Draw a stat pill: label + value, at position (px, py), width 60, height 17
void drawStatPill(int16_t px, int16_t py, const char *label, const char *val, uint16_t valCol){
  SPI_BEGIN();
  // Pill background
  tft.writeFillRect(TX(px), TY(py), 60, 17, WX_PILL_BG);
  // Subtle border
  tft.drawRect(TX(px), TY(py), 60, 17, C(22,35,65));
  SPI_END();
  // Label
  tft.setTextSize(1);
  tft.setTextColor(WX_LABEL_COL);
  tft.setCursor(TX(px+3), TY(py+2));
  tft.print(label);
  // Value
  tft.setTextColor(valCol);
  tft.setCursor(TX(px+3), TY(py+10));
  tft.print(val);
}

void weatherScreenSetup(){
  fillScreenC(WX_BG);

  // Subtle star field
  SPI_BEGIN();
  for(uint8_t i=0;i<20;i++){
    int16_t sx=rnd(0,W-1), sy=rnd(0,H-1);
    uint8_t br=rnd(30,100);
    tft.writePixel(TX(sx),TY(sy),C(br,br,(uint8_t)min(255,(int)br+20)));
  }
  // Vertical divider between left data and right pills
  tft.writeFastVLine(TX(97), TY(0), H, WX_DIV);
  SPI_END();

  wxDrawnOnce2 = false;
  wxIconPhase2 = 0;
  wxAnimLast2  = 0;
}

void weatherScreenLoop(){
  uint32_t now = millis();
  bool doAnim = (now - wxAnimLast2 >= 100);
  if(doAnim){ wxAnimLast2=now; wxIconPhase2+=8; }

  if(!wxDrawnOnce2 || doAnim){
    uint8_t ico = wxData.valid ? wmoIcon(wxData.weatherCode) : 1;

    // Animate icon always (top-left of left zone)
    if(doAnim) drawWxIcon2(2, 2, ico, wxIconPhase2);

    if(!wxDrawnOnce2){
      if(wxData.valid){
        // Temperature 
        char tbuf[8];
        int tempRounded = (int)wxData.tempF;
        sprintf(tbuf, "%d\xF7""F", tempRounded);

        SPI_BEGIN();
        tft.writeFillRect(TX(0), TY(20), 97, 22, WX_BG);
        SPI_END();
        tft.setTextSize(2);
        tft.setTextColor(WX_TEMP_COL);
        // Centre temperature text in left zone (97px wide)
        uint8_t tlen = strlen(tbuf);
        int16_t tx = (97 - tlen*12) / 2;
        if(tx < 2) tx = 2;
        tft.setCursor(TX(tx), TY(20));
        tft.print(tbuf);

        // Feels like
        char fbuf[14];
        sprintf(fbuf, "feels %d\xF7""F", (int)wxData.feelsLikeF);
        SPI_BEGIN();
        tft.writeFillRect(TX(0), TY(42), 97, 9, WX_BG);
        SPI_END();
        tft.setTextSize(1);
        tft.setTextColor(C(100,140,190));
        int16_t fx = (97 - (int16_t)strlen(fbuf)*6) / 2;
        if(fx < 1) fx = 1;
        tft.setCursor(TX(fx), TY(42));
        tft.print(fbuf);

        // Condition description
        const char *desc = wmoDescription(wxData.weatherCode);
        SPI_BEGIN();
        tft.writeFillRect(TX(0), TY(53), 97, 10, WX_BG);
        SPI_END();
        tft.setTextSize(1);
        tft.setTextColor(WX_DESC_COL);
        int16_t dx = (97 - (int16_t)strlen(desc)*6) / 2;
        if(dx < 1) dx = 1;
        tft.setCursor(TX(dx), TY(53));
        tft.print(desc);

        // Location label
        SPI_BEGIN();
        tft.writeFillRect(TX(0), TY(65), 97, 9, WX_BG);
        SPI_END();
        tft.setTextSize(1);
        tft.setTextColor(C(40,55,80));
        tft.setCursor(TX(2), TY(65));
        tft.print("WB, PA");

        // Right zone
        // Wind pill  (y = 4)
        char wbuf[10];
        sprintf(wbuf, "%.0fmph", wxData.windMph);
        drawStatPill(99, 4,  "WIND", wbuf, WX_WIND_COL);

        // Humidity pill (y = 25)
        char hbuf[8];
        sprintf(hbuf, "%d%%", wxData.humidity);
        drawStatPill(99, 25, "HUM",  hbuf, WX_HUM_COL);

        // Precip pill (y = 46)
        char pbuf[8];
        sprintf(pbuf, "%d%%", wxData.precipPct);
        drawStatPill(99, 46, "RAIN", pbuf, WX_PREC_COL);

        // Time-since label at bottom right
        SPI_BEGIN();
        tft.writeFillRect(TX(99), TY(66), 60, 9, WX_BG);
        SPI_END();
        tft.setTextSize(1);
        tft.setTextColor(C(30,45,70));
        tft.setCursor(TX(101), TY(67));
        tft.print("live wx");

      } else {
        // No data
        SPI_BEGIN();
        tft.writeFillRect(TX(0), TY(20), 97, 55, WX_BG);
        SPI_END();
        tft.setTextSize(1);
        tft.setTextColor(C(80,80,100));
        tft.setCursor(TX(4), TY(32)); tft.print("No data");
        tft.setCursor(TX(4), TY(44)); tft.print("needs WiFi");
      }

      wxDrawnOnce2 = true;
    }
  }
}

// =============================================================================
// CLOCK  (timekeeping helpers)
// =============================================================================
void getTime(uint8_t &h,uint8_t &m,uint8_t &s){
  uint32_t elapsed=(millis()-clockBaseMillis)/1000;
  s=(clockSec+elapsed)%60;
  uint32_t totalMin=clockMin+(clockSec+elapsed)/60;
  m=totalMin%60;h=(uint8_t)((clockHour+totalMin/60)%24);
}

uint8_t clockPrevH=255,clockPrevM=255,clockPrevS=255;
void drawClockScreen(){
  static bool firstClock=true;
  if(firstClock){ haloClockSetup(); firstClock=false; }
  haloClockLoop();
}

uint32_t clockLast=0;
void clockLoop(){if(millis()-clockLast<250)return;clockLast=millis();drawClockScreen();}

// =============================================================================
// SETTINGS
// =============================================================================
void drawSettingsScreen(){
  fillScreenC(C(0,0,0));
  tft.setTextSize(1);tft.setTextColor(C(0,200,255));tft.setCursor(TX(2),TY(2));tft.print("SETTINGS");
  for(uint8_t i=0;i<N_SETTINGS;i++){
    tft.setTextColor(i==settingIndex?C(255,255,0):C(150,150,150));
    tft.setCursor(TX(8),TY(20+i*16));tft.print(settingNames[i]);tft.print(": ");
    if(i==2) tft.print(settingVals[2]?"ON":"OFF");
    else tft.print(settingVals[i]);
    if(i==settingIndex)tft.print(" <");
  }
  drawBattery(batPercent, C(0,0,0));
}

// =============================================================================
// MENU
// =============================================================================
#define MENU_LABEL_Y  (H/2-10)
#define MENU_LABEL_H  20
#define MENU_BAR_BG   C(20,20,60)

void drawMenuFull(){
  fillScreenC(C(0,0,0));
  SPI_BEGIN(); tft.writeFillRect(TX(0),TY(0),W,14,MENU_BAR_BG); SPI_END();
  tft.setTextSize(1); tft.setTextColor(C(0,200,255));
  tft.setCursor(TX(W/2-12), TY(3)); tft.print("MENU");
  drawBattery(batPercent, MENU_BAR_BG);
  menuPrevIndex=255;
}

void drawMenuLabel(){
  SPI_BEGIN();
  tft.writeFillRect(TX(0),TY(MENU_LABEL_Y),W,MENU_LABEL_H,C(0,0,0));
  tft.writeFillRect(TX(0),TY(H-10),W,10,C(0,0,0));
  SPI_END();
  tft.setTextSize(1); tft.setTextColor(C(60,80,60));
  if(menuIndex>0){tft.setCursor(TX(2),TY(H/2-4));tft.print("<");}
  if(menuIndex<MENU_COUNT-1){tft.setCursor(TX(W-8),TY(H/2-4));tft.print(">");}
  tft.setTextSize(2); tft.setTextColor(C(255,255,255));
  const char *label=menuItems[menuIndex];
  tft.setCursor(TX((int16_t)(W/2-(strlen(label)*12)/2)),TY(H/2-8));tft.print(label);
  uint8_t ds=(uint8_t)(W/MENU_COUNT);if(ds>12)ds=12;
  uint8_t sx=W/2-(MENU_COUNT*ds)/2;
  SPI_BEGIN();
  for(uint8_t i=0;i<MENU_COUNT;i++)
    fillCircleC(sx+i*ds,H-6,i==menuIndex?2:1,i==menuIndex?C(0,255,180):C(40,40,60));
  SPI_END();
  menuPrevIndex=menuIndex;
}

// =============================================================================
// GAMES MENU
// =============================================================================
const char *gameItems[]={"Pong","Clicker","Back"};
const uint8_t GAMES_COUNT=3;
uint8_t gameMenuIndex=0;
static uint8_t gamePrevIndex=255;
#define GAMES_BAR_BG C(30,10,60)

void drawGamesMenuFull(){
  fillScreenC(C(0,0,5));
  SPI_BEGIN(); tft.writeFillRect(TX(0),TY(0),W,14,GAMES_BAR_BG); SPI_END();
  tft.setTextSize(1); tft.setTextColor(C(180,0,255));
  tft.setCursor(TX(W/2-15), TY(3)); tft.print("GAMES");
  drawBattery(batPercent, GAMES_BAR_BG);
  gamePrevIndex=255;
}

void drawGamesMenuLabel(){
  SPI_BEGIN();
  tft.writeFillRect(TX(0), TY(MENU_LABEL_Y), W, MENU_LABEL_H, C(0,0,5));
  tft.writeFillRect(TX(0), TY(H-10), W, 10, C(0,0,5));
  SPI_END();
  tft.setTextSize(1); tft.setTextColor(C(60,40,80));
  if(gameMenuIndex>0){ tft.setCursor(TX(2),TY(H/2-4)); tft.print("<"); }
  if(gameMenuIndex<GAMES_COUNT-1){ tft.setCursor(TX(W-8),TY(H/2-4)); tft.print(">"); }
  tft.setTextSize(2); tft.setTextColor(C(200,100,255));
  const char *label=gameItems[gameMenuIndex];
  tft.setCursor(TX((int16_t)(W/2-(strlen(label)*12)/2)), TY(H/2-8));
  tft.print(label);
  uint8_t ds=14, sx=W/2-(GAMES_COUNT*ds)/2;
  SPI_BEGIN();
  for(uint8_t i=0;i<GAMES_COUNT;i++)
    fillCircleC(sx+i*ds,H-6,i==gameMenuIndex?2:1,i==gameMenuIndex?C(180,0,255):C(40,20,60));
  SPI_END();
  gamePrevIndex=gameMenuIndex;
}

void drawGamesMenu(){
  drawGamesMenuFull();
  drawGamesMenuLabel();
}

// =============================================================================
// GAME: PONG
// =============================================================================
#define PONG_PADDLE_W  4
#define PONG_PADDLE_H  14
#define PONG_BALL_R    2
#define PONG_PLAYER_X  6
#define PONG_AI_X      (W - PONG_PADDLE_W - 6)
static const uint16_t PONG_BG = 0x0000;

struct PongState {
  float ballX, ballY, ballVX, ballVY;
  float playerY, aiY;
  float playerVY;
  float aiVY;
  int8_t playerScore, aiScore;
  uint32_t lastUpdate;
  uint8_t  comboHits;
  int16_t prevPlayerY;
  int16_t prevAiY;
  int16_t prevBallX, prevBallY;
  bool    firstDraw;
};
static PongState pong;

void pongErasePaddle(int16_t y, int16_t x){
  if(y < 0) return;
  SPI_BEGIN();
  tft.writeFillRect(TX(x), TY(y), PONG_PADDLE_W, PONG_PADDLE_H, PONG_BG);
  int16_t dashX = W/2 - 1;
  if(dashX >= x && dashX < x + PONG_PADDLE_W){
    for(int16_t dy = y; dy < y + PONG_PADDLE_H; dy += 6)
      tft.writeFillRect(TX(W/2-1), TY(dy), 2, 3, C(40,40,40));
  }
  SPI_END();
}

void pongDrawPaddle(int16_t y, int16_t x){
  SPI_BEGIN();
  tft.writeFillRect(TX(x), TY(y), PONG_PADDLE_W, PONG_PADDLE_H, C(255,255,255));
  SPI_END();
}

void pongEraseBall(int16_t bx, int16_t by){
  if(bx < 0) return;
  int16_t ex = bx - PONG_BALL_R - 1;
  int16_t ey = by - PONG_BALL_R - 1;
  int16_t ew = PONG_BALL_R * 2 + 3;
  int16_t eh = PONG_BALL_R * 2 + 3;
  SPI_BEGIN();
  tft.writeFillRect(TX(ex), TY(ey), ew, eh, PONG_BG);
  for(int16_t dy = ey; dy < ey + eh; dy += 6)
    if(dy >= 10 && dy < H)
      tft.writeFillRect(TX(W/2-1), TY(dy), 2, 3, C(40,40,40));
  SPI_END();
}

void pongResetBall(){
  if(pong.prevBallX >= 0) pongEraseBall(pong.prevBallX, pong.prevBallY);
  if(pong.prevPlayerY >= 0) pongErasePaddle(pong.prevPlayerY, PONG_PLAYER_X);
  if(pong.prevAiY >= 0)     pongErasePaddle(pong.prevAiY,     PONG_AI_X);
  pong.ballX = W/2.0f; pong.ballY = H/2.0f;
  float angle = (rnd(0,1)?1:-1)*(0.3f + rnd(0,6)*0.08f);
  pong.ballVX = (rnd(0,1)?1:-1) * 0.9f;
  pong.ballVY = 0.9f * sinf(angle);
  pong.playerY = H/2.0f - PONG_PADDLE_H/2.0f;
  pong.aiY     = H/2.0f - PONG_PADDLE_H/2.0f;
  pong.playerVY = 1.0f;
  pong.aiVY    = 1.0f;
  pong.comboHits = 0;
  pong.prevPlayerY = -1;
  pong.prevAiY     = -1;
  pong.prevBallX   = -1;
  pong.prevBallY   = -1;
  pong.firstDraw   = true;
}

void pongReset(bool full){
  if(full){ pong.playerScore=0; pong.aiScore=0; }
  pongResetBall();
}

void pongDrawBackground(){
  SPI_BEGIN();
  tft.writeFillRect(TX(0),TY(0),W,H,PONG_BG);
  for(int16_t y=10;y<H;y+=6) tft.writeFillRect(TX(W/2-1),TY(y),2,3,C(40,40,40));
  tft.writeFillRect(TX(0),TY(0),W,10,C(15,15,15));
  SPI_END();
}

void pongDrawScore(){
  SPI_BEGIN();
  tft.writeFillRect(TX(W/4-8),TY(1),20,8,C(15,15,15));
  tft.writeFillRect(TX(3*W/4-8),TY(1),20,8,C(15,15,15));
  tft.setTextSize(1);
  tft.setTextColor(C(255,255,255)); tft.setCursor(TX(W/4-4),TY(2));  tft.print(pong.playerScore);
  tft.setTextColor(C(255,255,255)); tft.setCursor(TX(3*W/4-4),TY(2)); tft.print(pong.aiScore);
  SPI_END();
}

void pongSetup(){
  pong.prevPlayerY = -1;
  pong.prevAiY     = -1;
  pong.prevBallX   = -1;
  pong.prevBallY   = -1;
  pong.playerScore = 0;
  pong.aiScore     = 0;
  pongDrawBackground();
  pongReset(false);
  pongDrawScore();
}

void pongOnButton(bool longPress){
  if(longPress){
    appState = STATE_GAMES_MENU;
    drawGamesMenuFull();
    drawGamesMenuLabel();
  } else {
    pong.playerVY *= -1.0f;
  }
}

void pongLoop(){
  uint32_t now = millis();
  if(now - pong.lastUpdate < 25) return;
  float dt = (now - pong.lastUpdate) / 16.67f;
  pong.lastUpdate = now;

  float pSpeed = 1.1f + pong.comboHits * 0.05f;
  pong.playerY += pong.playerVY * pSpeed * dt;
  if(pong.playerY < 10)                   { pong.playerY = 10;                pong.playerVY =  fabsf(pong.playerVY); }
  if(pong.playerY + PONG_PADDLE_H > H-2)  { pong.playerY = H-2-PONG_PADDLE_H; pong.playerVY = -fabsf(pong.playerVY); }

  float aiSpeed = 1.2f + pong.aiScore * 0.04f;
  aiSpeed = min(aiSpeed, 1.8f);
  pong.aiY += pong.aiVY * aiSpeed * dt;
  if(pong.aiY < 10)                  { pong.aiY = 10;                pong.aiVY =  fabsf(pong.aiVY); }
  if(pong.aiY + PONG_PADDLE_H > H-2) { pong.aiY = H-2-PONG_PADDLE_H; pong.aiVY = -fabsf(pong.aiVY); }
  if(pong.ballVX > 0){
    float paddleMid = pong.aiY + PONG_PADDLE_H / 2.0f;
    float diff = paddleMid - pong.ballY;
    if(pong.aiVY > 0 && diff >  PONG_PADDLE_H / 4.0f) pong.aiVY = -fabsf(pong.aiVY);
    if(pong.aiVY < 0 && diff < -PONG_PADDLE_H / 4.0f) pong.aiVY =  fabsf(pong.aiVY);
  }

  pong.ballX += pong.ballVX * dt;
  pong.ballY += pong.ballVY * dt;
  if(pong.ballY < 11 + PONG_BALL_R){ pong.ballY = 11+PONG_BALL_R; pong.ballVY = fabsf(pong.ballVY); }
  if(pong.ballY > H-1 - PONG_BALL_R){ pong.ballY = H-1-PONG_BALL_R; pong.ballVY = -fabsf(pong.ballVY); }

  if(pong.ballVX < 0 &&
     pong.ballX - PONG_BALL_R <= PONG_PLAYER_X + PONG_PADDLE_W &&
     pong.ballX - PONG_BALL_R >= PONG_PLAYER_X - 2 &&
     pong.ballY >= pong.playerY && pong.ballY <= pong.playerY + PONG_PADDLE_H){
    float relHit = (pong.ballY - (pong.playerY + PONG_PADDLE_H/2.0f)) / (PONG_PADDLE_H/2.0f);
    float speed  = sqrtf(pong.ballVX*pong.ballVX + pong.ballVY*pong.ballVY);
    speed = min(speed + 0.10f, 2.8f);
    float angle  = relHit * 1.1f;
    pong.ballVX  = fabsf(cosf(angle)) * speed;
    pong.ballVY  = sinf(angle) * speed;
    pong.ballX   = PONG_PLAYER_X + PONG_PADDLE_W + PONG_BALL_R + 1;
    pong.comboHits++;
  }
  if(pong.ballVX > 0 &&
     pong.ballX + PONG_BALL_R >= PONG_AI_X &&
     pong.ballX + PONG_BALL_R <= PONG_AI_X + PONG_PADDLE_W + 2 &&
     pong.ballY >= pong.aiY && pong.ballY <= pong.aiY + PONG_PADDLE_H){
    float relHit = (pong.ballY - (pong.aiY + PONG_PADDLE_H/2.0f)) / (PONG_PADDLE_H/2.0f);
    float speed  = sqrtf(pong.ballVX*pong.ballVX + pong.ballVY*pong.ballVY);
    speed = min(speed + 0.08f, 2.8f);
    float angle  = relHit * 1.1f;
    pong.ballVX  = -fabsf(cosf(angle)) * speed;
    pong.ballVY  = sinf(angle) * speed;
    pong.ballX   = PONG_AI_X - PONG_BALL_R - 1;
  }

  bool scored = false;
  if(pong.ballX < 2){ pong.aiScore++; scored=true; }
  if(pong.ballX > W-2){ pong.playerScore++; scored=true; }
  if(scored){ pongDrawScore(); pongResetBall(); return; }

  int16_t curPlayerY = (int16_t)pong.playerY;
  int16_t curAiY     = (int16_t)pong.aiY;
  int16_t curBallX   = (int16_t)pong.ballX;
  int16_t curBallY   = (int16_t)pong.ballY;

  if(pong.prevPlayerY != curPlayerY && pong.prevPlayerY >= 0) pongErasePaddle(pong.prevPlayerY, PONG_PLAYER_X);
  if(pong.prevAiY != curAiY && pong.prevAiY >= 0)             pongErasePaddle(pong.prevAiY,     PONG_AI_X);
  if((pong.prevBallX != curBallX || pong.prevBallY != curBallY) && pong.prevBallX >= 0) pongEraseBall(pong.prevBallX, pong.prevBallY);

  if(pong.prevPlayerY != curPlayerY || pong.firstDraw) pongDrawPaddle(curPlayerY, PONG_PLAYER_X);
  if(pong.prevAiY != curAiY || pong.firstDraw)         pongDrawPaddle(curAiY, PONG_AI_X);

  SPI_BEGIN();
  tft.fillCircle(TX(curBallX), TY(curBallY), PONG_BALL_R, C(255,255,255));
  SPI_END();

  static uint8_t prevCombo = 0;
  if(pong.comboHits != prevCombo || pong.firstDraw){
    SPI_BEGIN();
    tft.writeFillRect(TX(W/2-12), TY(H-8), 28, 8, PONG_BG);
    if(pong.comboHits >= 3){
      tft.setTextSize(1); tft.setTextColor(C(255,255,255));
      tft.setCursor(TX(W/2-8), TY(H-8)); tft.print("x"); tft.print(pong.comboHits);
    }
    SPI_END();
    prevCombo = pong.comboHits;
  }
  pong.prevPlayerY = curPlayerY;
  pong.prevAiY     = curAiY;
  pong.prevBallX   = curBallX;
  pong.prevBallY   = curBallY;
  pong.firstDraw   = false;
}

// =============================================================================
// GAME: CLICKER
// =============================================================================
#define CLK_N_BLDG      5
#define CLK_N_CUPGRADES 5
#define CLK_N_PARTICLES 12
#define CLK_SHOP_TOTAL  (CLK_N_BLDG + CLK_N_CUPGRADES)

struct ClickerBuilding {
  const char *name;
  uint32_t   baseCost;
  uint32_t   cps;
  uint8_t    count;
  uint32_t   cost;
};
struct ClickerParticle {
  int16_t x, y;
  uint8_t life, maxLife;
  uint32_t val;
  bool     active;
};
struct ClickerState {
  uint32_t coins,totalCoins,clickValue,cps,prestigeCount;
  uint8_t  clickUpgrades,tab;
  bool     needFullRedraw,needCoinUpdate;
  uint32_t lastTick,lastSave,lastClickFlash,clickCount;
  uint8_t  comboCount;
  uint32_t lastClickTime;
  uint8_t  goldenChance;
  uint32_t goldenUntil;
  int16_t  goldenX,goldenY;
  uint8_t  shopScroll,shopSelected;
  uint32_t prevCoins,prevCPS,prevClickValue,prevTotalCoins,prevClickCount,prevPrestigeCount;
  uint8_t  prevCombo,prevCoinPhase;
  bool     cookieDrawn,particleSide;
};

static ClickerState clk;
static ClickerBuilding clkBldg[CLK_N_BLDG] = {
  {"Cursor",    15,    1, 0, 15},
  {"Farm",     100,    5, 0, 100},
  {"Mine",     500,   20, 0, 500},
  {"Factory", 2500,   80, 0, 2500},
  {"Reactor",12000,  350, 0, 12000},
};
const char *clkUpgrNames[] = {"Dbl Click","Quad Click","Mega Click","Ultra Click","OMEGA Click"};
const uint32_t clkUpgrCost[]  = {50,  300,  2000,  15000, 100000};
const uint32_t clkUpgrBonus[] = {1,   4,    15,    75,    500};
static ClickerParticle clkParticles[CLK_N_PARTICLES];

#define CLK_COOKIE_CX  (W/2)
#define CLK_COOKIE_CY  54
#define CLK_COOKIE_R   18
#define CLK_INFO_Y     13
#define CLK_SAVE_INTERVAL 30000
#define CLK_PARTICLE_TICKS 20

void clkSaveGame(){
  prefs.begin("clicker",false);
  prefs.putULong("coins",    clk.coins);
  prefs.putULong("total",    clk.totalCoins);
  prefs.putULong("clicks",   clk.clickCount);
  prefs.putULong("prestige", clk.prestigeCount);
  prefs.putUChar("cupgr",    clk.clickUpgrades);
  for(uint8_t i=0;i<CLK_N_BLDG;i++){
    char key[8]; sprintf(key,"b%d",i);
    prefs.putUChar(key, clkBldg[i].count);
    sprintf(key,"bc%d",i);
    prefs.putULong(key, clkBldg[i].cost);
  }
  prefs.end();
  clk.lastSave = millis();
}
void clkLoadGame(){
  prefs.begin("clicker",true);
  clk.coins          = prefs.getULong("coins",    0);
  clk.totalCoins     = prefs.getULong("total",    0);
  clk.clickCount     = prefs.getULong("clicks",   0);
  clk.prestigeCount  = prefs.getULong("prestige", 0);
  clk.clickUpgrades  = prefs.getUChar("cupgr",    0);
  for(uint8_t i=0;i<CLK_N_BLDG;i++){
    char key[8]; sprintf(key,"b%d",i);
    clkBldg[i].count = prefs.getUChar(key, 0);
    sprintf(key,"bc%d",i);
    clkBldg[i].cost  = prefs.getULong(key, clkBldg[i].baseCost);
  }
  prefs.end();
}
void clkComputeCPS(){
  clk.cps = 0;
  for(uint8_t i=0;i<CLK_N_BLDG;i++) clk.cps += (uint32_t)clkBldg[i].count * clkBldg[i].cps;
  clk.cps *= (clk.prestigeCount + 1);
}
void clkComputeClickVal(){
  clk.clickValue = 1;
  for(uint8_t i=0;i<CLK_N_CUPGRADES;i++){
    if(clk.clickUpgrades & (1<<i)) clk.clickValue += clkUpgrBonus[i];
  }
  clk.clickValue *= (clk.prestigeCount + 1);
}
void fmtNum(char *buf, uint32_t n){
  if(n < 1000)         { sprintf(buf,"%lu",n); }
  else if(n < 10000)   { sprintf(buf,"%lu.%luK",(n/1000),(n%1000)/100); }
  else if(n < 1000000) { sprintf(buf,"%luK",n/1000); }
  else if(n < 10000000){ sprintf(buf,"%lu.%luM",(n/1000000),(n%1000000)/100000); }
  else                 { sprintf(buf,"%luM",n/1000000); }
}
void clkSpawnParticle(uint32_t val){
  for(uint8_t i=0;i<CLK_N_PARTICLES;i++){
    if(!clkParticles[i].active){
      bool goLeft = clk.particleSide;
      clk.particleSide = !clk.particleSide;
      int16_t px = goLeft ? rnd(4, 18) : rnd(W - 38, W - 26);
      int16_t py = CLK_COOKIE_CY + rnd(-10, 10);
      clkParticles[i] = {px, py, CLK_PARTICLE_TICKS, CLK_PARTICLE_TICKS, val, true};
      return;
    }
  }
}
void clkSpawnGolden(){
  clk.goldenX = rnd(CLK_COOKIE_CX-CLK_COOKIE_R+4, CLK_COOKIE_CX+CLK_COOKIE_R-4);
  clk.goldenY = rnd(CLK_COOKIE_CY-CLK_COOKIE_R+4, CLK_COOKIE_CY+CLK_COOKIE_R-4);
  clk.goldenUntil = millis() + 5000;
}
bool clkCheckGoldenClick(){
  if(clk.goldenUntil > millis()){
    clk.goldenUntil = 0;
    uint32_t bonus = clk.cps * 10;
    if(bonus < 100) bonus = 100;
    clk.coins += bonus; clk.totalCoins += bonus;
    return true;
  }
  return false;
}
void clkDrawCookie(){
  int16_t cx = CLK_COOKIE_CX;
  int16_t cy = CLK_COOKIE_CY;
  uint8_t r  = CLK_COOKIE_R;
  SPI_BEGIN();
  tft.fillCircle(TX(cx+1), TY(cy+2), r, C(0,0,0));
  tft.fillCircle(TX(cx), TY(cy), r, C(107,50,16));
  tft.fillCircle(TX(cx), TY(cy), r-2, C(191,115,34));
  tft.fillCircle(TX(cx-1), TY(cy-2), r-7, C(209,139,58));
  tft.fillCircle(TX(cx-2), TY(cy-3), r-13, C(223,160,85));
  tft.drawCircle(TX(cx), TY(cy), r-2, C(122,60,18));
  tft.drawCircle(TX(cx), TY(cy), r-1, C(90,42,12));
  tft.fillCircle(TX(cx-8), TY(cy-12), 3, C(26,10,2));
  tft.fillCircle(TX(cx-8), TY(cy-13), 3, C(61,26,6));
  tft.drawPixel( TX(cx-9), TY(cy-14), C(107,72,24));
  tft.fillCircle(TX(cx+2), TY(cy-3),  2, C(26,10,2));
  tft.fillCircle(TX(cx+2), TY(cy-4),  2, C(61,26,6));
  tft.drawPixel( TX(cx+1), TY(cy-5),  C(107,72,24));
  tft.fillCircle(TX(cx+8), TY(cy-10), 3, C(26,10,2));
  tft.fillCircle(TX(cx+8), TY(cy-11), 3, C(61,26,6));
  tft.drawPixel( TX(cx+7), TY(cy-12), C(107,72,24));
  tft.fillCircle(TX(cx-7), TY(cy+8),  3, C(26,10,2));
  tft.fillCircle(TX(cx-7), TY(cy+7),  3, C(61,26,6));
  tft.drawPixel( TX(cx-8), TY(cy+6),  C(107,72,24));
  tft.fillCircle(TX(cx+5), TY(cy+12), 3, C(26,10,2));
  tft.fillCircle(TX(cx+5), TY(cy+11), 3, C(61,26,6));
  tft.drawPixel( TX(cx+4), TY(cy+10), C(107,72,24));
  tft.fillCircle(TX(cx-4), TY(cy+11), 2, C(26,10,2));
  tft.fillCircle(TX(cx-4), TY(cy+10), 2, C(61,26,6));
  tft.drawPixel( TX(cx-5), TY(cy+9),  C(107,72,24));
  tft.fillCircle(TX(cx+10),TY(cy+2),  2, C(26,10,2));
  tft.fillCircle(TX(cx+10),TY(cy+1),  2, C(61,26,6));
  tft.drawPixel( TX(cx+9), TY(cy),    C(107,72,24));
  tft.drawPixel(TX(cx-7),  TY(cy+4),  C(240,216,152));
  tft.drawPixel(TX(cx+8),  TY(cy+4),  C(240,216,152));
  tft.drawPixel(TX(cx+1),  TY(cy-10), C(240,216,152));
  tft.drawPixel(TX(cx-3),  TY(cy+18), C(240,216,152));
  SPI_END();
  clk.cookieDrawn = true;
}

void clkDrawTabBar(){
  uint16_t barBg = C(20,10,40);
  SPI_BEGIN(); tft.writeFillRect(TX(0),TY(0),W,12,barBg); SPI_END();
  tft.setTextSize(1);
  const char *tabs[] = {"CLICK","SHOP","STATS"};
  for(uint8_t i=0;i<3;i++){
    bool active = (i==clk.tab);
    uint16_t col = active ? C(255,200,80) : C(80,60,40);
    tft.setTextColor(col);
    tft.setCursor(TX(6 + i*52), TY(2)); tft.print(tabs[i]);
  }
}
void clkUpdateMainNumbers(){
  uint16_t bgCol = C(5,5,15);
  char buf[16];
  SPI_BEGIN();
  tft.writeFillRect(TX(0), TY(CLK_INFO_Y), W, 18, bgCol);
  fmtNum(buf, clk.coins);
  char coinLine[24]; sprintf(coinLine,"%s cookies", buf);
  int16_t coinX = W/2 - (strlen(coinLine)*6)/2;
  tft.setTextSize(1); tft.setTextColor(C(220,160,80));
  tft.setCursor(TX(coinX), TY(CLK_INFO_Y)); tft.print(coinLine);
  fmtNum(buf, clk.cps);
  char cpsLine[16]; sprintf(cpsLine,"%s/s", buf);
  int16_t cpsX = W/2 - (strlen(cpsLine)*6)/2;
  tft.setTextColor(C(150,100,50));
  tft.setCursor(TX(cpsX), TY(CLK_INFO_Y+9)); tft.print(cpsLine);
  SPI_END();
}
void clkDrawMainTab(){
  uint16_t bgCol = C(5,5,15);
  SPI_BEGIN(); tft.writeFillRect(TX(0),TY(0),W,H,bgCol); SPI_END();
  clkDrawTabBar(); clkUpdateMainNumbers();
  clk.cookieDrawn = false; clkDrawCookie();
  if(clk.prestigeCount > 0){
    SPI_BEGIN();
    tft.writeFillRect(TX(W-22), TY(0), 22, 12, C(40,10,60));
    tft.setTextSize(1); tft.setTextColor(C(200,100,255));
    tft.setCursor(TX(W-20), TY(2)); tft.print("P"); tft.print(clk.prestigeCount);
    SPI_END();
  }
}
void clkPartialMainTab(){
  uint32_t now = millis();
  uint16_t bgCol = C(5,5,15);
  if(clk.coins != clk.prevCoins || clk.cps != clk.prevCPS){
    clkUpdateMainNumbers(); clk.prevCoins = clk.coins; clk.prevCPS=clk.cps;
  }
  if(clk.goldenUntil > now){
    SPI_BEGIN();
    uint8_t gPhase = (uint8_t)(now/6);
    tft.fillCircle(TX(clk.goldenX), TY(clk.goldenY), 4, hsv(gPhase, 200, 255));
    tft.drawCircle(TX(clk.goldenX), TY(clk.goldenY), 4, C(255,220,80));
    SPI_END();
  }
  if(clk.comboCount != clk.prevCombo){
    SPI_BEGIN();
    tft.writeFillRect(TX(2), TY(H-8), 40, 8, bgCol);
    if(clk.comboCount >= 3){
      char cbuf[6]; sprintf(cbuf,"x%d",min(clk.comboCount,(uint8_t)99));
      tft.setTextSize(1); tft.setTextColor(C(220,160,80));
      tft.setCursor(TX(2), TY(H-8)); tft.print(cbuf);
    }
    SPI_END();
    clk.prevCombo = clk.comboCount;
  }
  SPI_BEGIN();
  for(uint8_t i=0;i<CLK_N_PARTICLES;i++){
    if(!clkParticles[i].active) continue;
    clkParticles[i].life--;
    if(clkParticles[i].life == 0){
      tft.writeFillRect(TX(clkParticles[i].x - 1), TY(clkParticles[i].y - 1), 44, 10, bgCol);
      clkParticles[i].active = false; continue;
    }
    tft.setTextSize(1); tft.setTextColor(C(220, 140, 60));
    char pbuf[8]; fmtNum(pbuf, clkParticles[i].val);
    char fullbuf[12]; sprintf(fullbuf,"+%s",pbuf);
    tft.setCursor(TX(clkParticles[i].x), TY(clkParticles[i].y)); tft.print(fullbuf);
  }
  SPI_END();
}
bool clkShopItemCanBuy(uint8_t idx){
  if(idx < CLK_N_BLDG) return clk.coins >= clkBldg[idx].cost;
  uint8_t ui = idx - CLK_N_BLDG;
  return !(clk.clickUpgrades & (1<<ui)) && clk.coins >= clkUpgrCost[ui];
}
bool clkShopItemBought(uint8_t idx){
  if(idx < CLK_N_BLDG) return false;
  uint8_t ui = idx - CLK_N_BLDG;
  return (clk.clickUpgrades & (1<<ui));
}
void clkDrawShopRow(uint8_t slot, uint8_t shopIdx, bool selected){
  int16_t rowY = 22 + slot * 18;
  bool bought = clkShopItemBought(shopIdx);
  bool canBuy = clkShopItemCanBuy(shopIdx);
  char buf[16];
  SPI_BEGIN();
  uint16_t bg = selected ? C(15,10,25) : C(8,5,15);
  tft.writeFillRect(TX(0), TY(rowY), W, 17, bg);
  if(selected) tft.drawRect(TX(0), TY(rowY), W, 17, C(180,100,40));
  tft.setTextSize(1);
  if(shopIdx < CLK_N_BLDG){
    ClickerBuilding &b = clkBldg[shopIdx];
    char nameBuf[24]; sprintf(nameBuf,"%s [%d]", b.name, b.count);
    int16_t nx2 = W/2-(strlen(nameBuf)*6)/2;
    tft.setTextColor(canBuy ? C(220,160,80) : C(100,70,30));
    tft.setCursor(TX(nx2), TY(rowY+1)); tft.print(nameBuf);
    fmtNum(buf, b.cost);
    char cpsBuf[12]; fmtNum(cpsBuf, b.cps*(clk.prestigeCount+1));
    char costBuf[28]; sprintf(costBuf,"%s | +%s/s", buf, cpsBuf);
    int16_t costX = W/2-(strlen(costBuf)*6)/2;
    tft.setTextColor(canBuy ? C(180,120,50) : C(80,50,20));
    tft.setCursor(TX(costX), TY(rowY+9)); tft.print(costBuf);
  } else {
    uint8_t ui = shopIdx - CLK_N_BLDG;
    int16_t unx = W/2-(strlen(clkUpgrNames[ui])*6)/2;
    if(bought){
      tft.setTextColor(C(80,60,30)); tft.setCursor(TX(unx), TY(rowY+1)); tft.print(clkUpgrNames[ui]);
      tft.setTextColor(C(100,80,30)); tft.setCursor(TX(W/2-9), TY(rowY+9)); tft.print("OWNED");
    } else {
      tft.setTextColor(canBuy ? C(220,160,80) : C(100,70,30));
      tft.setCursor(TX(unx), TY(rowY+1)); tft.print(clkUpgrNames[ui]);
      fmtNum(buf, clkUpgrCost[ui]);
      char bonBuf[10]; fmtNum(bonBuf, clkUpgrBonus[ui]);
      char costBuf[28]; sprintf(costBuf,"%s | +%s/clk", buf, bonBuf);
      int16_t costX = W/2-(strlen(costBuf)*6)/2;
      tft.setTextColor(canBuy ? C(180,120,50) : C(80,50,20));
      tft.setCursor(TX(costX), TY(rowY+9)); tft.print(costBuf);
    }
  }
  SPI_END();
}
void clkDrawNextRow(uint8_t slot, bool selected){
  int16_t rowY = 22 + slot * 18;
  SPI_BEGIN();
  uint16_t bg = selected ? C(15,10,25) : C(8,5,15);
  tft.writeFillRect(TX(0), TY(rowY), W, 17, bg);
  if(selected) tft.drawRect(TX(0), TY(rowY), W, 17, C(180,100,40));
  tft.setTextSize(1);
  tft.setTextColor(selected ? C(220,160,80) : C(100,70,30));
  const char *nxt = "-- BACK TO TOP -->";
  tft.setCursor(TX(W/2-(strlen(nxt)*6)/2), TY(rowY+5)); tft.print(nxt);
  SPI_END();
}
void clkDrawShopTab(){
  SPI_BEGIN(); tft.writeFillRect(TX(0),TY(0),W,H,C(5,5,15)); SPI_END();
  clkDrawTabBar();
  char buf[16]; fmtNum(buf, clk.coins);
  char coinLine[24]; sprintf(coinLine,"%s cookies", buf);
  int16_t cx2 = W/2-(strlen(coinLine)*6)/2;
  tft.setTextSize(1); tft.setTextColor(C(220,160,80));
  tft.setCursor(TX(cx2), TY(CLK_INFO_Y)); tft.print(coinLine);
  for(uint8_t slot=0; slot<3; slot++){
    uint8_t shopIdx = clk.shopScroll + slot;
    bool selected = (shopIdx == clk.shopSelected);
    if(shopIdx == CLK_SHOP_TOTAL){ clkDrawNextRow(slot, selected); break; }
    if(shopIdx >= CLK_SHOP_TOTAL) break;
    clkDrawShopRow(slot, shopIdx, selected);
  }
  SPI_BEGIN(); tft.writeFillRect(TX(0), TY(H-8), W, 8, C(5,5,15)); SPI_END();
  clk.prevCoins = clk.coins;
}
static void clkStatsLine(const char *label, const char *val, int16_t y, uint16_t col){
  SPI_BEGIN(); tft.writeFillRect(TX(0), TY(y), W, 9, C(5,5,15)); SPI_END();
  char line[32]; sprintf(line,"%s%s",label,val);
  int16_t lx = DISP_W/2-(strlen(line)*6)/2;
  tft.setTextSize(1); tft.setTextColor(col);
  tft.setCursor(TX(lx),TY(y)); tft.print(line);
}
void clkDrawStatsTab(){
  SPI_BEGIN(); tft.writeFillRect(TX(0),TY(0),W,H,C(5,5,15)); SPI_END();
  clkDrawTabBar();
  char buf[16];
  fmtNum(buf, clk.totalCoins);  clkStatsLine("Baked: ",buf,CLK_INFO_Y,C(220,160,80));
  fmtNum(buf, clk.clickCount);  clkStatsLine("Clicks: ",buf,CLK_INFO_Y+10,C(180,120,60));
  fmtNum(buf, clk.cps);         clkStatsLine("CPS: ",buf,CLK_INFO_Y+20,C(160,110,50));
  char pbuf[8]; sprintf(pbuf,"%lu",clk.prestigeCount);
  clkStatsLine("Prestige: ",pbuf,CLK_INFO_Y+30,C(200,140,80));
  char mbuf[8]; sprintf(mbuf,"x%lu",(clk.prestigeCount+1));
  clkStatsLine("Bonus: ",mbuf,CLK_INFO_Y+40,C(150,100,50));
  SPI_BEGIN(); tft.writeFillRect(TX(0), TY(H-16), W, 9, C(5,5,15)); SPI_END();
  if(clk.totalCoins >= 1000000){
    const char *pm="[ CLICK = PRESTIGE! ]";
    tft.setTextSize(1); tft.setTextColor(C(255,180,60));
    tft.setCursor(TX(W/2-(strlen(pm)*6)/2),TY(H-16)); tft.print(pm);
  } else {
    tft.setTextSize(1); tft.setTextColor(C(80,50,20));
    fmtNum(buf, 1000000);
    char need[20]; sprintf(need,"Need %s",buf);
    tft.setCursor(TX(W/2-(strlen(need)*6)/2),TY(H-16)); tft.print(need);
  }
  clk.prevCoins=clk.coins; clk.prevCPS=clk.cps;
  clk.prevTotalCoins=clk.totalCoins; clk.prevClickCount=clk.clickCount;
}
void clkRedraw(){
  clk.needFullRedraw = false;
  switch(clk.tab){
    case 0: clkDrawMainTab();  break;
    case 1: clkDrawShopTab();  break;
    case 2: clkDrawStatsTab(); break;
  }
  clk.prevCoins=clk.coins; clk.prevCPS=clk.cps;
  clk.prevClickValue=clk.clickValue; clk.prevCombo=clk.comboCount;
}
void clkSetup(){
  clk = {};
  clk.tab = 0; clk.needFullRedraw = true;
  clk.lastTick=millis(); clk.lastSave=millis();
  clk.goldenChance=0; clk.goldenUntil=0;
  clk.shopScroll=0; clk.shopSelected=0;
  clk.prevCoins=0xFFFFFFFF; clk.prevCPS=0xFFFFFFFF;
  clk.prevClickValue=0xFFFFFFFF; clk.prevCombo=0xFF;
  clk.prevCoinPhase=0xFF; clk.cookieDrawn=false; clk.particleSide=false;
  for(uint8_t i=0;i<CLK_N_BLDG;i++){ clkBldg[i].count=0; clkBldg[i].cost=clkBldg[i].baseCost; }
  for(uint8_t i=0;i<CLK_N_PARTICLES;i++) clkParticles[i].active=false;
  clkLoadGame(); clkComputeCPS(); clkComputeClickVal(); clkRedraw();
}
void clkOnShortPress(){
  uint32_t now = millis();
  switch(clk.tab){
    case 0: {
      if(now - clk.lastClickTime < 800) { if(clk.comboCount<255) clk.comboCount++; }
      else clk.comboCount = 1;
      clk.lastClickTime = now;
      clkCheckGoldenClick();
      uint32_t earned = clk.clickValue;
      if(clk.comboCount >= 5)  earned = earned * 3 / 2;
      if(clk.comboCount >= 15) earned = earned * 2;
      clk.coins += earned; clk.totalCoins += earned; clk.clickCount++;
      clkSpawnParticle(earned);
      clk.goldenChance++;
      if(clk.goldenChance >= 40 + rnd(0,30)){ clk.goldenChance=0; clkSpawnGolden(); }
      clk.needCoinUpdate = true;
      break;
    }
    case 1: {
      uint8_t idx = clk.shopSelected;
      if(idx == CLK_SHOP_TOTAL){ clk.shopScroll=0; clk.shopSelected=0; clk.needFullRedraw=true; break; }
      if(idx < CLK_N_BLDG){
        ClickerBuilding &b = clkBldg[idx];
        if(clk.coins >= b.cost){
          clk.coins -= b.cost; b.count++;
          b.cost = (uint32_t)(b.cost * 1.15f + 1);
          clkComputeCPS();
          uint8_t slot = idx - clk.shopScroll;
          clkDrawShopRow(slot, idx, true);
          SPI_BEGIN(); tft.writeFillRect(TX(0), TY(CLK_INFO_Y), W, 8, C(5,5,15));
          char buf[16]; fmtNum(buf, clk.coins);
          char coinLine[24]; sprintf(coinLine,"%s cookies", buf);
          int16_t cx2 = W/2-(strlen(coinLine)*6)/2;
          tft.setTextSize(1); tft.setTextColor(C(220,160,80));
          tft.setCursor(TX(cx2), TY(CLK_INFO_Y)); tft.print(coinLine); SPI_END();
          clk.prevCoins = clk.coins;
        }
      } else {
        uint8_t ui = idx - CLK_N_BLDG;
        if(!(clk.clickUpgrades & (1<<ui)) && clk.coins >= clkUpgrCost[ui]){
          clk.coins -= clkUpgrCost[ui]; clk.clickUpgrades |= (1<<ui);
          clkComputeClickVal();
          uint8_t slot = idx - clk.shopScroll;
          clkDrawShopRow(slot, idx, true);
          SPI_BEGIN(); tft.writeFillRect(TX(0), TY(CLK_INFO_Y), W, 8, C(5,5,15));
          char buf[16]; fmtNum(buf, clk.coins);
          char coinLine[24]; sprintf(coinLine,"%s cookies", buf);
          int16_t cx2 = W/2-(strlen(coinLine)*6)/2;
          tft.setTextSize(1); tft.setTextColor(C(220,160,80));
          tft.setCursor(TX(cx2), TY(CLK_INFO_Y)); tft.print(coinLine); SPI_END();
          clk.prevCoins = clk.coins;
        }
      }
      break;
    }
    case 2: {
      if(clk.totalCoins >= 1000000){
        clk.prestigeCount++;
        clk.coins=0; clk.totalCoins=0; clk.clickCount=0; clk.clickUpgrades=0;
        for(uint8_t i=0;i<CLK_N_BLDG;i++){ clkBldg[i].count=0; clkBldg[i].cost=clkBldg[i].baseCost; }
        clkComputeCPS(); clkComputeClickVal(); clkSaveGame();
      }
      clk.tab=0; clk.needFullRedraw=true;
      break;
    }
  }
}
void clkOnNextItem(){
  if(clk.tab != 1) return;
  uint8_t oldSelected=clk.shopSelected;
  uint8_t oldSlot=oldSelected-clk.shopScroll;
  clk.shopSelected++;
  if(clk.shopSelected > CLK_SHOP_TOTAL){ clk.shopSelected=0; clk.shopScroll=0; clk.needFullRedraw=true; return; }
  if(clk.shopSelected > clk.shopScroll + 2){ clk.shopScroll=clk.shopSelected-2; clk.needFullRedraw=true; return; }
  uint8_t newSlot=clk.shopSelected-clk.shopScroll;
  if(oldSelected==CLK_SHOP_TOTAL) clkDrawNextRow(oldSlot,false);
  else if(oldSelected<CLK_SHOP_TOTAL) clkDrawShopRow(oldSlot,oldSelected,false);
  if(clk.shopSelected==CLK_SHOP_TOTAL) clkDrawNextRow(newSlot,true);
  else clkDrawShopRow(newSlot,clk.shopSelected,true);
}
void clkOnLongPress(){
  if(clk.tab==1){ clk.shopScroll=0; clk.shopSelected=0; clk.needFullRedraw=true; return; }
  if(clk.tab==2){ clkSaveGame(); appState=STATE_GAMES_MENU; drawGamesMenuFull(); drawGamesMenuLabel(); }
  else { clk.tab=(clk.tab+1)%3; clk.needFullRedraw=true; }
}
void clkLoop(){
  uint32_t now=millis();
  if(now-clk.lastTick>=1000){
    uint32_t ticks=(now-clk.lastTick)/1000;
    clk.lastTick+=ticks*1000;
    uint32_t earned=clk.cps*ticks;
    if(earned>0){ clk.coins+=earned; clk.totalCoins+=earned; clk.needCoinUpdate=true; }
    if(now-clk.lastClickTime>2000) clk.comboCount=0;
  }
  if(now-clk.lastSave>=CLK_SAVE_INTERVAL) clkSaveGame();
  if(clk.goldenUntil>0&&now>clk.goldenUntil){ clk.goldenUntil=0; clk.needFullRedraw=true; }
  if(clk.needFullRedraw){ clkRedraw(); clk.needFullRedraw=false; clk.needCoinUpdate=false; return; }
  switch(clk.tab){
    case 0: clkPartialMainTab(); break;
    case 1:
      if(clk.needCoinUpdate){
        char buf[16]; SPI_BEGIN();
        tft.writeFillRect(TX(0), TY(CLK_INFO_Y), W, 8, C(5,5,15));
        fmtNum(buf, clk.coins);
        char coinLine[24]; sprintf(coinLine,"%s cookies", buf);
        int16_t cx2=W/2-(strlen(coinLine)*6)/2;
        tft.setTextSize(1); tft.setTextColor(C(220,160,80));
        tft.setCursor(TX(cx2), TY(CLK_INFO_Y)); tft.print(coinLine); SPI_END();
        clk.prevCoins=clk.coins;
      }
      break;
    case 2: {
      char buf[16];
      bool prestigeChanged=(clk.prestigeCount!=clk.prevPrestigeCount);
      if(clk.totalCoins!=clk.prevTotalCoins||prestigeChanged){
        fmtNum(buf,clk.totalCoins); clkStatsLine("Baked: ",buf,CLK_INFO_Y,C(220,160,80));
        clk.prevTotalCoins=clk.totalCoins;
      }
      if(clk.clickCount!=clk.prevClickCount||prestigeChanged){
        fmtNum(buf,clk.clickCount); clkStatsLine("Clicks: ",buf,CLK_INFO_Y+10,C(180,120,60));
        clk.prevClickCount=clk.clickCount;
      }
      if(clk.cps!=clk.prevCPS||prestigeChanged){
        fmtNum(buf,clk.cps); clkStatsLine("CPS: ",buf,CLK_INFO_Y+20,C(160,110,50));
        clk.prevCPS=clk.cps;
      }
      if(prestigeChanged){
        char pbuf[8]; sprintf(pbuf,"%lu",clk.prestigeCount);
        clkStatsLine("Prestige: ",pbuf,CLK_INFO_Y+30,C(200,140,80));
        char mbuf[8]; sprintf(mbuf,"x%lu",(clk.prestigeCount+1));
        clkStatsLine("Bonus: ",mbuf,CLK_INFO_Y+40,C(150,100,50));
        SPI_BEGIN(); tft.writeFillRect(TX(0),TY(H-16),W,9,C(5,5,15)); SPI_END();
        if(clk.totalCoins>=1000000){
          const char *pm="[ CLICK = PRESTIGE! ]";
          tft.setTextSize(1); tft.setTextColor(C(255,180,60));
          tft.setCursor(TX(W/2-(strlen(pm)*6)/2),TY(H-16)); tft.print(pm);
        } else {
          tft.setTextSize(1); tft.setTextColor(C(80,50,20));
          fmtNum(buf,1000000);
          char need[20]; sprintf(need,"Need %s",buf);
          tft.setCursor(TX(W/2-(strlen(need)*6)/2),TY(H-16)); tft.print(need);
        }
        clk.prevPrestigeCount=clk.prestigeCount;
      }
      static bool statsBottomWasEligible=false;
      bool nowEligible=(clk.totalCoins>=1000000);
      if(nowEligible!=statsBottomWasEligible&&!prestigeChanged){
        SPI_BEGIN(); tft.writeFillRect(TX(0),TY(H-16),W,9,C(5,5,15)); SPI_END();
        if(nowEligible){
          const char *pm="[ CLICK = PRESTIGE! ]";
          tft.setTextSize(1); tft.setTextColor(C(255,180,60));
          tft.setCursor(TX(W/2-(strlen(pm)*6)/2),TY(H-16)); tft.print(pm);
        } else {
          tft.setTextSize(1); tft.setTextColor(C(80,50,20));
          fmtNum(buf,1000000);
          char need[20]; sprintf(need,"Need %s",buf);
          tft.setCursor(TX(W/2-(strlen(need)*6)/2),TY(H-16)); tft.print(need);
        }
        statsBottomWasEligible=nowEligible;
      }
      break;
    }
  }
  clk.needCoinUpdate=false;
}

// =============================================================================
// ANIM SWITCHING
// =============================================================================
typedef void(*AnimFn)();
AnimFn setupFns[]={
  aquariumSetup, lavaSetup,    plasmaSetup,  starfieldSetup,
  rainSetup,     dvdSetup,     matrixSetup,
  haloClockSetup,
  weatherScreenSetup
};
AnimFn loopFns[] ={
  aquariumLoop,  lavaLoop,     plasmaLoop,   starfieldLoop,
  rainLoop,      dvdLoop,      matrixLoop,
  haloClockLoop,
  weatherScreenLoop
};

void switchAnim(uint8_t n,bool force){
  uint8_t next=n%TOTAL_ANIMS;
  if(!force&&next==currentAnim){animStart=millis();return;}
  currentAnim=next;
  clockPrevH=clockPrevM=clockPrevS=255;
  fillScreenC(C(0,0,0));
  setupFns[currentAnim]();
  animStart=millis(); ssRunning=true;
}
void advanceAnim(){
  if(cycleEnabled) switchAnim(currentAnim+1,true);
  else animStart=millis();
}

void menuSelect(){
  if(menuIndex==0){
    currentAnim=ANIM_IDX_CLOCK;
    appState=STATE_SCREENSAVER;
    fillScreenC(C(0,0,0));
    haloClockSetup();
    animStart=millis();
  } else if(menuIndex==1){
    appState=STATE_SCREENSAVER; switchAnim(savedAnim,true); animStart=millis();
  } else if(menuIndex==2){
    appState=STATE_GAMES_MENU; gameMenuIndex=0;
    drawGamesMenuFull(); drawGamesMenuLabel();
  } else if(menuIndex==3){
    appState=STATE_SETTINGS; settingIndex=0; drawSettingsScreen();
  } else {
    appState=STATE_SCREENSAVER; switchAnim(savedAnim,true); animStart=millis();
  }
}

void menuLoop(){
  if(millis()-menuIdleTime>MENU_TIMEOUT){
    fillScreenC(C(0,0,0));
    appState=STATE_SCREENSAVER;
    switchAnim(savedAnim,true);
    animStart=millis();
  }
}

// =============================================================================
// BUTTON HANDLER
// =============================================================================
void handleButton() {
    uint32_t now    = millis();
    bool     pinLow = (digitalRead(BTN_PIN) == LOW);

    noInterrupts();
    bool     gotDown = isrGotDown; isrGotDown = false;
    uint32_t downAt  = isrDownAt;
    interrupts();

    if (gotDown && !btnActive) {
      btnActive    = true;
      btnPressedAt = downAt;
      btnLongFired = false;
      shopNextFired = false;
    }

    uint32_t heldMs = btnActive ? (now - btnPressedAt) : 0;

    uint32_t longThreshold;
    if (appState == STATE_CLICKER && clk.tab == 2) longThreshold = CLICKER_EXIT_MS;
    else longThreshold = LONG_PRESS_MS;

    if (btnActive && !btnLongFired && pinLow && heldMs >= longThreshold) {
        btnLongFired = true;

        if (appState == STATE_SCREENSAVER || appState == STATE_CLOCK) {
            appState = STATE_MENU; menuIndex = 0; menuIdleTime = now;
            drawMenuFull(); drawMenuLabel();
        } else if (appState == STATE_MENU) {
            menuIdleTime = now; menuSelect();
        } else if (appState == STATE_SETTINGS) {
            if (settingIndex < N_SETTINGS - 1) { settingIndex++; drawSettingsScreen(); }
            else {
                clockHour=settingVals[0]; clockMin=settingVals[1]; clockSec=0;
                clockBaseMillis=now; cycleEnabled=settingVals[2]; savePrefs();
                appState=STATE_MENU; menuIndex=0; menuIdleTime=now;
                drawMenuFull(); drawMenuLabel();
            }
        } else if (appState == STATE_GAMES_MENU) {
            if(gameMenuIndex==0){ appState=STATE_PONG; pongSetup(); }
            else if(gameMenuIndex==1){ appState=STATE_CLICKER; clkSetup(); }
            else { appState=STATE_MENU; menuIndex=0; menuIdleTime=now; drawMenuFull(); drawMenuLabel(); }
        } else if (appState == STATE_PONG) {
            pongOnButton(true);
        } else if (appState == STATE_CLICKER) {
            clkOnLongPress();
        }
    }

    if (btnActive && !pinLow && (now - btnPressedAt >= DEBOUNCE_MS)) {
        btnActive = false;
        if (!btnLongFired) {
            if (appState == STATE_SCREENSAVER) {
                switchAnim(currentAnim + 1, true); animStart = now;
                savedAnim = currentAnim;
                prefs.begin("device", false); prefs.putUChar("sa", savedAnim); prefs.end();
            } else if (appState == STATE_MENU) {
                menuIndex = (menuIndex + 1) % MENU_COUNT; menuIdleTime = now; drawMenuLabel();
            } else if (appState == STATE_SETTINGS) {
                if      (settingIndex == 0) settingVals[0] = (settingVals[0]+1) % 24;
                else if (settingIndex == 1) settingVals[1] = (settingVals[1]+1) % 60;
                else                        settingVals[2]  = settingVals[2] ? 0 : 1;
                drawSettingsScreen();
            } else if (appState == STATE_CLOCK) {
                appState=STATE_MENU; menuIndex=0; menuIdleTime=now;
                drawMenuFull(); drawMenuLabel();
            } else if (appState == STATE_GAMES_MENU) {
                gameMenuIndex = (gameMenuIndex + 1) % GAMES_COUNT; drawGamesMenuLabel();
            } else if (appState == STATE_PONG) {
                pongOnButton(false);
            } else if (appState == STATE_CLICKER) {
                clkOnShortPress();
            }
        }
        btnLongFired  = false;
        shopNextFired = false;
    }
}

// =============================================================================
// SETUP / LOOP
// =============================================================================
void setup(){
  Serial.begin(115200);
  batterySetup(); batPercent=100; delay(100); batPercent=readBattery();
  clockBaseMillis=millis();
  pinMode(BTN_PIN,INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), btnISR, FALLING);
  tft.setSPISpeed(27000000UL);
  tft.initR(INITR_BLACKTAB); tft.invertDisplay(true); tft.setRotation(1);
  lcg_state=esp_random();
  loadPrefs();
  clockHour=settingVals[0]; clockMin=settingVals[1]; clockSec=0;
  currentAnim=savedAnim;

  tft.fillScreen(C(0,0,0));
  tft.setTextSize(1); tft.setTextColor(C(80,140,200));
  tft.setCursor(TX(30),TY(32)); tft.print("Syncing time...");
  wifiSync();

  switchAnim(currentAnim,true);
}

void loop(){
  handleButton();
  if(millis()-batLastRead>BAT_READ_INTERVAL){ batLastRead=millis(); batPercent=readBattery(); }
  switch(appState){
    case STATE_SCREENSAVER:
      if(millis()-animStart>ANIM_DURATION) advanceAnim();
      loopFns[currentAnim]();
      break;
    case STATE_MENU:
      menuLoop();
      break;
    case STATE_CLOCK:
      clockLoop();
      break;
    case STATE_SETTINGS:
      break;
    case STATE_GAMES_MENU:
      break;
    case STATE_PONG:
      pongLoop();
      break;
    case STATE_CLICKER:
      clkLoop();
      break;
    case STATE_WEATHER:
      weatherScreenLoop();
      break;
  }
}
