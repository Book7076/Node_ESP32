#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <HX711.h>
#include <Preferences.h>

Preferences prefs;
int NODE_ID = 1;

uint8_t gatewayAddress[] = {0x14, 0x2B, 0x2F, 0xD7, 0x63, 0x9C};
const int WIFI_CHANNEL = 6;

enum ChemicalType {
  CHEMICAL_ALL = 0,
  CHEMICAL_MOLASSES = 1,
  CHEMICAL_LIME = 2
};

// Existing molasses set
const int MOLASSES_RELAY_PIN = 26;   // Active LOW
const int MOLASSES_DOUT_PIN = 32;
const int MOLASSES_SCK_PIN = 33;

// New lime set
const int LIME_RELAY_PIN = 27;       // Active LOW
const int LIME_DOUT_PIN = 25;
const int LIME_SCK_PIN = 14;

HX711 molassesScale;
HX711 limeScale;

// Calibrate each load cell separately before actual use.
float MOLASSES_CALIBRATION_FACTOR = 1.0;
float LIME_CALIBRATION_FACTOR = 1.0;

const float MAX_LOADCELL_WEIGHT_G = 1000.0;
const float STOP_OFFSET_G = 3.0;

const unsigned long WEIGHT_READ_INTERVAL = 200;
const unsigned long STATUS_SEND_INTERVAL = 500;
const unsigned long IDLE_STATUS_INTERVAL = 500;
const unsigned long SETTLE_TIME = 2000;
const unsigned long HEARTBEAT_INTERVAL = 10000;
const unsigned long MANUAL_MAX_MS = 5UL * 60UL * 1000UL;
const unsigned long DISPENSE_MAX_MS = 3UL * 60UL * 1000UL;
const unsigned long NO_PROGRESS_TIMEOUT_MS = 15000;
const float PROGRESS_MIN_G = 0.5;
const unsigned long STAGGER_STEP_MS = 60;

// These structures must exactly match gateway_multi.ino.
typedef struct struct_message {
  int node_id;
  int command;
  int chemical;
  float amount;
} struct_message;

typedef struct struct_node_status {
  int node_id;
  int chemical;
  float weight;
  bool relay_status;
  int status;
  float start_weight;
  float target_weight;
  float dispensed;
} struct_node_status;

typedef struct struct_heartbeat {
  int node_id;
  bool is_online;
} struct_heartbeat;

const int STATUS_IDLE = 0;
const int STATUS_DISPENSING = 1;
const int STATUS_COMPLETE = 2;
const int STATUS_NOT_ENOUGH = 3;
const int STATUS_ERROR = 4;
const int STATUS_MANUAL = 5;
const int STATUS_HX711_ERROR = 6;

struct_node_status statusMsg;
struct_heartbeat heartbeatMsg;
esp_now_peer_info_t peerInfo;

enum DispState { D_IDLE, D_RUNNING, D_SETTLING };
DispState dispState = D_IDLE;

float currentWeight[3] = {0.0, 0.0, 0.0};
bool pumpActive[3] = {false, false, false};
int chemicalStatus[3] = {STATUS_IDLE, STATUS_IDLE, STATUS_IDLE};

int activeChemical = CHEMICAL_ALL;
float dispenseStartWeight = 0.0;
float dispenseTargetWeight = 0.0;

unsigned long lastWeightReadTime = 0;
unsigned long lastStatusSendTime = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastIdleStatus = 0;
unsigned long dispenseStartTime = 0;
unsigned long settleStartTime = 0;
unsigned long manualStartTime = 0;
unsigned long lastProgressTime = 0;
float lastProgressWeight = 0.0;
int nextIdleChemical = CHEMICAL_MOLASSES;

volatile bool hasPendingCmd = false;
struct_message pendingCmd;
portMUX_TYPE commandMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool scanPending = false;
volatile unsigned long scanReplyAt = 0;

bool isValidChemical(int chemical) {
  return chemical == CHEMICAL_MOLASSES || chemical == CHEMICAL_LIME;
}

