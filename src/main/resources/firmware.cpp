#include <Arduino.h>
#include <stdint.h>
#include <august.h>

#include "ActionHandler.h"
#include "EEPROM.h"
#include <APIReturn.h>

#include "esp_wifi.h"

// If you have an Infura key and are using an Infura supported chain you may get better performance by adding your key here
// Note: you can get a free account, which will easily be sufficient for this application
//#define USING_INFURA 1
//#define INFURA_KEY   00000000000000000000000000000000

#include <Web3.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WifiServer.h>
#include <TcpBridge.h>
#include <functional>
#include <string>
#include <vector>

const char* ssid = "[SSID]";
const char* password = "[PASSWORD]";

#define DOOR_CONTRACT "[LOCK_CONTRACT]"

//This device private key is used for comms, doesn't have any value
#define DEVICE_PRIVATE_KEY "[DEVICE_KEY]"

#define DISCONNECT_TIMEOUT 1000*30 //30 Seconds BlueTooth scan disconnect timeout

enum APIRoutes
{
  api_unknown,
  api_getChallenge,
  api_checkSignature,
  api_checkSignatureLock,
  api_end
};

std::map<std::string, APIRoutes> s_apiRoutes;
ActionHandler* actionHandler;

void Initialize()
{
  s_apiRoutes["getChallenge"] = api_getChallenge;
  s_apiRoutes["checkSignature"] = api_checkSignature;
  s_apiRoutes["checkSignatureLock"] = api_checkSignatureLock;
  s_apiRoutes["end"] = api_end;
}

void generateSeed(BYTE* buffer);
void updateChallenge();
void setupWifi();
Web3* web3;
KeyID* keyID;

long blueToothTimer = 0;

//Replace these with any words you like to form cryptographic challenge
const char* seedWords[] = { "Apples", "Oranges", "Grapes", "DragonFruit", "BreadFruit", "Pomegranate", "Aubergine", "Fungi", "Falafel", "Cryptokitty", "Kookaburra", "Elvis", "Koala", "Cassowary", 0 };

string currentChallenge;

TcpBridge* tcpConnection;

const char* apiRoute = "api/";

[AUGUST_LOCK_CREDENTIALS]

bool isLocked;
void UnlockDoor(int unlockSeconds);
void ToggleDoor();
void doLockCommand();
void doUnlockCommand();

// These two callback decls required here for the case of mutiple instances of AugustLock.
// It would be required to switch them here based on client address. TODO: It may be possible to fold them into the AugustLock class
void notifyCB(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify)
{
  // If you have multiple locks you'll need to switch here using the service address: pRemoteCharacteristic->getRemoteService()->getClient()->getPeerAddress()
  augustLock._notifyCB(pRemoteCharacteristic, pData, length);
}

void secureLockCallback(NimBLERemoteCharacteristic* pRemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify)
{
  augustLock._secureLockCallback(pRemoteCharacteristic, pData, length);
}

bool usingBluetooth = false;

void connectCallback(const char* msg, bool status)
{
  Serial.print("Connect Callback: ");
  Serial.println(msg);
  isLocked = status;
}

void disConnectCallback()
{
  Serial.println("Disconnect complete");
  augustLock.blankClient();
  usingBluetooth = false;
  delay(1000);

  usingBluetooth = false;
}

bool QueryBalance(const char* contractAddr, std::string* userAddress)
{
  // transaction
  bool hasToken = false;
  Contract contract(web3, contractAddr);
  string func = "balanceOf(address)";
  string param = contract.SetupContractData(func.c_str(), userAddress);
  string result = contract.ViewCall(&param);

  Serial.println(result.c_str());

  // break down the result
  uint256_t baseBalance = web3->getUint256(&result);

  if (baseBalance > 0)
  {
    hasToken = true;
    Serial.println("Has token");
  }

  return hasToken;
}

std::string handleTCPAPI(APIReturn* apiReturn)
{
  Serial.print("ESP Free Heap: ");
  Serial.println(ESP.getFreeHeap());
  Serial.print("ESP Max alloc Heap: ");
  Serial.println(ESP.getMaxAllocHeap());

  Serial.println(apiReturn->apiName.c_str());
  string address;

  switch (s_apiRoutes[apiReturn->apiName])
  {
  case api_getChallenge:
    Serial.println(currentChallenge.c_str());
    return currentChallenge;
  case api_checkSignature:
  {
    Serial.print("Sig: ");
    Serial.println(apiReturn->params["sig"].c_str());
    address = Crypto::ECRecoverFromPersonalMessage(&apiReturn->params["sig"], &currentChallenge);
    int unlockSeconds = strtol(apiReturn->params["openTime"].c_str(), NULL, 10);
    Serial.print("EC-Addr: ");
    Serial.println(address.c_str());
    boolean hasToken = QueryBalance(DOOR_CONTRACT, &address);
    updateChallenge(); // generate a new challenge after each check
    if (hasToken)
    {
      actionHandler->AddCallback(500, &doUnlockCommand);
      return string("pass");
    }
    else
    {
      return string("fail");
    }
  }
  break;
  case api_checkSignatureLock:
  {
    Serial.print("Sig: ");
    Serial.println(apiReturn->params["sig"].c_str());
    address = Crypto::ECRecoverFromPersonalMessage(&apiReturn->params["sig"], &currentChallenge);
    int unlockSeconds = strtol(apiReturn->params["openTime"].c_str(), NULL, 10);
    Serial.print("EC-Addr: ");
    Serial.println(address.c_str());
    boolean hasToken = QueryBalance(DOOR_CONTRACT, &address);
    updateChallenge(); // generate a new challenge after each check
    if (hasToken)
    {
      actionHandler->AddCallback(500, &doLockCommand);
      return string("pass");
    }
    else
    {
      return string("fail");
    }
  }
  break;
  case api_unknown:
  case api_end:
    break;
  }

  return string("");
}

