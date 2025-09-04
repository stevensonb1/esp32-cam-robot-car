  #include "esp_camera.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"             // disable brownout problems
#include "soc/rtc_cntl_reg.h"    // disable brownout problems
#include "esp_http_server.h"

const char* ssid = "Freddys s20";
const char* password = "ppst2649";

IPAddress local_IP(192, 168, 1, 184); // desired IP
IPAddress gateway(192, 168, 1, 1); // routers IP
IPAddress subnet(255, 255, 255, 0); // subnet mask

#define PART_BOUNDARY "123456789000000000000987654321"

#define CAMERA_MODEL_WROVER_KIT // Has PSRAM
//#define CAMERA_MODEL_ESP_EYE  // Has PSRAM
//#define CAMERA_MODEL_ESP32S3_EYE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_PSRAM // Has PSRAM
//#define CAMERA_MODEL_M5STACK_V2_PSRAM // M5Camera version B Has PSRAM
//#define CAMERA_MODEL_M5STACK_WIDE // Has PSRAM
//#define CAMERA_MODEL_M5STACK_ESP32CAM // No PSRAM
//#define CAMERA_MODEL_M5STACK_UNITCAM // No PSRAM
//#define CAMERA_MODEL_M5STACK_CAMS3_UNIT  // Has PSRAM
//#defi+CAMERA_MODEL_AI_THINKER // Has PSRAM
//#define CAMERA_MODEL_TTGO_T_JOURNAL // No PSRAM
//#define CAMERA_MODEL_XIAO_ESP32S3 // Has PSRAM
// ** Espressif Internal Boards **
//#define CAMERA_MODEL_ESP32_CAM_BOARD
//#define CAMERA_MODEL_ESP32S2_CAM_BOARD
//#define CAMERA_MODEL_ESP32S3_CAM_LCD
//#define CAMERA_MODEL_DFRobot_FireBeetle2_ESP32S3 // Has PSRAM
//#define CAMERA_MODEL_DFRobot_Romeo_ESP32S3 // Has PSRAM
#include "camera_pins.h"

bool wasConnected = true;

// Motor A pins
const int AIN1 = 13;
const int AIN2 = 12;
const int PWMA = 14;

// Motor B pins
const int BIN1 = 33;
const int BIN2 = 32;
const int PWMB = 27;

TaskHandle_t Task1;

static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

struct Command {
  String cmd;
  unsigned long delayMs;
};

#define MAX_RECORDS 100
Command recordedPath[MAX_RECORDS];
int recordCount = 0;
bool isRecording = false;
unsigned long lastRecordTime = 0;
bool replayReverse = false;

static const char PROGMEM RECORD_HTML[] = R"rawliteral(
<html>
  <head>
    <title>Record & Replay</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body { font-family: Arial; text-align: center; padding-top: 50px; }
      .button {
        background-color: #2f4468;
        border: none;
        color: white;
        padding: 12px 25px;
        font-size: 18px;
        margin: 8px 4px;
        cursor: pointer;
      }
      label { display:block; margin-top:10px; }
    </style>
  </head>
  <body>
    <h1>Record & Replay Path</h1>
    <button class="button" onclick="fetch('/record?start')">Start Recording</button>
    <button class="button" onclick="fetch('/record?stop')">Stop Recording</button>
    <button class="button" onclick="fetch('/replay')">Replay Path</button>

    <label for="replayOrder">Replay Order:</label>
    <select id="replayOrder" onchange="setReplayOrder(this.value)">
      <option value="normal">Normal</option>
      <option value="reverse">Reverse</option>
    </select>

    <script>
      function setReplayOrder(order) {
        fetch(`/replay_order?${order}`);
      }
    </script>

    <br><br>
    <a href="/">Back to Main Page</a>
  </body>
</html>
)rawliteral";


