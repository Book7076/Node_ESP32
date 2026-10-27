#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

const int MAX_NODES = 20;
const int WIFI_CHANNEL = 6;
const int RX_QUEUE_SIZE = 32;
const int RX_MAX_LEN = 48;
const int SERIAL_BUF_MAX = 256;

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum ChemicalType {
  CHEMICAL_ALL = 0,
  CHEMICAL_MOLASSES = 1,
  CHEMICAL_LIME = 2
};

// These structures must exactly match node_multi.ino.
typedef struct struct_message {
  int node_id;
  int command;       // 0 OFF, 1 Manual ON, 2 Dispense, 10 Tare, 99 Scan
  int chemical;      // 0 all, 1 molasses, 2 lime
  float amount;      // grams
} struct_message;

typedef struct struct_heartbeat {
  int node_id;
  bool is_online;
} struct_heartbeat;

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

static_assert(sizeof(struct_heartbeat) != sizeof(struct_node_status),
              "heartbeat and status must have different sizes");
static_assert(sizeof(struct_message) != sizeof(struct_heartbeat),
              "message and heartbeat must have different sizes");

struct_message myData;
esp_now_peer_info_t peerInfo;

struct RxItem {
  uint8_t len;
  uint8_t data[RX_MAX_LEN];
};

RxItem rxQueue[RX_QUEUE_SIZE];
volatile int rxHead = 0;
volatile int rxTail = 0;
volatile uint32_t rxDropped = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool sendResultPending = false;
volatile bool sendResultOk = false;

void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  sendResultOk = (status == ESP_NOW_SEND_SUCCESS);
  sendResultPending = true;
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len <= 0 || len > RX_MAX_LEN) return;

  portENTER_CRITICAL(&rxMux);
  int next = (rxHead + 1) % RX_QUEUE_SIZE;
  if (next == rxTail) {
    rxDropped++;
  } else {
    rxQueue[rxHead].len = (uint8_t)len;
    memcpy(rxQueue[rxHead].data, data, len);
    rxHead = next;
  }
  portEXIT_CRITICAL(&rxMux);
}

bool popRx(RxItem &out) {
  bool ok = false;
  portENTER_CRITICAL(&rxMux);
  if (rxTail != rxHead) {
    out = rxQueue[rxTail];
    rxTail = (rxTail + 1) % RX_QUEUE_SIZE;
    ok = true;
  }
  portEXIT_CRITICAL(&rxMux);
  return ok;
}

const char *statusToText(int status) {
  switch (status) {
    case 0: return "idle";
    case 1: return "dispensing";
    case 2: return "complete";
    case 3: return "not_enough_weight";
    case 4: return "error";
    case 5: return "manual";
    case 6: return "hx711_error";
    default: return "unknown";
  }
}

const char *chemicalToText(int chemical) {
  switch (chemical) {
    case CHEMICAL_ALL: return "all";
    case CHEMICAL_MOLASSES: return "molasses";
    case CHEMICAL_LIME: return "lime";
    default: return "unknown";
  }
}

void handleRx(const RxItem &item) {
  char buf[320];

  if (item.len == sizeof(struct_heartbeat)) {
    struct_heartbeat hb;
    memcpy(&hb, item.data, sizeof(hb));
    if (hb.node_id < 1 || hb.node_id > MAX_NODES) return;

    snprintf(buf, sizeof(buf),
             "{\"action\":\"node_status\",\"node_id\":%d,\"status\":\"%s\"}",
             hb.node_id, hb.is_online ? "online" : "offline");
    Serial.println(buf);
    return;
  }

  if (item.len == sizeof(struct_node_status)) {
    struct_node_status st;
    memcpy(&st, item.data, sizeof(st));
    if (st.node_id < 1 || st.node_id > MAX_NODES) return;
    if (st.chemical != CHEMICAL_MOLASSES && st.chemical != CHEMICAL_LIME) return;

    snprintf(buf, sizeof(buf),
             "{\"action\":\"weight\",\"node_id\":%d,\"chemical\":\"%s\"," 
             "\"weight\":%.2f,\"pump\":%s,\"status\":\"%s\"," 
             "\"start_weight\":%.2f,\"target_weight\":%.2f,\"dispensed\":%.2f}",
             st.node_id, chemicalToText(st.chemical), st.weight,
             st.relay_status ? "true" : "false", statusToText(st.status),
             st.start_weight, st.target_weight, st.dispensed);
    Serial.println(buf);
  }
}

bool sendCommand(int node, int command, int chemical, float amount) {
  myData.node_id = node;
  myData.command = command;
  myData.chemical = chemical;
  myData.amount = amount;

  esp_err_t result =
    esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));

  char buf[180];
  if (result == ESP_OK) {
    snprintf(buf, sizeof(buf),
             "{\"status\":\"sent_espnow\",\"node\":%d,\"cmd\":%d," 
             "\"chemical\":\"%s\",\"amount\":%.2f}",
             node, command, chemicalToText(chemical), amount);
  } else {
    snprintf(buf, sizeof(buf),
             "{\"status\":\"error_espnow\",\"node\":%d,\"error_code\":%d}",
             node, (int)result);
  }
  Serial.println(buf);
  return result == ESP_OK;
}

bool extractNumber(const String &s, const char *key, float &out) {
  int idx = s.indexOf(key);
  if (idx < 0) return false;

  int i = idx + strlen(key);
  if (i < (int)s.length() && s.charAt(i) == ':') i++;

  String num = "";
  bool dot = false;
  for (; i < (int)s.length(); i++) {
    char c = s.charAt(i);
    if (c >= '0' && c <= '9') {
      num += c;
    } else if (c == '.' && !dot) {
      num += c;
      dot = true;
    } else if (c == '-' && num.length() == 0) {
      num += c;
    } else {
      break;
    }
  }
  if (num.length() == 0 || num == "-") return false;
  out = num.toFloat();
  return true;
}

