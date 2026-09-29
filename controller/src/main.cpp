// DGX rack fan controller
//
// Board   : LOLIN S3 (ESP32-S3-WROOM-1)
// Fans    : 2x Noctua NF-* 120mm 4-pin PWM (12 V)  -- or 1, see FAN_COUNT
// Sensors : 2x InLine 36219I -- 10k NTC thermistor @ 25 C, 2-pin
//
// Fan N follows sensor N (see ZONE_MODE). A sensor that reads at either ADC
// rail is treated as unplugged/shorted: it is reported as FAULT and the fan
// it controls is stopped. The fans never run at full speed on their own.
//
// A latching on/off switch (in = fans on, out = fans off) and the board's 0
// button for the profile: single press Normal <-> Quiet, double press Max,
// long press back to Normal. The onboard RGB LED shows the profile, or amber
// while the switch is off. Every power-up starts in Normal.
//
// Optional Wi-Fi: a status page at http://dgx-fans.local/ with profile buttons
// (password-protected) and firmware updates over the network. Credentials are
// entered once over USB and kept in flash. Fan control never waits on Wi-Fi.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
#include "driver/pulse_cnt.h"
#else
#include "driver/pcnt.h"
#endif

// Firmware version: shown on the web page, in /api/status and on the console.
// Bump it with every change that gets flashed.
static const char *const FW_VERSION = "1.1.0";

// ------------------------------------------------------------- fan count --
// One fan + probe per GX10. Set in platformio.ini (fan_count), not here. With
// 1, only fan 1 / probe 1 exist: the fan-2 pins are never touched, and the
// web page, status JSON and console show a single fan.
#ifndef FAN_COUNT
#define FAN_COUNT 2
#endif
static_assert(FAN_COUNT == 1 || FAN_COUNT == 2, "FAN_COUNT must be 1 or 2");
static const int NFANS = FAN_COUNT;

// ---------------------------------------------------------------- pin map --
// Verify against the silkscreen before wiring. Avoided here: GPIO0/3/45/46
// (strapping), 19/20 (USB), 26-37 (flash + octal PSRAM), 43/44 (UART0).
static const int PIN_NTC[2]  = {1, 2};    // ADC1_CH0 / ADC1_CH1
static const int PIN_PWM[2]  = {11, 12};
static const int PIN_TACH[2] = {13, 14};

// Profile button: the board's own "0" button (GPIO0, active low, has its own
// pull-up). Reading GPIO0 after boot is fine; it only acts as a strapping pin
// when RST is pressed while it is held.
static const int  PIN_BUTTON        = 0;
static const bool BUTTON_ACTIVE_LOW = true;

// On/off switch: DFRobot DFR0789 Gravity LED Switch, self-locking, D -> GPIO
// 21, + -> 3V3, - -> GND; pressed in (lit) reads HIGH. The internal pull-down
// makes an unwired switch read "off".
static const int  PIN_POWER_SWITCH         = 21;
static const bool POWER_SWITCH_ACTIVE_HIGH = true;

// ------------------------------------------------------------ thermistors --
static const float NTC_R25       = 10000.0f;  // ohms at 25 C (36219I datasheet)
static const float NTC_BETA      = 3950.0f;   // B25/85 -- calibrate, see README
static const float NTC_R_FIXED   = 10000.0f;  // 1% divider resistor to 3V3
static const float ADC_VREF_MV   = 3300.0f;   // divider top rail

// Readings outside this window mean an open or shorted probe. The bounds sit
// well inside the S3 ADC's trustworthy span: with 12 dB attenuation it stops
// tracking above roughly 3100 mV, so a threshold near the rail would miss an
// unplugged probe (node pulled to 3V3) and report it as "very cold" instead of
// FAULT. 200/2800 mV correspond to about +103 C and -9 C, neither of which a
// rack will ever legitimately reach.
static const float FAULT_MV_LOW  = 200.0f;
static const float FAULT_MV_HIGH = 2800.0f;

static const int   ADC_SAMPLES   = 32;
static const float TEMP_EMA      = 0.20f;     // 0..1, lower = smoother

// ------------------------------------------------------------------- fans --
// Intel 4-wire spec: 25 kHz. 10-bit gives 1024 steps, well inside what the
// LEDC 80 MHz clock can resolve at that frequency.
static const uint32_t PWM_FREQ_HZ  = 25000;
static const uint8_t  PWM_RES_BITS = 10;
static const uint16_t PWM_MAX      = (1 << PWM_RES_BITS) - 1;

// A fan is either stopped (0 %) or running at DUTY_MIN or more: between the
// two a Noctua can stall or chatter, so that band is never commanded.
static const uint8_t  DUTY_OFF     = 0;    // %, 0 % PWM stops a Noctua
static const uint8_t  DUTY_MIN     = 20;   // %, lowest running duty (NF-12 stalls below)
static const uint8_t  DUTY_MAX     = 100;  // %
static const uint8_t  DUTY_START   = 50;   // %, restart kick when leaving 0 %
static const uint8_t  DUTY_SLEW    = 1;    // %, max change per control tick
static const uint8_t  TACH_PPR     = 2;    // Noctua: 2 pulses per revolution

// The tach wire runs next to the 25 kHz PWM wire, and every PWM edge couples a
// sub-microsecond spike onto it. Counted as pulses, those inflate the RPM --
// worst at mid duty and while the duty is ramping, correct only at 100 % where
// the PWM stops switching. The PCNT glitch filter drops anything shorter than
// ~12.8 us (its maximum); a real tach half-period is milliseconds (10 ms at
// 3000 RPM), so no genuine pulse comes close to it.
static const uint16_t TACH_FILTER_APB = 1023;   // APB cycles @ 80 MHz = 12.8 us