const char *chemicalName(int chemical) {
  if (chemical == CHEMICAL_MOLASSES) return "molasses";
  if (chemical == CHEMICAL_LIME) return "lime";
  return "all";
}

HX711 *scaleFor(int chemical) {
  if (chemical == CHEMICAL_MOLASSES) return &molassesScale;
  if (chemical == CHEMICAL_LIME) return &limeScale;
  return nullptr;
}

int relayPinFor(int chemical) {
  return chemical == CHEMICAL_LIME ? LIME_RELAY_PIN : MOLASSES_RELAY_PIN;
}

void sendHeartbeat() {
  lastHeartbeatTime = millis();
  heartbeatMsg.node_id = NODE_ID;
  heartbeatMsg.is_online = true;
  esp_now_send(gatewayAddress, (uint8_t *)&heartbeatMsg, sizeof(heartbeatMsg));
}

void sendNodeStatus(int chemical) {
  if (!isValidChemical(chemical)) return;

  statusMsg.node_id = NODE_ID;
  statusMsg.chemical = chemical;
  statusMsg.weight = currentWeight[chemical];
  statusMsg.relay_status = pumpActive[chemical];
  statusMsg.status = chemicalStatus[chemical];

  if (activeChemical == chemical &&
      (dispState != D_IDLE || chemicalStatus[chemical] == STATUS_COMPLETE)) {
    statusMsg.start_weight = dispenseStartWeight;
    statusMsg.target_weight = dispenseTargetWeight;
    statusMsg.dispensed = dispenseStartWeight - currentWeight[chemical];
  } else {
    statusMsg.start_weight = 0.0;
    statusMsg.target_weight = 0.0;
    statusMsg.dispensed = 0.0;
  }

  esp_now_send(gatewayAddress, (uint8_t *)&statusMsg, sizeof(statusMsg));
}

void pumpOff(int chemical) {
  if (!isValidChemical(chemical)) return;
  digitalWrite(relayPinFor(chemical), HIGH);
  pumpActive[chemical] = false;
  Serial.print("Pump OFF: ");
  Serial.println(chemicalName(chemical));
}

void pumpOffAll() {
  pumpOff(CHEMICAL_MOLASSES);
  pumpOff(CHEMICAL_LIME);
}

void pumpOn(int chemical) {
  if (!isValidChemical(chemical)) return;

  // Software interlock: both pumps can never run together.
  int other = (chemical == CHEMICAL_MOLASSES) ? CHEMICAL_LIME : CHEMICAL_MOLASSES;
  pumpOff(other);
  digitalWrite(relayPinFor(chemical), LOW);
  pumpActive[chemical] = true;
  Serial.print("Pump ON: ");
  Serial.println(chemicalName(chemical));
}

bool readWeight(int chemical) {
  HX711 *scale = scaleFor(chemical);
  if (scale == nullptr || !scale->wait_ready_timeout(300)) {
    Serial.print("ERROR: HX711 not ready: ");
    Serial.println(chemicalName(chemical));
    return false;
  }

  float weight = scale->get_units(3);
  if (weight < 0) weight = 0;
  currentWeight[chemical] = weight;

  Serial.print("[NODE ");
  Serial.print(NODE_ID);
  Serial.print("] ");
  Serial.print(chemicalName(chemical));
  Serial.print(" weight: ");
  Serial.print(weight, 2);
  Serial.println(" g");
  return true;
}

void failSafe(int chemical, int status, const char *reason) {
  pumpOffAll();
  dispState = D_IDLE;
  if (isValidChemical(chemical)) {
    chemicalStatus[chemical] = status;
    activeChemical = chemical;
  }

  Serial.print("FAILSAFE: ");
  Serial.println(reason);
  if (isValidChemical(chemical)) sendNodeStatus(chemical);
  activeChemical = CHEMICAL_ALL;
}

