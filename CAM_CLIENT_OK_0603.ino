#include <WiFi.h>              // Wi-Fi 연결을 위한 라이브러리
#include <HTTPClient.h>        // HTTP 요청을 위한 라이브러리
#include <ArduinoJson.h>       // JSON 파싱용 라이브러리
#include <SD.h>                // SD 카드 파일 시스템 라이브러리

// 버튼 입력 핀 정의
const int buttonPin = 6;       // 버튼이 눌리면 LOW 상태가 됨

// Wi-Fi 정보
const char* ssid = "WeVO_2.4G";         // 연결할 Wi-Fi SSID
const char* password = "Toolbox";     // 연결할 Wi-Fi 비밀번호

// 서버 IP 주소 (ESP32-CAM 쪽)
const char* serverIP = "192.168.10.4";  // ESP32-CAM 웹서버 주소

// 상태 머신 정의
enum TransferState { IDLE, GETTING_LIST, DOWNLOADING, COMPLETE, ERROR };
TransferState state = IDLE;    // 초기 상태는 대기(IDLE)

// 서버에서 받을 파일 목록
String fileList[50];           // 최대 50개 파일까지 저장 가능
int fileCount = 0;             // 총 파일 수
int currentFileIndex = 0;      // 현재 다운로드 중인 파일 인덱스

// 버튼 디바운싱용 변수
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;  // 50ms 이상 지속 시 눌림으로 판단

void setup() {
  Serial.begin(115200);                    // 시리얼 통신 초기화
  pinMode(buttonPin, INPUT_PULLUP);       // 버튼 입력 핀 설정 (내부 풀업 사용)

  // SD 카드 초기화
  if (!SD.begin()) {
    Serial.println("SD Card Mount Failed");
    state = ERROR;
    return;
  }

  // Wi-Fi 연결 시도
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");                    // 연결 중 출력
  }

  // 연결 성공
  Serial.println("\nWiFi connected");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.println("System Ready - Press button to download files");
}

void loop() {
  static int lastButtonState = HIGH;
  int currentButtonState = digitalRead(buttonPin);

  // 버튼 상태 변화 감지 및 디바운싱
  if (currentButtonState != lastButtonState) {
    lastDebounceTime = millis();
  }

  // 디바운싱된 버튼 입력이 LOW이고 상태가 IDLE이면 다운로드 시작
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (currentButtonState == LOW && state == IDLE) {
      startDownloadProcess();
    }
  }
  lastButtonState = currentButtonState;

  // 상태에 따라 작업 분기
  switch (state) {
    case GETTING_LIST:
      getFileList();            // 서버에서 파일 목록 가져오기
      break;
    case DOWNLOADING:
      downloadCurrentFile();    // 파일 다운로드 수행
      break;
    case COMPLETE:
    case ERROR:
    case IDLE:
      // 아무 동작 없음
      break;
  }
}

// 다운로드 프로세스 초기화
void startDownloadProcess() {
  Serial.println("\nStarting download process...");
  state = GETTING_LIST;
  fileCount = 0;
  currentFileIndex = 0;
}

// 서버에서 파일 목록을 JSON 형식으로 가져옴
void getFileList() {
  HTTPClient http;
  String url = "http://" + String(serverIP) + "/list";  // /list 엔드포인트 요청
  http.begin(url);

  int httpCode = http.GET();     // GET 요청 전송

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();     // 응답 본문(JSON 문자열) 수신
    parseFileList(payload);                // JSON 파싱하여 fileList에 저장
    if (fileCount > 0) {
      Serial.printf("Found %d files to download\n", fileCount);
      state = DOWNLOADING;                 // 다음 상태로 전환
    } else {
      Serial.println("No files found on server");
      state = COMPLETE;
    }
  } else {
    Serial.printf("Failed to get file list, error: %s\n", http.errorToString(httpCode).c_str());
    state = ERROR;
  }

  http.end();
}

// JSON 문자열을 파싱하여 파일 이름과 크기를 배열에 저장
void parseFileList(String json) {
  DynamicJsonDocument doc(4096);       // 최대 4KB 크기의 JSON 문서
  deserializeJson(doc, json);          // JSON 디코드

  fileCount = doc.size();              // 항목 개수
  for (int i = 0; i < fileCount; i++) {
    fileList[i] = doc[i]["name"].as<String>();     // 파일 이름
    Serial.printf("%d. %s (%d bytes)\n", i + 1, fileList[i].c_str(), doc[i]["size"].as<int>());
  }
}

// 개별 파일 다운로드 및 SD 카드에 저장
void downloadCurrentFile() {
  if (currentFileIndex >= fileCount) {
    state = COMPLETE;
    Serial.println("All files downloaded successfully");
    return;
  }

  String fileName = fileList[currentFileIndex];
  Serial.printf("Downloading %d/%d: %s\n", currentFileIndex + 1, fileCount, fileName.c_str());

  HTTPClient http;
  String url = "http://" + String(serverIP) + "/download?file=" + fileName;
  http.begin(url);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String fullPath = "/" + fileName;

    // 기존 파일이 있으면 삭제
    if (SD.exists(fullPath)) {
      SD.remove(fullPath);
    }

    // 파일 쓰기 모드로 열기
    File file = SD.open(fullPath, FILE_WRITE);
    if (file) {
      WiFiClient* stream = http.getStreamPtr();  // 응답 스트림 포인터
      uint8_t buffer[1024];                      // 버퍼
      size_t bytesRead;
      size_t totalBytes = 0;

      // 스트림에서 데이터 읽어와 파일에 기록
      while (http.connected() && (bytesRead = stream->readBytes(buffer, sizeof(buffer))) > 0) {
        file.write(buffer, bytesRead);
        totalBytes += bytesRead;
        Serial.printf("\rDownloading: %d bytes", totalBytes);
      }

      file.close();   // 파일 닫기
      Serial.printf("\nFile saved: %s\n", fullPath.c_str());
      currentFileIndex++;         // 다음 파일로 이동
    } else {
      Serial.println("Failed to open file for writing");
      state = ERROR;
    }
  } else {
    Serial.printf("Failed to download file, error: %s\n", http.errorToString(httpCode).c_str());
    state = ERROR;
  }

  http.end();
}
