#include <Arduino.h>
#include <initializer_list>
#include <esp_arduino_version.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "Select a classic ESP32-WROOM-32 target"
#endif
#if ESP_ARDUINO_VERSION_MAJOR < 2
#error "Requires Arduino-ESP32 core 2.x or 3.x"
#endif

// CASE 1 only: continuous recirculation, NO overall run timer.
// P2 LPWM drive; choose wiring below with power off.
constexpr bool P2_RPWM_HARD_GROUNDED=false;
// false: GPIO19 -> driver RPWM, no external ground on GPIO19.
// true: disconnect GPIO19 lead; connect driver RPWM directly to driver GND.
constexpr int P1_R=33,P1_L=32,P1_RE=26,P1_LE=13;
constexpr int P2_R=19,P2_L=18,P2_RE=17,P2_LE=16;
constexpr int FLOAT_PIN=35,FLOW_PIN=14,SOV=27;
constexpr int FLOAT_HIGH=LOW,VALVE_OPEN=HIGH;
constexpr uint32_t VALVE_DELAY_MS=300, CLEAR_MS=1000;
constexpr uint32_t FIRST_FLOW_TIMEOUT_MS=20000, FLOW_LOSS_MS=3000;
float K_PULSES_PER_LITRE=0; // Enter measured K with 'k <pulses/litre>' while IDLE.
bool plotMode=false;
char commandBuffer[64];
size_t commandLength=0;
bool discardCommand=false;

enum RunState { IDLE, OPENING, CIRCULATING, FAULT };
RunState state=IDLE;
bool armed=false,pwmOK=false,blocked=false,clearing=false,flowSeen=false;
int p1Duty=0; bool p2On=false,sovOn=false;
uint32_t startAt=0,clearAt=0,lastPulseAt=0,observed=0;
uint32_t runStartPulses=0,runPulses=0,reportAt=0,reportPulses=0;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t totalPulses=0;

void ARDUINO_ISR_ATTR onFlow() {
  portENTER_CRITICAL_ISR(&mux); ++totalPulses; portEXIT_CRITICAL_ISR(&mux);
}
uint32_t pulses() {
  portENTER_CRITICAL(&mux); uint32_t n=totalPulses; portEXIT_CRITICAL(&mux); return n;
}
void lowOutput(int p) { pinMode(p,OUTPUT); digitalWrite(p,LOW); }
bool pwmWrite(uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  return ledcWrite(P1_R,duty);
#else
  ledcWrite(0,duty); return true;
#endif
}
bool initPWM() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  return ledcAttach(P1_R,20000,10) && pwmWrite(0);
#else
  if(ledcSetup(0,20000,10)<=0) return false;
  ledcAttachPin(P1_R,0); return pwmWrite(0);