void startDispense(int chemical, float amount) {
  if (!isValidChemical(chemical) || amount <= 0) {
    failSafe(chemical, STATUS_ERROR, "Invalid dispense request");
    return;
  }

  if (!readWeight(chemical)) {
    failSafe(chemical, STATUS_HX711_ERROR, "HX711 read failed");
    return;
  }

  if (currentWeight[chemical] > MAX_LOADCELL_WEIGHT_G) {
    failSafe(chemical, STATUS_ERROR, "Load cell overload");
    return;
  }
  if (amount > currentWeight[chemical]) {
    failSafe(chemical, STATUS_NOT_ENOUGH, "Not enough solution");
    return;
  }

  pumpOffAll();
  chemicalStatus[CHEMICAL_MOLASSES] = STATUS_IDLE;
  chemicalStatus[CHEMICAL_LIME] = STATUS_IDLE;
  activeChemical = chemical;
  dispenseStartWeight = currentWeight[chemical];
  dispenseTargetWeight = dispenseStartWeight - amount;
  if (dispenseTargetWeight < 0) dispenseTargetWeight = 0;

  unsigned long now = millis();
  dispenseStartTime = now;
  lastProgressTime = now;
  lastProgressWeight = currentWeight[chemical];
  lastWeightReadTime = now;
  lastStatusSendTime = now;

  dispState = D_RUNNING;
  chemicalStatus[chemical] = STATUS_DISPENSING;
  pumpOn(chemical);
  sendNodeStatus(chemical);
}

void processDispensing() {
  if (!isValidChemical(activeChemical)) return;
  unsigned long now = millis();
  int chemical = activeChemical;

  if (dispState == D_RUNNING) {
    if (now - lastWeightReadTime >= WEIGHT_READ_INTERVAL) {
      lastWeightReadTime = now;

      if (!readWeight(chemical)) {
        failSafe(chemical, STATUS_HX711_ERROR, "HX711 lost during dispense");
        return;
      }
      if (currentWeight[chemical] > MAX_LOADCELL_WEIGHT_G) {
        failSafe(chemical, STATUS_ERROR, "Overload during dispense");
        return;
      }

      if (currentWeight[chemical] < lastProgressWeight - PROGRESS_MIN_G) {
        lastProgressWeight = currentWeight[chemical];
        lastProgressTime = now;
      }
      if (now - lastProgressTime > NO_PROGRESS_TIMEOUT_MS) {
        failSafe(chemical, STATUS_ERROR, "No weight change (empty or blocked)");
        return;
      }
      if (now - dispenseStartTime > DISPENSE_MAX_MS) {
        failSafe(chemical, STATUS_ERROR, "Dispense timeout");
        return;
      }

      if (currentWeight[chemical] <= dispenseTargetWeight + STOP_OFFSET_G) {
        pumpOff(chemical);
        dispState = D_SETTLING;
        settleStartTime = now;
        return;
      }
    }

    if (now - lastStatusSendTime >= STATUS_SEND_INTERVAL) {
      lastStatusSendTime = now;
      sendNodeStatus(chemical);
    }
    return;
  }

  if (dispState == D_SETTLING && now - settleStartTime >= SETTLE_TIME) {
    if (!readWeight(chemical)) {
      failSafe(chemical, STATUS_HX711_ERROR, "HX711 read failed at settle");
      return;
    }

    chemicalStatus[chemical] = STATUS_COMPLETE;
    dispState = D_IDLE;
    sendNodeStatus(chemical);
    chemicalStatus[chemical] = STATUS_IDLE;
    activeChemical = CHEMICAL_ALL;
  }
}

void stopRequestedChemical(int chemical) {
  if (chemical == CHEMICAL_ALL) {
    pumpOffAll();
    chemicalStatus[CHEMICAL_MOLASSES] = STATUS_IDLE;
    chemicalStatus[CHEMICAL_LIME] = STATUS_IDLE;
    dispState = D_IDLE;
    activeChemical = CHEMICAL_ALL;
    sendNodeStatus(CHEMICAL_MOLASSES);
    delay(5);
    sendNodeStatus(CHEMICAL_LIME);
    return;
  }

  if (!isValidChemical(chemical)) return;
  pumpOff(chemical);
  chemicalStatus[chemical] = STATUS_IDLE;
  if (activeChemical == chemical) {
    dispState = D_IDLE;
    activeChemical = CHEMICAL_ALL;
  }
  readWeight(chemical);
  sendNodeStatus(chemical);
}