// --------------------------------------------------------------- response --
// Below FAN_OFF_BELOW_C the fan is stopped. From there it follows a
// piecewise-linear temperature -> duty curve, ascending by temperature. With
// the hysteresis below, a running fan stops only when its probe is < 28.5 C.
// The probes sit at the back of the fan shrouds, in air the two ASUS GX10s
// have already heated (54 C seen with the fan at 100 %), so the curve is set
// for exhaust air: an idle GX10 stays under 30 C there, heavy load is 45-55 C.
static const float FAN_OFF_BELOW_C = 30.0f;
struct CurvePoint { float tempC; uint8_t duty; };
static const CurvePoint CURVE[] = {
    {30.0f,  20},
    {35.0f,  30},
    {40.0f,  45},
    {45.0f,  65},
    {50.0f,  85},
    {55.0f, 100},
};
static const size_t CURVE_LEN = sizeof(CURVE) / sizeof(CURVE[0]);

static const float HYSTERESIS_C = 1.5f;  // must cool this much before ramping down

// ------------------------------------------------------------- profiles ---
// Normal: the curve, capped at NORMAL_MAX_PCT.  Quiet: the curve scaled to QUIET_PCT (RPM on these
// Noctuas is close to proportional to duty, so 67 % caps them near 2000 RPM).
// Max: 100 %.  Quiet scales at every temperature, hot included -- it never
// gives way to Normal (a deliberate choice). The on/off switch stops
// the fans, except that a fan whose probe reaches OVERHEAT_C runs on its
// profile until it has cooled HYSTERESIS_C.
enum Profile : uint8_t { PROFILE_NORMAL, PROFILE_QUIET, PROFILE_MAX };
static const char *const PROFILE_NAME[] = {"normal", "quiet", "max"};
static const uint8_t QUIET_PCT  = 67;
static const uint8_t NORMAL_MAX_PCT = 80;   // only Max reaches 100 %
static const float   OVERHEAT_C = 55.0f;

static const uint32_t BTN_DEBOUNCE_MS = 30;
static const uint32_t BTN_LONG_MS     = 1000;  // held this long = long press
static const uint32_t BTN_GAP_MS      = 350;   // max gap inside a double press

// 0 = both fans follow max(sensor0, sensor1)
// 1 = fan N follows sensor N                  -- independent zones (chosen)
// A faulted probe stops the fan(s) that depend on it (by design: no
// full-speed failsafe). In mode 1 that is only fan N; in mode 0 both fans fall
// back to the healthy probe and stop only if both probes are faulted. With a
// single fan the two modes are the same thing.
#define ZONE_MODE 1

// A deliberate manual command outranks the sensor-fault stop -- otherwise
// the fans cannot be bench-tested before the probes are wired. It expires on
// its own so the rack can never be left parked at a fixed duty by accident.
static const uint32_t MANUAL_TIMEOUT_MS = 10UL * 60UL * 1000UL;

// Identify: one fan at 100 %, the other stopped, so you can see (and hear)
// which physical fan is which (with one fan: that fan at 100 %). Short, and it
// outranks everything else.
static const uint32_t IDENTIFY_MS = 15000;

static const uint32_t CONTROL_PERIOD_MS = 100;
static const uint32_t REPORT_PERIOD_MS  = 1000;

// ------------------------------------------------------------------ state --
static float   tempC[2]      = {NAN, NAN};
static bool    sensorFault[2] = {false, false};
static float   controlTemp[2] = {0.0f, 0.0f};  // hysteresis-held temperature
static uint8_t dutyPct[2]     = {DUTY_OFF, DUTY_OFF};
static uint16_t rpm[2]        = {0, 0};

static Profile  profile     = PROFILE_NORMAL;
static bool     overheat[2] = {false, false};
static bool     fansOn      = false;   // on/off switch, debounced

static bool     manualMode  = false;
static uint8_t  manualDuty  = 50;
static uint32_t manualSince = 0;

static int8_t   identifyFan   = -1;    // -1 = not identifying

// Per-fan off from the web page: a hard stop that outranks everything, the
// 55 C overheat run included (by design). Not saved; the physical switch
// clears it.
static bool     fanOff[2]     = {false, false};
static uint32_t identifySince = 0;

// ------------------------------------------------------------------ tach ---
// Hardware pulse counter, one unit per fan, counting falling edges.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static pcnt_unit_handle_t tachUnit[2];

static void tachInit(int idx) {
    pcnt_unit_config_t ucfg = {};
    ucfg.low_limit  = -1;
    ucfg.high_limit = 32767;
    pcnt_new_unit(&ucfg, &tachUnit[idx]);

    pcnt_glitch_filter_config_t fcfg = {};
    fcfg.max_glitch_ns = TACH_FILTER_APB * 1000UL / 80;   // same 12.8 us
    pcnt_unit_set_glitch_filter(tachUnit[idx], &fcfg);

    pcnt_chan_config_t ccfg = {};
    ccfg.edge_gpio_num  = PIN_TACH[idx];
    ccfg.level_gpio_num = -1;
    pcnt_channel_handle_t chan;
    pcnt_new_channel(tachUnit[idx], &ccfg, &chan);
    pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_HOLD,
                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE);

    pcnt_unit_enable(tachUnit[idx]);
    pcnt_unit_clear_count(tachUnit[idx]);
    pcnt_unit_start(tachUnit[idx]);
}