static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<html>
  <head>
    <title>Nukinator Robot</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body { font-family: Arial; text-align: center; margin:0; padding:0; }
      #mainContent {
        padding: 30px 10px 250px 10px; /* bottom padding leaves space for joystick */
      }
      .button {
        background-color: #2f4468;
        border: none;
        color: white;
        padding: 12px 25px;
        font-size: 18px;
        margin: 10px auto;
        cursor: pointer;
        display: block;
        min-width: 180px;
      }
      img { width: auto; max-width: 100%; height: auto; }
      #joystick {
        position: fixed;
        bottom: 20px;
        left: 50%;
        transform: translateX(-50%);
        width: 200px;
        height: 200px;
        z-index: 1000;
      }
      .modal {
        display: none;
        position: fixed;
        z-index: 2000;
        left: 0; top: 0;
        width: 100%; height: 100%;
        background-color: rgba(0,0,0,0.6);
      }
      .modal-content {
        background: #fff;
        margin: 10% auto;
        padding: 20px;
        border-radius: 10px;
        width: 90%;
        max-width: 400px;
        text-align: left;
      }
      .close {
        color: red;
        float: right;
        font-size: 24px;
        cursor: pointer;
      }
      label { display: block; margin-top: 10px; }
    </style>
  </head>
  <body>
    <div id="mainContent">
      <h1>Nukinator with Joystick</h1>
      <button class="button" id="openConfig">Camera Config</button>
      <button class="button" id="toggleStream">Stop Stream</button>
      <br>
      <img src="" id="photo" >
      <button class="button" onclick="window.location.href='/record_page'">Record & Replay</button>
    </div>

    <div id="joystick"></div>

    <!-- Modal for Config -->
    <div id="configModal" class="modal">
      <div class="modal-content">
        <span class="close" id="closeConfig">&times;</span>
        <h2>Camera Settings</h2>
        <label for="resSelect">Resolution:</label>
        <select id="resSelect">
          <option value="8">QVGA (320x240)</option>
          <option value="7">240x240</option>
          <option value="6">HQVGA (240x176)</option>
          <option value="5">QCIF (176x144)</option>
          <option value="4">128x128</option>
          <option value="3">QQVGA (160x120)</option>
          <option value="2">96x96</option>
        </select>
        <label for="qualityRange">Quality:</label>
        <input type="range" id="qualityRange" min="1" max="150" value="10">
        <span id="qualityValue">10</span>
      </div>
    </div>

    <script src="https://cdnjs.cloudflare.com/ajax/libs/nipplejs/0.9.0/nipplejs.min.js"></script>
    <script>
      function sendCommand(command) { fetch("/action?go=" + command); }
      function setCamera(param, value) { fetch(`/set?${param}=${value}`); }

      window.onload = function() {
        const photo = document.getElementById("photo");
        const streamBtn = document.getElementById("toggleStream");
        let streaming = true;

        function startStream() {
          photo.src = window.location.origin + ":81/stream";
          streaming = true;
          streamBtn.textContent = "Stop Stream";
        }
        function stopStream() {
          photo.src = "";
          streaming = false;
          streamBtn.textContent = "Start Stream";
        }

        startStream();

        streamBtn.onclick = function() {
          if (streaming) stopStream();
          else startStream();
        };

        const joystick = nipplejs.create({
          zone: document.getElementById('joystick'),
          mode: 'static',
          position: { left: '50%', top: '50%' },
          color: 'blue',
          size: 200
        });

        let lastCommand = "";
        let lastSentTime = 0;
        const throttleInterval = 200;

        joystick.on('move', function (evt, data) {
          if (!data.direction) return;
          let direction = data.direction.angle;
          let distance = data.distance;
          let maxDistance = joystick.options.size / 2;
          let speed = Math.min(100, Math.round((distance / maxDistance) * 100));
          let command = direction + ":" + speed;
          let now = Date.now();
          if (command !== lastCommand && (now - lastSentTime > throttleInterval)) {
            lastCommand = command;
            lastSentTime = now;
            sendCommand(command);
          }
        });

        joystick.on('end', function () {
          sendCommand("stop");
          lastCommand = "";
        });

        const modal = document.getElementById("configModal");
        document.getElementById("openConfig").onclick = () => modal.style.display = "block";
        document.getElementById("closeConfig").onclick = () => modal.style.display = "none";
        window.onclick = (e) => { if (e.target == modal) modal.style.display = "none"; };

        document.getElementById("resSelect").onchange = function() {
          setCamera("framesize", this.value);
        };
        document.getElementById("qualityRange").oninput = function() {
          document.getElementById("qualityValue").innerText = this.value;
          setCamera("quality", this.value);
        };
      };
    </script>
  </body>