void executeCommand(const struct_message &cmd) {
  Serial.println();
  Serial.println("========== COMMAND ==========");
  Serial.print("Node: "); Serial.println(cmd.node_id);
  Serial.print("Command: "); Serial.println(cmd.command);
  Serial.print("Chemical: "); Serial.println(chemicalName(cmd.chemical));
  Serial.print("Amount: "); Serial.print(cmd.amount, 2); Serial.println(" g");
  Serial.println("=============================");

  if (cmd.command != 0 && !isValidChemical(cmd.chemical)) {
    Serial.println("Invalid chemical");
    return;
  }

  switch (cmd.command) {
    case 0:
      stopRequestedChemical(cmd.chemical);
      break;

    case 1:
      pumpOffAll();
      chemicalStatus[CHEMICAL_MOLASSES] = STATUS_IDLE;
      chemicalStatus[CHEMICAL_LIME] = STATUS_IDLE;
      dispState = D_IDLE;
      activeChemical = cmd.chemical;
      chemicalStatus[cmd.chemical] = STATUS_MANUAL;
      manualStartTime = millis();
      pumpOn(cmd.chemical);
      sendNodeStatus(cmd.chemical);
      break;

    case 2:
      if (dispState != D_IDLE) {
        Serial.println("Node is already dispensing");
        return;
      }
      if (isValidChemical(activeChemical) && pumpActive[activeChemical]) {
        pumpOffAll();
        chemicalStatus[activeChemical] = STATUS_IDLE;
      }
      startDispense(cmd.chemical, cmd.amount);
      break;

    case 10: {
      pumpOffAll();
      dispState = D_IDLE;
      activeChemical = CHEMICAL_ALL;
      HX711 *scale = scaleFor(cmd.chemical);
      if (scale == nullptr || !scale->wait_ready_timeout(1000)) {
        failSafe(cmd.chemical, STATUS_HX711_ERROR, "HX711 unavailable for tare");
        return;
      }
      scale->tare(10);
      currentWeight[cmd.chemical] = 0;
      chemicalStatus[cmd.chemical] = STATUS_IDLE;
      sendNodeStatus(cmd.chemical);
      break;
    }

    default:
      Serial.println("Unknown command");
      break;
  }
}

void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
}

void OnDataRecv(const esp_now_recv_info_t *info,
                const uint8_t *incomingData, int len) {
  const uint8_t *mac = info->src_addr;
  for (int i = 0; i < 6; i++) {
    if (mac[i] != gatewayAddress[i]) return;
  }
  if (len != sizeof(struct_message)) return;

  struct_message msg;
  memcpy(&msg, incomingData, sizeof(msg));

  if (msg.node_id == 255 && msg.command == 99) {
    scanReplyAt = millis() + (unsigned long)NODE_ID * STAGGER_STEP_MS;
    scanPending = true;
    return;
  }
  if (msg.node_id != NODE_ID) return;

  portENTER_CRITICAL(&commandMux);
  pendingCmd = msg;
  hasPendingCmd = true;
  portEXIT_CRITICAL(&commandMux);
}

void handleSerialSetup() {
  static String buf = "";

  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      buf.trim();
      if (buf.startsWith("setid")) {
        int id = buf.substring(5).toInt();
        if (id >= 1 && id <= 20) {
          prefs.begin("node", false);
          prefs.putInt("id", id);
          prefs.end();
          Serial.print("Saved NODE_ID = ");
          Serial.println(id);
          delay(300);
          ESP.restart();
        } else {
          Serial.println("NODE_ID must be 1-20");
        }
      }
      buf = "";
    } else if (buf.length() < 32) {
      buf += c;
    }
  }
}