static uint32_t tachTake(int idx) {
    int n = 0;
    pcnt_unit_get_count(tachUnit[idx], &n);
    pcnt_unit_clear_count(tachUnit[idx]);
    return n > 0 ? (uint32_t)n : 0;
}
#else
static void tachInit(int idx) {
    pcnt_unit_t unit = (pcnt_unit_t)idx;
    pcnt_config_t cfg = {};
    cfg.pulse_gpio_num = PIN_TACH[idx];
    cfg.ctrl_gpio_num  = PCNT_PIN_NOT_USED;
    cfg.channel        = PCNT_CHANNEL_0;
    cfg.unit           = unit;
    cfg.pos_mode       = PCNT_COUNT_DIS;
    cfg.neg_mode       = PCNT_COUNT_INC;
    cfg.lctrl_mode     = PCNT_MODE_KEEP;
    cfg.hctrl_mode     = PCNT_MODE_KEEP;
    cfg.counter_h_lim  = 32767;
    cfg.counter_l_lim  = -1;
    pcnt_unit_config(&cfg);

    pcnt_set_filter_value(unit, TACH_FILTER_APB);
    pcnt_filter_enable(unit);

    pcnt_counter_pause(unit);
    pcnt_counter_clear(unit);
    pcnt_counter_resume(unit);
}

static uint32_t tachTake(int idx) {
    pcnt_unit_t unit = (pcnt_unit_t)idx;
    int16_t n = 0;
    pcnt_get_counter_value(unit, &n);
    pcnt_counter_clear(unit);
    return n > 0 ? (uint32_t)n : 0;
}
#endif

// ------------------------------------------------------------------- pwm ---
static void pwmInit(int idx) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttachChannel(PIN_PWM[idx], PWM_FREQ_HZ, PWM_RES_BITS, idx);
#else
    ledcSetup(idx, PWM_FREQ_HZ, PWM_RES_BITS);
    ledcAttachPin(PIN_PWM[idx], idx);
#endif
}

static void pwmWritePct(int idx, uint8_t pct) {
    uint32_t duty = (uint32_t)PWM_MAX * pct / 100;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteChannel(idx, duty);
#else
    ledcWrite(idx, duty);
#endif
}

// --------------------------------------------------------------- sensing ---
// Divider: 3V3 -- R_FIXED -- node(ADC) -- NTC -- GND.  Hotter NTC = lower node
// voltage. Either rail means the probe is open or shorted.
static bool readTemp(int idx, float &out) {
    uint32_t acc = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) acc += analogReadMilliVolts(PIN_NTC[idx]);
    float mv = (float)acc / ADC_SAMPLES;

    if (mv < FAULT_MV_LOW || mv > FAULT_MV_HIGH) return false;

    float rNtc = NTC_R_FIXED * mv / (ADC_VREF_MV - mv);
    float invT = 1.0f / 298.15f + logf(rNtc / NTC_R25) / NTC_BETA;
    out = 1.0f / invT - 273.15f;
    return true;
}

// ------------------------------------------------------------------ curve ---
static uint8_t curveDuty(float t) {
    if (t < FAN_OFF_BELOW_C) return DUTY_OFF;
    if (t <= CURVE[0].tempC) return CURVE[0].duty;
    for (size_t i = 1; i < CURVE_LEN; i++) {
        if (t <= CURVE[i].tempC) {
            const CurvePoint &a = CURVE[i - 1];
            const CurvePoint &b = CURVE[i];
            float span = b.tempC - a.tempC;
            float frac = span > 0.0f ? (t - a.tempC) / span : 0.0f;
            return (uint8_t)lroundf(a.duty + frac * (b.duty - a.duty));
        }
    }
    return CURVE[CURVE_LEN - 1].duty;
}

// Anything at or below 0 is off; anything else is lifted out of the stall
// band to at least DUTY_MIN.
static uint8_t clampDuty(int pct) {
    if (pct <= DUTY_OFF) return DUTY_OFF;
    if (pct < DUTY_MIN)  return DUTY_MIN;
    if (pct > DUTY_MAX)  return DUTY_MAX;
    return (uint8_t)pct;
}

// --------------------------------------------------------- profile + led ---
static void ledShow() {
#ifdef RGB_BUILTIN
    static const uint8_t RGB[][3] = {
        { 0, 12,  0},   // normal: green
        { 0,  0, 16},   // quiet:  blue
        {16,  0,  0},   // max:    red
    };
    static const uint8_t AMBER[3] = {12, 5, 0};   // switch off
    const uint8_t *c = fansOn ? RGB[profile] : AMBER;
#ifdef ARDUINO_LOLIN_S3_MINI
    // The Mini's LED takes red and green the other way round from what the
    // core sends (verified on the bench: "red" lit green). Swap them back.
    uint8_t r = c[1], g = c[0], b = c[2];
#else
    uint8_t r = c[0], g = c[1], b = c[2];
#endif
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    rgbLedWrite(RGB_BUILTIN, r, g, b);
#else
    neopixelWrite(RGB_BUILTIN, r, g, b);
#endif
#endif
}

// Choosing a profile is operator intent, so it also ends a console manual hold
// and an identify.
static void setProfile(Profile p) {
    profile = p;
    manualMode = false;
    identifyFan = -1;
    ledShow();
    Serial.printf("# profile %s\n", PROFILE_NAME[profile]);
}

// -------------------------------------------------------------- on/off sw ---
static bool readPowerSwitch() {
    bool level = digitalRead(PIN_POWER_SWITCH) == HIGH;
    return POWER_SWITCH_ACTIVE_HIGH ? level : !level;
}