#endif
}
bool pump1(bool on) {
  if(!on) { digitalWrite(P1_RE,LOW); digitalWrite(P1_LE,LOW); }
  digitalWrite(P1_L,LOW);
  bool ok=pwmOK && pwmWrite(on?512:0);
  digitalWrite(P1_RE,on && ok); digitalWrite(P1_LE,on && ok);
  p1Duty=on && ok?50:0; return !on || ok;
}
void pump2(bool on) {
  if(!on) { digitalWrite(P2_RE,LOW); digitalWrite(P2_LE,LOW); }
  if(!P2_RPWM_HARD_GROUNDED) digitalWrite(P2_R,LOW);
  digitalWrite(P2_L,on?HIGH:LOW);
  if(on) { digitalWrite(P2_RE,HIGH); digitalWrite(P2_LE,HIGH); }
  p2On=on;
}
void valve(bool on) { digitalWrite(SOV,on?VALVE_OPEN:!VALVE_OPEN); sovOn=on; }
void stopRun(bool fault,const char* why) {
  if(state==OPENING || state==CIRCULATING) runPulses=pulses()-runStartPulses;
  pump1(false); pump2(false); valve(false); armed=false;
  state=fault?FAULT:IDLE;
  Serial.printf("# %s: %s\n",fault?"FAULT":"STOP",why);
}
void startRun() {
  if(state!=IDLE || !armed || !pwmOK) { Serial.println("# Not ready: r resets fault; a arms; s starts."); return; }
  armed=false; blocked=digitalRead(FLOAT_PIN)==FLOAT_HIGH; clearing=false;
  flowSeen=false; runPulses=0;
  runStartPulses=observed=pulses(); startAt=lastPulseAt=millis();
  reportAt=startAt; reportPulses=runStartPulses;
  valve(true); state=OPENING;
  Serial.println("# CASE1 starting. No overall duration limit. x stops.");
  if(K_PULSES_PER_LITRE<=0) Serial.println("# UNCALIBRATED: pulses only; flow/volume fields are nan.");
}
void csvHeader() {
  Serial.println("time_s,state,armed,float_raw,high,P1_pct,P2_pct,SOV,pulses,Hz,run_pulses,flow_mLs,flow_Lmin,returned_L");
}
void handleCommand(char* c) {
  if(strcmp(c,"a")==0 && state==IDLE && pwmOK) { armed=true; Serial.println("# ARMED"); }
  else if(strcmp(c,"s")==0) startRun();
  else if(strcmp(c,"r")==0 && state==FAULT && pwmOK) stopRun(false,"fault acknowledged; fix cause before restarting");
  else if(strcmp(c,"p")==0) {
    if(!plotMode && K_PULSES_PER_LITRE<=0) { Serial.println("# Enter measured k before plot mode."); return; }
    plotMode=!plotMode;
    if(!plotMode) csvHeader();
    else Serial.println("# Plot mode: 1 sample/second, mL/s and L/min.");
  }
  else if(c[0]=='k' && c[1]==' ' && state==IDLE) {
    char* end; float k=strtof(c+2,&end);
    while(*end==' ') ++end;
    if(end==c+2 || *end!='\0' || !isfinite(k) || k<=0) {
      Serial.println("# Invalid K. Example syntax: k 6000 (use YOUR measured value)."); return;
    }
    K_PULSES_PER_LITRE=k;
    runPulses=0; // Previous trial may have used another calibration.
    Serial.printf("# K=%.4f pulses/L. RAM only: re-enter after reset.\n",k);
  }
  else if(strcmp(c,"h")==0) {
    Serial.println("# Newline commands: a arm; s start; x immediate stop; r reset fault; k <measured K>; p plot/CSV; h help");
  }
  else if(c[0]) Serial.println("# Command rejected: use h; calibration changes require IDLE.");
}
void readCommands() {
  int queued=Serial.available();
  for(int i=0;i<queued;i++) {
    char c=(char)Serial.read();
    // Stop remains immediate, even without a newline or during an overlong line.
    if(c=='x' || c=='X') {
      stopRun(state==FAULT,"operator stop"); commandLength=0; discardCommand=false;
    } else if(c=='\n' || c=='\r') {
      if(!discardCommand && commandLength) {
        commandBuffer[commandLength]='\0'; handleCommand(commandBuffer);
      }
      commandLength=0; discardCommand=false;
    } else if(!discardCommand) {
      if(commandLength<sizeof(commandBuffer)-1) commandBuffer[commandLength++]=c;
      else { commandLength=0; discardCommand=true; Serial.println("# Command too long; line discarded."); }
    }
  }
}
void setup() {
  for(int p:{P1_RE,P1_LE,P2_RE,P2_LE,22,21}) lowOutput(p);
  for(int p:{P1_R,P1_L,P2_L,23}) lowOutput(p);
  if(P2_RPWM_HARD_GROUNDED) pinMode(P2_R,INPUT); else lowOutput(P2_R);
  pinMode(25,INPUT); // P3 disabled; ambiguous PWM net undriven.
  pinMode(SOV,OUTPUT); valve(false);
  pinMode(FLOAT_PIN,INPUT); // External 3.3 V pull-up required.
  pinMode(FLOW_PIN,INPUT); // 3.3 V-compatible signal required.
  Serial.begin(115200); pwmOK=initPWM();
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN),onFlow,RISING);
  stopRun(!pwmOK,pwmOK?"ready":"P1 PWM initialization failed");
  Serial.println("# FINAL CASE1: 115200 baud, select NEWLINE. a=arm; s=start; x=stop; r=reset fault");
  Serial.println("# k <measured pulses/L>=calibration; p=plot/CSV; h=help. No physical start switch.");
  Serial.println("# Continuous circulation; no pulses means FAULT, NOT proof of empty tank.");
  csvHeader();
}
void loop() {
  readCommands();
  uint32_t now=millis(),n=pulses();
  bool high=digitalRead(FLOAT_PIN)==FLOAT_HIGH;
  if(state==OPENING || state==CIRCULATING) {
    if(high) { blocked=true; clearing=false; }
    else if(blocked) {
      if(!clearing) { clearing=true; clearAt=now; }
      if(now-clearAt>=CLEAR_MS) { blocked=false; clearing=false; }
    }
  }
  if(state==OPENING && now-startAt>=VALVE_DELAY_MS) {
    // Ignore pulses before actual pump start for startup flow qualification.
    observed=pulses(); lastPulseAt=now; startAt=now;
    state=CIRCULATING; pump2(true);
  }
  if(state==CIRCULATING) {
    n=pulses();
    if(n!=observed) { observed=n; lastPulseAt=now; flowSeen=true; }
    if(!pump1(!blocked)) stopRun(true,"P1 PWM write failed");
    else if(!flowSeen && now-startAt>=FIRST_FLOW_TIMEOUT_MS)
      stopRun(true,"no flow established; check water, priming, pump and sensor");
    else if(flowSeen && now-lastPulseAt>=FLOW_LOSS_MS)
      stopRun(true,"return flow lost; empty tank OR other fault; inspect before reset");
  }
  if(now-reportAt>=1000) {
    n=pulses(); float hz=(n-reportPulses)*1000.0f/(now-reportAt);
    uint32_t run=(state==OPENING || state==CIRCULATING)?n-runStartPulses:runPulses;
    const char* label=state==IDLE?"IDLE":state==OPENING?"OPENING":state==FAULT?"FAULT":"CASE1";
    float mLs=NAN,Lmin=NAN,litres=NAN;
    if(K_PULSES_PER_LITRE>0) {
      mLs=hz*1000/K_PULSES_PER_LITRE;
      Lmin=hz*60/K_PULSES_PER_LITRE;
      litres=run/K_PULSES_PER_LITRE;
    }
    if(plotMode) Serial.printf("flow_mLs:%.3f\tflow_Lmin:%.3f\n",mLs,Lmin);
    else Serial.printf("%.3f,%s,%d,%d,%d,%d,%d,%d,%lu,%.3f,%lu,%.3f,%.3f,%.4f\n",
      now/1000.0,label,armed,digitalRead(FLOAT_PIN),high,p1Duty,p2On?100:0,sovOn,
      (unsigned long)n,hz,(unsigned long)run,mLs,Lmin,litres);
    reportAt=now; reportPulses=n;
  }
}