void setup()
{
  Serial.begin(115200);
  Initialize();
  augustLock.init();
  delay(100);
  setupWifi();
  actionHandler = new ActionHandler(6);
#ifdef USING_INFURA
  web3 = new Web3([CHAIN_ID], INFURA_KEY);
#else
  web3 = new Web3([CHAIN_ID]);
#endif
  keyID = new KeyID(web3, DEVICE_PRIVATE_KEY);
  updateChallenge();

  tcpConnection = new TcpBridge();
  tcpConnection->setKey(keyID, web3);
  tcpConnection->startConnection();
}

void loop()
{
  long currentMillis = millis();
  actionHandler->CheckEvents(currentMillis);
  if (!usingBluetooth)
  {
    setupWifi(); // ensure we maintain a connection. This may cause the server to reboot periodically, if it loses connection
    tcpConnection->checkClientAPI(&handleTCPAPI);
  }
  else
  {
    augustLock.checkStatus(); // required in loop() to handle lock comms, in case of connection failure
  }

  delay(100);
}

//if unlockSeconds specified, re-lock the door after that time
void UnlockDoor(int unlockSeconds)
{
  usingBluetooth = true;

  if (unlockSeconds > 0)
  {
    blueToothTimer = millis() + unlockSeconds * 1000;
  }

  // code action sequence to open the gate
  actionHandler->AddCallback(500, &doUnlockCommand);
}

void doLockCommand()
{
  usingBluetooth = true;
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);

  augustLock.connect(&connectCallback, &notifyCB, &secureLockCallback, &disConnectCallback);
  augustLock.lockAction(LOCK);

  actionHandler->AddCallback(DISCONNECT_TIMEOUT, &disConnectCallback);
}

void doUnlockCommand()
{
  usingBluetooth = true;
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);

  augustLock.connect(&connectCallback, &notifyCB, &secureLockCallback, &disConnectCallback);
  augustLock.lockAction(UNLOCK);

  actionHandler->AddCallback(DISCONNECT_TIMEOUT, &disConnectCallback);
}

void doToggleCommand()
{
  usingBluetooth = true;
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);

  augustLock.connect(&connectCallback, &notifyCB, &secureLockCallback, &disConnectCallback);
  augustLock.lockAction(LockAction::TOGGLE_LOCK);
}

void ToggleDoor()
{
  usingBluetooth = true;
  // code action sequence to toggle the door
  actionHandler->AddCallback(1000, &doToggleCommand);
}

void updateChallenge()
{
  // generate a new challenge
  int size = 0;
  while (seedWords[size] != 0)
    size++;
  Serial.println(size);
  char buffer[32];

  int seedIndex = random(0, size);
  currentChallenge = seedWords[seedIndex];
  currentChallenge += "-";
  long challengeVal = random32();
  currentChallenge += itoa(challengeVal, buffer, 16);

  Serial.print("Challenge: ");
  Serial.println(currentChallenge.c_str());
}

bool wifiConnect(const char* ssid, const char* password)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return true;
  }

  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  if (WiFi.status() != WL_CONNECTED)
  {
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    WiFi.begin(ssid, password);
  }

  int wificounter = 0;
  while (WiFi.status() != WL_CONNECTED && wificounter < 20)
  {
    for (int i = 0; i < 500; i++)
    {
      delay(1);
    }
    Serial.print(".");
    wificounter++;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.print("-");
    return false;
  }
  else
  {
    return true;
  }
}

void setupWifi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }

  delay(100);

  WiFi.enableSTA(true);
  WiFi.mode(WIFI_STA);

  wifiConnect(ssid, password);
  bool connected = wifiConnect(ssid, password);

  if (!connected)
  {
    Serial.println("Restarting ...");
    ESP.restart(); // targetting 8266 & Esp32 - you may need to replace this
  }

  esp_wifi_set_max_tx_power(78); // save a little power if your unit is near the router. If it's located away then use 78 - max
  delay(10);

  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}