// Flipping the switch is operator intent too, so it ends a console hold, an
// identify, and any per-fan off from the web page.
static void pollPowerSwitch(uint32_t now) {
    static bool     raw = false;
    static uint32_t rawSince = 0;
    bool r = readPowerSwitch();
    if (r != raw) { raw = r; rawSince = now; }
    if (raw != fansOn && now - rawSince >= BTN_DEBOUNCE_MS) {
        fansOn = raw;
        manualMode = false;
        identifyFan = -1;
        fanOff[0] = fanOff[1] = false;
        ledShow();
        Serial.println(fansOn ? "# switch on" : "# switch off");
    }
}

// ---------------------------------------------------------------- button ---
enum Gesture : uint8_t { GESTURE_NONE, GESTURE_SINGLE, GESTURE_DOUBLE, GESTURE_LONG };

// Non-blocking; call every loop. A long press fires while still held; a
// single press fires once BTN_GAP_MS passes without a second one.
static Gesture pollButton(uint32_t now) {
    static bool     raw = false, stable = false, longFired = false;
    static uint32_t rawSince = 0, downAt = 0, upAt = 0;
    static uint8_t  clicks = 0;

    bool level = digitalRead(PIN_BUTTON) == HIGH;
    bool r = BUTTON_ACTIVE_LOW ? !level : level;
    if (r != raw) { raw = r; rawSince = now; }

    if (raw != stable && now - rawSince >= BTN_DEBOUNCE_MS) {
        stable = raw;
        if (stable) {
            downAt = now;
            longFired = false;
        } else if (!longFired) {
            upAt = now;
            if (++clicks == 2) { clicks = 0; return GESTURE_DOUBLE; }
        }
    }
    if (stable && !longFired && now - downAt >= BTN_LONG_MS) {
        longFired = true;
        clicks = 0;
        return GESTURE_LONG;
    }
    if (!stable && clicks == 1 && now - upAt >= BTN_GAP_MS) {
        clicks = 0;
        return GESTURE_SINGLE;
    }
    return GESTURE_NONE;
}

// Works with the switch off too: the new profile applies once it is back on.
static void handleGesture(Gesture g) {
    switch (g) {
        case GESTURE_SINGLE:
            setProfile(profile == PROFILE_NORMAL ? PROFILE_QUIET : PROFILE_NORMAL);
            break;
        case GESTURE_DOUBLE:
            setProfile(profile == PROFILE_MAX ? PROFILE_NORMAL : PROFILE_MAX);
            break;
        case GESTURE_LONG:
            setProfile(PROFILE_NORMAL);
            break;
        default:
            break;
    }
}

// --------------------------------------------------------------- network ---
// Everything here is best-effort. The control loop never blocks on it: if the
// network is down the page is simply unreachable and the fans carry on.
static const char *const HOSTNAME = "dgx-fans";   // http://dgx-fans.local/
static const char *const WEB_USER = "fans";       // user name for changes
static const uint32_t    WIFI_RETRY_MS = 30000;