int parseNodeId(const String &s) {
  int idx = s.indexOf("feeder_");
  if (idx >= 0) return s.substring(idx + 7).toInt();

  float value;
  if (extractNumber(s, "\"node_id\"", value)) return (int)value;
  return 0;
}

int parseCommand(const String &s) {
  float value;

  if (s.indexOf("\"action\":\"dispense\"") >= 0) return 2;
  if (s.indexOf("\"action\":\"tare\"") >= 0) return 10;

  if (extractNumber(s, "\"command\"", value)) {
    int command = (int)value;
    if (command == 0 || command == 1 || command == 2 || command == 10) {
      return command;
    }
    return -1;
  }

  if (s.indexOf("\"state\":\"on\"") >= 0) return 1;
  if (s.indexOf("\"state\":\"off\"") >= 0) return 0;
  if (extractNumber(s, "\"state\"", value)) return ((int)value == 1) ? 1 : 0;
  if (s.indexOf("true") >= 0) return 1;
  if (s.indexOf("false") >= 0) return 0;
  return -1;
}

// Old commands without "chemical" still control molasses.
// For safety, OFF without "chemical" stops both pumps.
int parseChemical(const String &s, int command) {
  if (s.indexOf("\"chemical\":\"molasses\"") >= 0) return CHEMICAL_MOLASSES;
  if (s.indexOf("\"chemical\":\"lime\"") >= 0) return CHEMICAL_LIME;
  if (s.indexOf("\"chemical\":\"all\"") >= 0) return CHEMICAL_ALL;

  float value;
  if (extractNumber(s, "\"chemical\"", value)) {
    int chemical = (int)value;
    if (chemical >= CHEMICAL_ALL && chemical <= CHEMICAL_LIME) return chemical;
    return -1;
  }

  if (s.indexOf("\"chemical\"") >= 0) return -1;
  return (command == 0) ? CHEMICAL_ALL : CHEMICAL_MOLASSES;
}

void printIgnored(const char *msg) {
  Serial.print("{\"status\":\"ignored\",\"msg\":\"");
  Serial.print(msg);
  Serial.println("\"}");
}

void processCommand(String input) {
  input.trim();
  if (input.length() == 0) return;

  String s = input;
  s.replace(" ", "");

  if (s.equalsIgnoreCase("scan") ||
      s.indexOf("\"command\":\"scan\"") >= 0 ||
      s.indexOf("\"action\":\"scanNodes\"") >= 0) {
    Serial.println("{\"status\":\"scanning\"}");
    sendCommand(255, 99, CHEMICAL_ALL, 0);
    return;
  }

  int node = parseNodeId(s);
  if (node < 1 || node > MAX_NODES) {
    printIgnored("Invalid node");
    return;
  }

  int command = parseCommand(s);
  if (command < 0) {
    printIgnored("Invalid command");
    return;
  }

  int chemical = parseChemical(s, command);
  if (chemical < 0 || (chemical == CHEMICAL_ALL && command != 0)) {
    printIgnored("Invalid chemical");
    return;
  }

  float amount = 0;
  extractNumber(s, "\"amount\"", amount);
  if (command == 2 && amount <= 0) {
    printIgnored("Dispense needs amount > 0");
    return;
  }

  sendCommand(node, command, chemical, amount);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("{\"status\":\"error\",\"msg\":\"Failed to set WiFi channel\"}");
  }

  Serial.println();
  Serial.println("======================================");
  Serial.println("ESP-NOW DUAL-CHEMICAL GATEWAY");
  Serial.println("======================================");
  Serial.print("Gateway MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Channel: ");
  Serial.println(WIFI_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("{\"status\":\"error\",\"msg\":\"ESP-NOW init failed\"}");
    return;
  }
  if (esp_now_register_send_cb(OnDataSent) != ESP_OK) {
    Serial.println("{\"status\":\"error\",\"msg\":\"Send callback failed\"}");
    return;
  }
  if (esp_now_register_recv_cb(OnDataRecv) != ESP_OK) {
    Serial.println("{\"status\":\"error\",\"msg\":\"Receive callback failed\"}");
    return;
  }

  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("{\"status\":\"error\",\"msg\":\"Failed to add broadcast peer\"}");
    return;
  }

  Serial.println("{\"status\":\"ready\",\"msg\":\"Gateway initialization complete\"}");
}

String inputBuffer = "";
unsigned long lastDropReport = 0;

void loop() {
  RxItem item;
  int guard = 0;
  while (guard++ < 8 && popRx(item)) handleRx(item);

  if (sendResultPending) {
    sendResultPending = false;
    Serial.println(sendResultOk ? "{\"status\":\"espnow_ack\"}"
                                : "{\"status\":\"espnow_nack\"}");
  }

  if (rxDropped > 0 && millis() - lastDropReport > 5000) {
    lastDropReport = millis();
    Serial.print("{\"status\":\"warning\",\"msg\":\"rx queue overflow\",\"dropped\":");
    Serial.print((uint32_t)rxDropped);
    Serial.println("}");
  }

  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        processCommand(inputBuffer);
        inputBuffer = "";
      }
    } else if (inputBuffer.length() < SERIAL_BUF_MAX) {
      inputBuffer += c;
    } else {
      inputBuffer = "";
      printIgnored("Input too long");
    }
  }
}