bool initializeScale(int chemical) {
  HX711 *scale = scaleFor(chemical);
  if (scale == nullptr) return false;

  if (chemical == CHEMICAL_MOLASSES) {
    scale->begin(MOLASSES_DOUT_PIN, MOLASSES_SCK_PIN);
    scale->set_scale(MOLASSES_CALIBRATION_FACTOR);
  } else {
    scale->begin(LIME_DOUT_PIN, LIME_SCK_PIN);
    scale->set_scale(LIME_CALIBRATION_FACTOR);
  }

  Serial.print("Initializing HX711: ");
  Serial.println(chemicalName(chemical));
  if (!scale->wait_ready_timeout(3000)) {
    chemicalStatus[chemical] = STATUS_HX711_ERROR;
    Serial.println("HX711 not found");
    return false;
  }

  scale->tare(10);
  currentWeight[chemical] = 0;
  chemicalStatus[chemical] = STATUS_IDLE;
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  prefs.begin("node", true);
  NODE_ID = prefs.getInt("id", 1);
  prefs.end();

  pinMode(MOLASSES_RELAY_PIN, OUTPUT);
  pinMode(LIME_RELAY_PIN, OUTPUT);
  digitalWrite(MOLASSES_RELAY_PIN, HIGH);
  digitalWrite(LIME_RELAY_PIN, HIGH);

  Serial.println();
  Serial.println("================================");
  Serial.println("DUAL-CHEMICAL NODE INITIALIZING");
  Serial.println("Do not change either load-cell weight during tare");
  Serial.println("================================");

  initializeScale(CHEMICAL_MOLASSES);
  initializeScale(CHEMICAL_LIME);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW INIT ERROR");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);

  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, gatewayAddress, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add Gateway peer");
  } else {
    Serial.println("Gateway peer added");
  }

  Serial.print("Node ID: ");
  Serial.println(NODE_ID);
  Serial.println("Molasses: relay 26, DOUT 32, SCK 33");
  Serial.println("Lime: relay 27, DOUT 25, SCK 14");

  delay((unsigned long)NODE_ID * STAGGER_STEP_MS);
  readWeight(CHEMICAL_MOLASSES);
  readWeight(CHEMICAL_LIME);
  sendHeartbeat();
  delay(5);
  sendNodeStatus(CHEMICAL_MOLASSES);
  delay(5);
  sendNodeStatus(CHEMICAL_LIME);

  lastIdleStatus = millis() -
                   (unsigned long)(NODE_ID * 97) % IDLE_STATUS_INTERVAL;
}

void loop() {
  unsigned long now = millis();
  handleSerialSetup();

  if (hasPendingCmd) {
    struct_message cmd;
    portENTER_CRITICAL(&commandMux);
    cmd = pendingCmd;
    hasPendingCmd = false;
    portEXIT_CRITICAL(&commandMux);
    executeCommand(cmd);
  }

  if (scanPending && (long)(now - scanReplyAt) >= 0) {
    scanPending = false;
    sendHeartbeat();
    delay(5);
    if (readWeight(CHEMICAL_MOLASSES)) sendNodeStatus(CHEMICAL_MOLASSES);
    delay(5);
    if (readWeight(CHEMICAL_LIME)) sendNodeStatus(CHEMICAL_LIME);
  }

  processDispensing();

  if (isValidChemical(activeChemical) &&
      chemicalStatus[activeChemical] == STATUS_MANUAL &&
      pumpActive[activeChemical] && now - manualStartTime > MANUAL_MAX_MS) {
    failSafe(activeChemical, STATUS_ERROR, "Manual ON timeout");
  }

  if (now - lastHeartbeatTime >= HEARTBEAT_INTERVAL) sendHeartbeat();

  // Alternate readings to avoid sending both status packets simultaneously.
  if (dispState == D_IDLE && now - lastIdleStatus >= IDLE_STATUS_INTERVAL) {
    lastIdleStatus = now;
    int chemical = nextIdleChemical;
    nextIdleChemical = (nextIdleChemical == CHEMICAL_MOLASSES)
                       ? CHEMICAL_LIME : CHEMICAL_MOLASSES;

    if (readWeight(chemical)) {
      if (chemicalStatus[chemical] == STATUS_HX711_ERROR) {
        chemicalStatus[chemical] = STATUS_IDLE;
      }
    } else {
      chemicalStatus[chemical] = STATUS_HX711_ERROR;
    }
    sendNodeStatus(chemical);
  }
}