static Preferences prefs;
static String      wifiSsid, wifiPass, webPass;
static WebServer   web(80);
static bool        netUp = false, otaUp = false;

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DGX fans</title>
<style>
:root{--bg:#f4f4f6;--card:#fff;--fg:#1c1c1e;--mute:#6b6b70;--line:#dcdce0;--acc:#2f7d4f}
@media (prefers-color-scheme:dark){:root{--bg:#161618;--card:#222226;--fg:#ececf0;--mute:#9a9aa2;--line:#3a3a40;--acc:#5cc58a}}
*{box-sizing:border-box}body{margin:0;padding:16px;background:var(--bg);color:var(--fg);
font:16px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;max-width:640px;margin:auto}
h1{font-size:20px;margin:4px 0 12px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px}
.k{color:var(--mute);font-size:13px}.big{font-size:32px;font-weight:600;font-variant-numeric:tabular-nums}
.row{display:flex;gap:8px;margin-top:12px}button{flex:1;padding:12px;border-radius:10px;border:1px solid var(--line);
background:var(--card);color:var(--fg);font-size:16px;cursor:pointer}button.on{background:var(--acc);border-color:var(--acc);color:#fff}
button.off{background:#d9480f;border-color:#d9480f;color:#fff}
#msg{min-height:1.4em;color:var(--mute);font-size:14px;margin-top:8px}.bad{color:#d9480f}
</style></head><body>
<h1>DGX rack fans</h1>
<div class="grid" id="grid">
<div class="card"><div class="k">Probe 1 &rarr; fan 1</div><div class="big" id="t0">&ndash;</div><div id="f0" class="k"></div>
<div class="row"><button id="i0" onclick="ident(1)">Identify fan 1</button></div>
<div class="row"><button id="o0" onclick="fanOnOff(1)">Turn off fan 1</button></div></div>
<div class="card" id="c1"><div class="k">Probe 2 &rarr; fan 2</div><div class="big" id="t1">&ndash;</div><div id="f1" class="k"></div>
<div class="row"><button id="i1" onclick="ident(2)">Identify fan 2</button></div>
<div class="row"><button id="o1" onclick="fanOnOff(2)">Turn off fan 2</button></div></div>
</div>
<div class="card" style="margin-top:12px"><div class="k">Profile</div>
<div class="row"><button id="b-normal" onclick="setP('normal')">Normal</button>
<button id="b-quiet" onclick="setP('quiet')">Quiet</button><button id="b-max" onclick="setP('max')">Max</button></div>
<div id="msg"></div></div>
<p class="k" id="foot"></p>
<script>
const $=id=>document.getElementById(id);let idn=0,offs=[false,false];
function msg(t,bad){$('msg').textContent=t;$('msg').className=bad?'bad':''}
async function poll(){
 try{const s=await (await fetch('/api/status',{cache:'no-store'})).json();
  const n=s.fans||2,ix=n>1?[0,1]:[0];
  if(n<2){$('c1').style.display='none';$('grid').style.gridTemplateColumns='1fr'}
  for(const i of ix){
   $('t'+i).textContent=s.temp[i]===null?'FAULT':s.temp[i].toFixed(1)+' °C';
   $('t'+i).className='big'+(s.temp[i]===null?' bad':'');
   $('f'+i).textContent=(s.duty[i]?s.duty[i]+' %  ·  '+s.rpm[i]+' RPM':'stopped')+(s.overheat[i]?'  ·  over 55 °C':'')+(s.off[i]?'  ·  turned off':'');}
  for(const p of ['normal','quiet','max'])$('b-'+p).className=s.profile===p?'on':'';
  idn=s.identify;
  for(const i of ix){const on=idn===i+1;$('i'+i).className=on?'on':'';
   $('i'+i).textContent=on?'Stop · '+s.identifyLeft+' s':'Identify fan '+(i+1);
   $('o'+i).className=s.off[i]?'off':'';
   $('o'+i).textContent=(s.off[i]?'Turn on fan ':'Turn off fan ')+(i+1);}
  offs=s.off;
  let st=s.switchOn?'':'Switch is OFF — fans stay stopped (except above 55 °C). ';
  if(s.manual)st+='Console hold active. ';
  const o=ix.map(i=>i+1).filter(f=>s.off[f-1]);
  if(o.length)st+=(o.length===2?'Both fans':'Fan '+o[0])+' turned off here — stays stopped even if hot. ';
  if(s.identify)st='Identifying fan '+s.identify+': it runs at 100 %'+(n>1?', the other is stopped':'')+'. Back to normal in '+s.identifyLeft+' s. ';
  if(!$('msg').dataset.keep)msg(st,!s.switchOn||o.length>0);
  $('foot').textContent='Firmware '+(s.version||'?')+' · Wi-Fi '+s.rssi+' dBm · up '+Math.floor(s.uptime/3600)+' h '+Math.floor(s.uptime%3600/60)+' min'
   +(s.locked?' · changes disabled until a web password is set over USB':'');
 }catch(e){msg('Controller not reachable',true)}}
async function setP(p){
 const r=await fetch('/api/profile',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'name='+p});
 if(r.status===403)msg('Set a web password over USB first (webpass <password>)',true);
 else if(r.status===401)msg('Wrong or missing password',true);
 else if(!r.ok)msg('Error '+r.status,true);
 poll()}
async function ident(n){
 const r=await fetch('/api/identify',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'fan='+(idn===n?0:n)});
 if(r.status===403)msg('Set a web password over USB first (webpass <password>)',true);
 else if(r.status===401)msg('Wrong or missing password',true);
 else if(!r.ok)msg('Error '+r.status,true);
 poll()}
async function fanOnOff(n){
 const r=await fetch('/api/fan',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'fan='+n+'&on='+(offs[n-1]?1:0)});
 if(r.status===403)msg('Set a web password over USB first (webpass <password>)',true);
 else if(r.status===401)msg('Wrong or missing password',true);
 else if(!r.ok)msg('Error '+r.status,true);
 poll()}
poll();setInterval(poll,1000);
</script></body></html>)HTML";

static void wifiStart() {
    if (!wifiSsid.length()) {
        Serial.println("# wifi: not set up -- 'wifi ssid <name>' then 'wifi pass <password>'");
        return;
    }
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    Serial.printf("# wifi: connecting to \"%s\"\n", wifiSsid.c_str());
}

static void wifiReport() {
    if (!wifiSsid.length()) { wifiStart(); return; }
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("# wifi: connected to \"%s\", %s, %d dBm -- http://%s.local/\n",
                      wifiSsid.c_str(), WiFi.localIP().toString().c_str(),
                      (int)WiFi.RSSI(), HOSTNAME);
    else
        Serial.printf("# wifi: not connected to \"%s\" (status %d), retrying\n",
                      wifiSsid.c_str(), (int)WiFi.status());
    Serial.printf("# web password: %s, ota: %s\n",
                  webPass.length() ? "set" : "NOT set (changes and ota disabled)",
                  otaUp ? "on" : "off");
}

// Viewing is open to the local network; changes need the web password, and
// are refused outright until one has been set over USB.
static bool webAuthorised() {
    if (!webPass.length()) {
        web.send(403, "application/json", "{\"error\":\"no web password set\"}");
        return false;
    }
    if (!web.authenticate(WEB_USER, webPass.c_str())) {
        web.requestAuthentication(BASIC_AUTH, HOSTNAME);
        return false;
    }
    return true;
}

// Whole seconds left, rounded up; 0 when not identifying (or just expiring).
static unsigned long identifyLeftS() {
    if (identifyFan < 0) return 0;
    uint32_t gone = millis() - identifySince;
    return gone >= IDENTIFY_MS ? 0 : (IDENTIFY_MS - gone + 999) / 1000;
}

// Per-fan arrays hold NFANS entries; "fans" says how many.
static void handleStatus() {
    String temp, duty, rpms, hot, off;
    for (int i = 0; i < NFANS; i++) {
        const char *sep = i ? "," : "";
        temp += sep;
        temp += (sensorFault[i] || isnan(tempC[i])) ? String("null") : String(tempC[i], 1);
        duty += sep; duty += dutyPct[i];
        rpms += sep; rpms += rpm[i];
        hot  += sep; hot  += overheat[i] ? "true" : "false";
        off  += sep; off  += fanOff[i] ? "true" : "false";
    }
    char json[480];
    snprintf(json, sizeof json,
             "{\"version\":\"%s\",\"fans\":%d,\"temp\":[%s],\"duty\":[%s],\"rpm\":[%s],\"profile\":\"%s\","
             "\"switchOn\":%s,\"manual\":%s,\"overheat\":[%s],\"rssi\":%d,"
             "\"uptime\":%lu,\"locked\":%s,\"identify\":%d,\"identifyLeft\":%lu,\"off\":[%s]}",
             FW_VERSION, NFANS, temp.c_str(), duty.c_str(), rpms.c_str(), PROFILE_NAME[profile],
             fansOn ? "true" : "false", manualMode ? "true" : "false", hot.c_str(),
             (int)WiFi.RSSI(), (unsigned long)(millis() / 1000),
             webPass.length() ? "false" : "true", identifyFan + 1,
             identifyLeftS(), off.c_str());
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "application/json", json);
}