</html>
)rawliteral";


static const char PROGMEM CONFIG_HTML[] = R"rawliteral(
<html>
  <head>
    <title>Camera Config</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body { font-family: Arial; text-align: center; background: #f4f4f4; margin: 0; padding: 20px; }
      h2 { color: #2f4468; }
      label { display: block; margin-top: 20px; font-weight: bold; }
      select, input[type=range] {
        width: 80%; max-width: 300px;
        padding: 8px; margin-top: 10px; border-radius: 5px; border: 1px solid #ccc;
      }
      .button {
        background-color: #4CAF50; border: none; color: white;
        padding: 10px 20px; font-size: 16px; margin-top: 20px;
        border-radius: 5px; cursor: pointer;
      }
      .value-display { font-weight: bold; margin-left: 10px; }
    </style>
  </head>
  <body>
    <h2>Camera Configuration</h2>

    <!-- Resolution -->
    <label for="resolution">Resolution</label>
    <select id="resolution" onchange="updateSetting('framesize', this.value)">
      <option value="8">QVGA (320x240)</option>
      <option value="12">240x240</option>
      <option value="11">HQVGA (240x176)</option>
      <option value="7">QCIF (176x144)</option>
      <option value="13">128x128</option>
      <option value="10">QQVGA (160x120)</option>
      <option value="14">96x96</option>
    </select>

    <!-- Quality -->
    <label for="quality">JPEG Quality</label>
    <input type="range" id="quality" min="1" max="63" value="10" oninput="updateQualityValue(this.value)" onchange="updateSetting('quality', this.value)">
    <span class="value-display" id="qualityValue">10</span>

    <br><br>
    <button class="button" onclick="window.location.href='/'">Back to Joystick</button>

    <script>
      function updateSetting(param, value) {
        fetch(`/set?${param}=${value}`).catch(err => console.error(err));
      }

      function updateQualityValue(val) {
        document.getElementById('qualityValue').innerText = val;
      }
    </script>
  </body>
</html>
)rawliteral";

void stopMotors() {
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, LOW);
  analogWrite(PWMA, 0);

  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, LOW);
  analogWrite(PWMB, 0);
}

static esp_err_t index_handler(httpd_req_t *req){
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t record_page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)RECORD_HTML, strlen(RECORD_HTML));
}

static esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK){
    return res;
  }

  while(true){
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if(fb->width > 400){
        if(fb->format != PIXFORMAT_JPEG){
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if(!jpeg_converted){
            Serial.println("JPEG compression failed");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if(res == ESP_OK){
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if(fb){
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if(_jpg_buf){
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if(res != ESP_OK){
      break;
    }
    //Serial.printf("MJPG: %uB\n",(uint32_t)(_jpg_buf_len));
  }
  return res;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char*  buf;
  size_t buf_len;
  char variable[32] = {0,};
  
  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char*)malloc(buf_len);
    if(!buf){
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      if (httpd_query_key_value(buf, "go", variable, sizeof(variable)) != ESP_OK) {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
      }
    } else {
      free(buf);
      httpd_resp_send_404(req);
      return ESP_FAIL;
    }
    free(buf);
  } else {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  char *dir = strtok(variable, ":");
  char *spdStr = strtok(NULL, ":");
  int speed = 255;

  String fullCmd = String(dir);
  if (spdStr) {
    fullCmd += ":" + String(speed);
  }

  if (isRecording && recordCount < MAX_RECORDS) {
    unsigned long now = millis();
    recordedPath[recordCount].cmd = fullCmd;
    recordedPath[recordCount].delayMs = now - lastRecordTime;
    lastRecordTime = now;
    recordCount++;
  }

  if (spdStr != NULL) {
    int jsSpeed = atoi(spdStr); 
    speed = map(jsSpeed, 0, 100, 0, 255);
  }

  if (dir && !strcmp(dir, "up")) {
    Serial.printf("Forward at %d\n", speed);
    digitalWrite(AIN1, LOW);
    digitalWrite(AIN2, HIGH);
    analogWrite(PWMA, speed);

    digitalWrite(BIN1, HIGH);
    digitalWrite(BIN2, LOW);
    analogWrite(PWMB, speed);
  }
  else if (dir && !strcmp(dir, "left")) {
    Serial.printf("Left at %d\n", speed);
    digitalWrite(AIN1, LOW);
    digitalWrite(AIN2, HIGH);
    analogWrite(PWMA, speed);

    digitalWrite(BIN1, HIGH);
    digitalWrite(BIN2, LOW);
    analogWrite(PWMB, speed/2);
  }
  else if (dir && !strcmp(dir, "right")) {
    Serial.printf("Right at %d\n", speed);
    digitalWrite(AIN1, LOW);
    digitalWrite(AIN2, HIGH);
    analogWrite(PWMA, speed/2);

    digitalWrite(BIN1, HIGH);
    digitalWrite(BIN2, LOW);
    analogWrite(PWMB, speed);
  }
  else if (dir && !strcmp(dir, "down")) {
    Serial.printf("Backward at %d\n", speed);
    digitalWrite(AIN1, HIGH);
    digitalWrite(AIN2, LOW);
    analogWrite(PWMA, speed);

    digitalWrite(BIN1, LOW);
    digitalWrite(BIN2, HIGH);
    analogWrite(PWMB, speed);
  }
  else if (dir && !strcmp(dir, "stop")) {
    Serial.println("Stop");
    digitalWrite(AIN1, LOW);
    digitalWrite(AIN2, LOW);
    analogWrite(PWMA, 0);
  
    digitalWrite(BIN1, LOW);
    digitalWrite(BIN2, LOW);
    analogWrite(PWMB, 0);
  }
  else {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

void executeCommand(String cmd) {
  int sep = cmd.indexOf(":");
  String dir = cmd.substring(0, sep);
  int spd = cmd.substring(sep+1).toInt();

  if (dir == "up") {
    digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH); analogWrite(PWMA, spd);
    digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW); analogWrite(PWMB, spd);
  }
  else if (dir == "down") {
    digitalWrite(AIN1, HIGH); digitalWrite(AIN2, LOW); analogWrite(PWMA, spd);
    digitalWrite(BIN1, LOW); digitalWrite(BIN2, HIGH); analogWrite(PWMB, spd);
  }
  else if (dir == "left") {
    digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH); analogWrite(PWMA, spd);
    digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW); analogWrite(PWMB, spd/2);
  }
  else if (dir == "right") {
    digitalWrite(AIN1, LOW); digitalWrite(AIN2, HIGH); analogWrite(PWMA, spd/2);
    digitalWrite(BIN1, HIGH); digitalWrite(BIN2, LOW); analogWrite(PWMB, spd);
  }
  else if (dir == "stop") {
    stopMotors();
  }
}

void replayPath() {
  Serial.println("Replaying path...");
  if (replayReverse) {
    for (int i = recordCount - 1; i >= 0; i--) {
      delay(recordedPath[i].delayMs);
      executeCommand(recordedPath[i].cmd);
    }
  } else {
    for (int i = 0; i < recordCount; i++) {
      delay(recordedPath[i].delayMs);
      executeCommand(recordedPath[i].cmd);
    }
  }
  stopMotors();
  Serial.println("Replay finished.");
}

static esp_err_t record_handler(httpd_req_t *req) {
  char buf[32];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    if (strstr(buf, "start")) {
      isRecording = true;
      recordCount = 0;
      lastRecordTime = millis();
      Serial.println("Recording started.");
    } else if (strstr(buf, "stop")) {
      isRecording = false;
      Serial.printf("Recording stopped. %d steps saved.\n", recordCount);
    }
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t replay_handler(httpd_req_t *req) {
  replayPath();
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t replay_order_handler(httpd_req_t *req) {
  char buf[32];
  if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
    if (strstr(buf, "reverse")) {
      replayReverse = true;
      Serial.println("Replay order set: REVERSE");
    } else if (strstr(buf, "normal")) {
      replayReverse = false;
      Serial.println("Replay order set: NORMAL");
    }
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t config_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)CONFIG_HTML, strlen(CONFIG_HTML));
}
static esp_err_t set_handler(httpd_req_t *req) {
  char* buf;
  size_t buf_len;
  char param[32] = {0};
  char value[32] = {0};

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char*)malloc(buf_len);
    if(!buf) return httpd_resp_send_500(req);

    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      if (httpd_query_key_value(buf, "framesize", value, sizeof(value)) == ESP_OK) {
        sensor_t * s = esp_camera_sensor_get();
        s->set_framesize(s, (framesize_t)atoi(value));
      }
      if (httpd_query_key_value(buf, "quality", value, sizeof(value)) == ESP_OK) {
        sensor_t * s = esp_camera_sensor_get();
        s->set_quality(s, atoi(value));
      }
    }
    free(buf);
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}


void startCameraServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t cmd_uri = {
    .uri       = "/action",
    .method    = HTTP_GET,
    .handler   = cmd_handler,
    .user_ctx  = NULL
  };
  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t config_uri = {
    .uri       = "/config",
    .method    = HTTP_GET,
    .handler   = config_handler,
    .user_ctx  = NULL
  };
  
  httpd_uri_t set_uri = {
    .uri       = "/set",
    .method    = HTTP_GET,
    .handler   = set_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t record_uri = {
    .uri       = "/record",
    .method    = HTTP_GET,
    .handler   = record_handler,
    .user_ctx  = NULL
  };
  httpd_uri_t replay_uri = {
    .uri       = "/replay",
    .method    = HTTP_GET,
    .handler   = replay_handler,
    .user_ctx  = NULL
  };
   httpd_uri_t replay_order_uri = {
    .uri       = "/replay_order",
    .method    = HTTP_GET,
    .handler   = replay_order_handler,
    .user_ctx  = NULL
  };
  httpd_uri_t record_page_uri = {
    .uri       = "/record_page",
    .method    = HTTP_GET,
    .handler   = record_page_handler,
    .user_ctx  = NULL
  };


  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &config_uri);  // ADD
    httpd_register_uri_handler(camera_httpd, &set_uri);     // ADD
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
    httpd_register_uri_handler(camera_httpd, &record_uri);
    httpd_register_uri_handler(camera_httpd, &replay_uri);
    httpd_register_uri_handler(camera_httpd, &replay_order_uri);
    httpd_register_uri_handler(camera_httpd, &record_page_uri);
  }
  config.server_port += 1;
  config.ctrl_port += 1;
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void MotorCode( void * parameter) {
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(PWMA, OUTPUT);
  
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);
  pinMode(PWMB, OUTPUT);


  vTaskDelete(NULL);
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector

  xTaskCreatePinnedToCore(
    MotorCode,
    "Motor",
    2000,
    NULL,     
    1,
    &Task1,
    0);
  
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG; 
  //config.pixel_format = PIXFORMAT_RGB565;
   
  if(psramFound()){
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }
  
  // Camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  // Set static IP
//  if (!WiFi.config(local_IP, gateway, subnet)) {
//    Serial.println("STA Failed to configure");
//  }
//  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");
  
  Serial.print("Camera Stream Ready! Go to: http://");
  Serial.println(WiFi.localIP());
  
  startCameraServer();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED && wasConnected) {
    Serial.println("WiFi connection lost! Stopping motors...");
    stopMotors();
    wasConnected = false;
  } else if (WiFi.status() == WL_CONNECTED && !wasConnected) {
    Serial.println("WiFi reconnected.");
    wasConnected = true;
  }

  delay(500);
}