static void handleProfilePost() {
    if (!webAuthorised()) return;
    String name = web.arg("name");
    for (uint8_t p = 0; p <= PROFILE_MAX; p++) {
        if (name == PROFILE_NAME[p]) {
            setProfile((Profile)p);
            web.send(200, "application/json", "{\"ok\":true}");
            return;
        }
    }
    web.send(400, "application/json", "{\"error\":\"unknown profile\"}");
}

static void stopIdentify() {
    if (identifyFan < 0) return;
    identifyFan = -1;
    Serial.println("# identify done, back to normal control");
}

static void startIdentify(int fan) {
    identifyFan = fan;
    identifySince = millis();
    Serial.printf("# identify fan %d: 100%%%s, for %lu s\n", fan + 1,
                  NFANS > 1 ? ", other fan stopped" : "", IDENTIFY_MS / 1000UL);
}

static void handleIdentifyPost() {
    if (!webAuthorised()) return;
    int fan = web.arg("fan").toInt();      // 0 = stop
    if (fan < 0 || fan > NFANS) {
        web.send(400, "application/json", NFANS > 1 ? "{\"error\":\"fan must be 0, 1 or 2\"}"
                                                    : "{\"error\":\"fan must be 0 or 1\"}");
        return;
    }
    if (fan) startIdentify(fan - 1);
    else     stopIdentify();
    web.send(200, "application/json", "{\"ok\":true}");
}

static void setFanOff(int i, bool off) {
    fanOff[i] = off;
    Serial.printf("# fan %d turned %s\n", i + 1, off ? "off (stays off even when hot)" : "on");
}

static void handleFanPost() {
    if (!webAuthorised()) return;
    int fan = web.arg("fan").toInt();
    if (fan < 1 || fan > NFANS || !web.hasArg("on")) {
        web.send(400, "application/json", NFANS > 1 ? "{\"error\":\"need fan=1|2 and on=0|1\"}"
                                                    : "{\"error\":\"need fan=1 and on=0|1\"}");
        return;
    }
    setFanOff(fan - 1, web.arg("on").toInt() == 0);
    web.send(200, "application/json", "{\"ok\":true}");
}

static void otaStart() {
    if (otaUp || !netUp || !webPass.length()) return;
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(webPass.c_str());
    ArduinoOTA.setMdnsEnabled(false);             // mDNS is already ours
    ArduinoOTA.onStart([]() { Serial.println("# ota: update starting"); });
    ArduinoOTA.onEnd([]()   { Serial.println("# ota: done, restarting"); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("# ota: error %u\n", (unsigned)e); });
    ArduinoOTA.begin();
    otaUp = true;
}

static void netStartServices() {
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    web.on("/", HTTP_GET, []() { web.send_P(200, "text/html", PAGE); });
    web.on("/api/status", HTTP_GET, handleStatus);
    web.on("/api/profile", HTTP_POST, handleProfilePost);
    web.on("/api/identify", HTTP_POST, handleIdentifyPost);
    web.on("/api/fan", HTTP_POST, handleFanPost);
    web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
    web.begin();
    netUp = true;
    otaStart();
}

static void pollNetwork(uint32_t now) {
    static bool     was = false;
    static uint32_t lastTry = 0;
    bool is = WiFi.status() == WL_CONNECTED;
    if (is && !was) {
        if (!netUp) netStartServices();
        Serial.printf("# wifi: connected, http://%s.local/ (%s)\n",
                      HOSTNAME, WiFi.localIP().toString().c_str());
    } else if (!is && was) {
        Serial.println("# wifi: connection lost, retrying");
    }
    if (!is && wifiSsid.length() && now - lastTry >= WIFI_RETRY_MS) {
        lastTry = now;
        WiFi.reconnect();
    }
    was = is;
    if (netUp) {
        web.handleClient();
        if (otaUp) ArduinoOTA.handle();
    }
}

// ---------------------------------------------------------------- serial ---
static void handleCommand(String raw) {
    raw.trim();
    String cmd = raw;                    // lower-cased for matching; names and
    cmd.toLowerCase();                   // passwords are taken from raw
    if (cmd == "wifi") {
        wifiReport();
    } else if (cmd.startsWith("wifi ssid ")) {
        wifiSsid = raw.substring(10);
        prefs.putString("ssid", wifiSsid);
        Serial.printf("# wifi: network name saved: \"%s\"\n", wifiSsid.c_str());
        wifiStart();
    } else if (cmd.startsWith("wifi pass ")) {
        wifiPass = raw.substring(10);
        prefs.putString("pass", wifiPass);
        Serial.println("# wifi: password saved (not shown)");
        wifiStart();
    } else if (cmd == "wifi forget") {
        wifiSsid = wifiPass = "";
        prefs.remove("ssid");
        prefs.remove("pass");
        WiFi.disconnect(true);
        Serial.println("# wifi: forgotten, off");
    } else if (cmd.startsWith("webpass ")) {
        webPass = raw.substring(8);
        prefs.putString("webpass", webPass);
        if (otaUp) ArduinoOTA.setPassword(webPass.c_str());
        otaStart();
        Serial.printf("# web password saved (not shown); web user name is \"%s\"\n", WEB_USER);
    } else if (cmd == "auto") {
        manualMode = false;
        stopIdentify();
        Serial.println("# auto mode");
    } else if (cmd == "max") {
        manualMode = true;
        manualSince = millis();
        manualDuty = DUTY_MAX;
        Serial.println("# manual 100%");
    } else if (cmd.startsWith("set ")) {
        manualMode = true;
        manualSince = millis();
        manualDuty = clampDuty(cmd.substring(4).toInt());
        Serial.printf("# manual %u%% (reverts to auto in %lu min)\n",
                      manualDuty, MANUAL_TIMEOUT_MS / 60000UL);
    } else if (cmd.length() == 10 && cmd.startsWith("identify ") &&
               cmd.charAt(9) >= '1' && cmd.charAt(9) < '1' + NFANS) {
        startIdentify(cmd.charAt(9) - '1');
    } else if ((cmd.length() == 8 || cmd.length() == 9) && cmd.startsWith("fan ") &&
               cmd.charAt(4) >= '1' && cmd.charAt(4) < '1' + NFANS &&
               (cmd.endsWith(" on") || cmd.endsWith(" off")) && cmd.charAt(5) == ' ') {
        setFanOff(cmd.charAt(4) - '1', cmd.endsWith("off"));
    } else if (cmd == "identify stop") {
        stopIdentify();
    } else if (cmd.startsWith("profile ")) {
        String name = cmd.substring(8);
        name.trim();
        for (uint8_t p = 0; p <= PROFILE_MAX; p++)
            if (name == PROFILE_NAME[p]) { setProfile((Profile)p); return; }
        Serial.println("# profiles: normal | quiet | max");
    } else if (cmd == "curve") {
        Serial.printf("# <%4.1fC -> off\n", FAN_OFF_BELOW_C);
        for (size_t i = 0; i < CURVE_LEN; i++)
            Serial.printf("# %5.1fC -> %3u%%\n", CURVE[i].tempC, CURVE[i].duty);
    } else if (cmd == "version") {
        Serial.printf("# firmware %s, %d fan%s\n", FW_VERSION, NFANS, NFANS > 1 ? "s" : "");
    } else if (cmd == "help") {
        Serial.println(NFANS > 1
            ? "# auto | max | set <pct> | identify <1|2|stop> | fan <1|2> <on|off> | profile <normal|quiet|max> | curve | version | help"
            : "# auto | max | set <pct> | identify <1|stop> | fan 1 <on|off> | profile <normal|quiet|max> | curve | version | help");
        Serial.println("# wifi | wifi ssid <name> | wifi pass <password> | wifi forget | webpass <password>");
    } else if (cmd.length()) {
        Serial.printf("# unknown command \"%s\", try: help\n", raw.c_str());
    }
}

// Typing in a terminal sends edits as characters: Backspace/Delete remove the
// last one, other control characters are dropped. While a line is being typed
// the 1 Hz status report is held back so it does not scroll through it.
static String serialLine;

static void pollSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialLine.length()) handleCommand(serialLine);
            serialLine = "";
        } else if (c == '\b' || c == 0x7f) {
            if (serialLine.length()) serialLine.remove(serialLine.length() - 1);
        } else if ((uint8_t)c >= 0x20 && serialLine.length() < 100) {
            serialLine += c;
        }
    }
}

// ------------------------------------------------------------------ setup ---
void setup() {
    Serial.begin(115200);
    pinMode(PIN_BUTTON, BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
    pinMode(PIN_POWER_SWITCH, POWER_SWITCH_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);
    delay(5);
    fansOn = readPowerSwitch();          // the switch is physical memory: honour it
    ledShow();

    analogReadResolution(12);
    for (int i = 0; i < NFANS; i++) {
        analogSetPinAttenuation(PIN_NTC[i], ADC_11db);  // ~0..3.1 V usable
        pinMode(PIN_TACH[i], INPUT_PULLUP);             // external 10k also fitted
        tachInit(i);
        pwmInit(i);
        // Boot stopped, no full-speed burst: the first control tick starts any
        // fan whose probe is at or above FAN_OFF_BELOW_C, with the DUTY_START
        // kick. Booting as "running" would let the stop hysteresis keep a fan
        // on below the switch-on point.
        pwmWritePct(i, DUTY_OFF);
    }

    prefs.begin("dgx", false);
    wifiSsid = prefs.getString("ssid", "");
    wifiPass = prefs.getString("pass", "");
    webPass  = prefs.getString("webpass", "");
    if (wifiSsid.length()) wifiStart();

    Serial.printf("# dgx-rack fan controller %s, %d fan%s -- type 'help'\n",
                  FW_VERSION, NFANS, NFANS > 1 ? "s" : "");
}

// ------------------------------------------------------------------- loop ---
void loop() {
    static uint32_t lastControl = 0, lastReport = 0;
    uint32_t now = millis();

    pollSerial();
    pollPowerSwitch(now);
    handleGesture(pollButton(now));
    pollNetwork(now);

    if (now - lastControl >= CONTROL_PERIOD_MS) {
        lastControl = now;

        for (int i = 0; i < NFANS; i++) {
            float t;
            if (readTemp(i, t)) {
                sensorFault[i] = false;
                tempC[i] = isnan(tempC[i]) ? t : tempC[i] + TEMP_EMA * (t - tempC[i]);
            } else {
                sensorFault[i] = true;
                tempC[i] = NAN;
            }
        }

        if (manualMode && now - manualSince >= MANUAL_TIMEOUT_MS) {
            manualMode = false;
            Serial.println("# manual hold expired, back to auto");
        }
        if (identifyFan >= 0 && now - identifySince >= IDENTIFY_MS) stopIdentify();

        for (int i = 0; i < NFANS; i++) {
            uint8_t target;

#if ZONE_MODE == 0 && FAN_COUNT == 2
            bool  blind = sensorFault[0] && sensorFault[1];
            float t = sensorFault[0] ? tempC[1]
                    : sensorFault[1] ? tempC[0]
                    : max(tempC[0], tempC[1]);
#else
            bool  blind = sensorFault[i];
            float t = tempC[i];
#endif

            if (manualMode) {
                target = manualDuty;             // operator is present, they win
            } else if (profile == PROFILE_MAX && fansOn) {
                target = DUTY_MAX;               // chosen on the button, runs even blind
            } else if (blind) {
                target = DUTY_OFF;               // no reading, no fan
                overheat[i] = false;
            } else {
                // Only let the held temperature fall once it has dropped a full
                // hysteresis band, so the fans do not hunt around a curve knee.
                if (t > controlTemp[i] || t < controlTemp[i] - HYSTERESIS_C)
                    controlTemp[i] = t;
                target = curveDuty(controlTemp[i]);

                // The held temperature above steps down from wherever the probe
                // peaked, so on its own it can stop a fan at 25.0 C. Stopping is
                // judged against the threshold instead: a running fan stays at
                // DUTY_MIN until the probe is a full band below FAN_OFF_BELOW_C.
                if (target == DUTY_OFF && dutyPct[i] > DUTY_OFF &&
                    t >= FAN_OFF_BELOW_C - HYSTERESIS_C)
                    target = DUTY_MIN;

                if (t >= OVERHEAT_C)                     overheat[i] = true;
                else if (t < OVERHEAT_C - HYSTERESIS_C)  overheat[i] = false;

                // Switch-off gives way while overheated; Quiet never does.
                if (!fansOn && !overheat[i])
                    target = DUTY_OFF;
                else if (profile == PROFILE_QUIET && target > DUTY_OFF)
                    target = max(DUTY_MIN, (uint8_t)((target * QUIET_PCT + 50) / 100));
                else if (profile == PROFILE_NORMAL)
                    target = min(target, NORMAL_MAX_PCT);
            }

            target = clampDuty(target);

            // A fan turned off on the web page stops at once, whatever else
            // is going on.
            if (fanOff[i]) {
                dutyPct[i] = DUTY_OFF;
                pwmWritePct(i, dutyPct[i]);
                continue;
            }

            // Identify skips the slew so the difference is obvious at once;
            // afterwards the fans slew back to their normal targets.
            if (identifyFan >= 0) {
                dutyPct[i] = (i == identifyFan) ? DUTY_MAX : DUTY_OFF;
                pwmWritePct(i, dutyPct[i]);
                continue;
            }

            // Slew-limit so a step in temperature does not slam the fans. The
            // on/off transitions jump instead of crawling through the stall
            // band: a stopped fan gets a kick (then slews down to its target),
            // and a fan slewing down to DUTY_MIN with "off" as target stops.
            if (dutyPct[i] == DUTY_OFF && target > DUTY_OFF)
                dutyPct[i] = max(target, DUTY_START);
            else if (target == DUTY_OFF && dutyPct[i] <= DUTY_MIN)
                dutyPct[i] = DUTY_OFF;
            else if (target > dutyPct[i])
                dutyPct[i] = clampDuty(min((int)target, dutyPct[i] + DUTY_SLEW));
            else if (target < dutyPct[i])
                dutyPct[i] = clampDuty(max((int)target, dutyPct[i] - DUTY_SLEW));

            pwmWritePct(i, dutyPct[i]);
        }
    }

    if (now - lastReport >= REPORT_PERIOD_MS) {
        uint32_t elapsed = now - lastReport;
        lastReport = now;

        for (int i = 0; i < NFANS; i++) {
            uint32_t pulses = tachTake(i);
            rpm[i] = (uint16_t)((uint64_t)pulses * 60000ULL / (TACH_PPR * elapsed));
        }

        if (serialLine.length()) return;   // someone is typing a command
        String t0 = sensorFault[0] ? String("FAULT") : String(tempC[0], 1);
        String t1 = sensorFault[1] ? String("FAULT") : String(tempC[1], 1);
        if (NFANS == 1)
            Serial.printf("t0=%s duty=%u%% rpm=%u", t0.c_str(), dutyPct[0], rpm[0]);
        else
            Serial.printf("t0=%s t1=%s duty=%u%%/%u%% rpm=%u/%u", t0.c_str(), t1.c_str(),
                          dutyPct[0], dutyPct[1], rpm[0], rpm[1]);
        Serial.printf("%s%s%s%s%s%s%s\n",
                      identifyFan >= 0 ? " [identify]" : manualMode ? " [manual]" : "",
                      profile != PROFILE_NORMAL ? " [" : "",
                      profile != PROFILE_NORMAL ? PROFILE_NAME[profile] : "",
                      profile != PROFILE_NORMAL ? "]" : "",
                      fansOn ? "" : " [switch off]",
                      fanOff[0] ? " [fan1 off]" : "", fanOff[1] ? " [fan2 off]" : "");
    }
}
