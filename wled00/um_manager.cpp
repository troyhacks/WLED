#include "wled.h"
#include "event_bus.h"  // WLEDMM v3: wled::Event, wled::EventBus
/*
 * Registration and management utility for v2 usermods
 */

//Usermod Manager internals
void UsermodManager::setup()             { for (unsigned i = 0; i < numMods; i++) ums[i]->setup(); }
void UsermodManager::connected()         { for (unsigned i = 0; i < numMods; i++) ums[i]->connected(); }
void UsermodManager::loop()              { for (unsigned i = 0; i < numMods; i++) ums[i]->loop();  }
void UsermodManager::loop2()             { for (unsigned i = 0; i < numMods; i++) ums[i]->loop2(); }
void UsermodManager::handleOverlayDraw() { for (unsigned i = 0; i < numMods; i++) ums[i]->handleOverlayDraw(); }
// void UsermodManager::appendConfigData()  { for (unsigned i = 0; i < numMods; i++) ums[i]->appendConfigData(); } //WLEDMM not used
bool UsermodManager::handleButton(uint8_t b) {
  bool overrideIO = false;
  for (unsigned i = 0; i < numMods; i++) {
    if (ums[i]->handleButton(b)) overrideIO = true;
  }
  return overrideIO;
}
bool UsermodManager::getUMData(um_data_t **data, uint8_t mod_id) {
  for (unsigned i = 0; i < numMods; i++) {
    if (mod_id > 0 && ums[i]->getId() != mod_id) continue;  // only get data form requested usermod if provided
    if (ums[i]->getUMData(data)) return true;               // if usermod does provide data return immediately (only one usermod can provide data at one time)
  }
  return false;
}
void UsermodManager::addToJsonState(JsonObject& obj)    { for (unsigned i = 0; i < numMods; i++) ums[i]->addToJsonState(obj); }
void UsermodManager::addToJsonInfo(JsonObject& obj)     { for (unsigned i = 0; i < numMods; i++) ums[i]->addToJsonInfo(obj); }
// WLEDMM: the /json/ar endpoint's fan-out - the machine-readable twin of the
// info page. Same shape as addToJsonInfo: every usermod adds its own object.
void UsermodManager::addToJsonAr(JsonObject& obj)       { for (unsigned i = 0; i < numMods; i++) ums[i]->addToJsonAr(obj); }
void UsermodManager::readFromJsonState(JsonObject& obj) { for (unsigned i = 0; i < numMods; i++) ums[i]->readFromJsonState(obj); }
void UsermodManager::addToConfig(JsonObject& obj)       { for (unsigned i = 0; i < numMods; i++) ums[i]->addToConfig(obj); }
bool UsermodManager::readFromConfig(JsonObject& obj)    {
  bool allComplete = true;
  for (unsigned i = 0; i < numMods; i++) {
    if (!ums[i]->readFromConfig(obj)) allComplete = false;
  }
  return allComplete;
}
void UsermodManager::onMqttConnect(bool sessionPresent) { for (unsigned i = 0; i < numMods; i++) ums[i]->onMqttConnect(sessionPresent); }
bool UsermodManager::onMqttMessage(char* topic, char* payload) {
  for (unsigned i = 0; i < numMods; i++) if (ums[i]->onMqttMessage(topic, payload)) return true;
  return false;
}
void UsermodManager::onUpdateBegin(bool init) { for (unsigned i = 0; i < numMods; i++) ums[i]->onUpdateBegin(init); } // notify usermods that update is to begin
void UsermodManager::onStateChange(uint8_t mode) { for (unsigned i = 0; i < numMods; i++) ums[i]->onStateChange(mode); } // notify usermods that WLED state changed
// WLEDMM v3: pre-state-change fan-out. Fires before stateUpdated()
// wipes currentPreset, so usermods can latch pre-wipe values.
void UsermodManager::onPreStateChange(uint8_t mode) { for (unsigned i = 0; i < numMods; i++) ums[i]->onPreStateChange(mode); }
// WLEDMM v3: event bus fan-out. Synchronous; runs in the publisher's
// task context. Handlers must be short.
void UsermodManager::onEvent(const wled::Event& ev) { for (unsigned i = 0; i < numMods; i++) ums[i]->onEvent(ev); }

/*
 * Enables usermods to lookup another Usermod.
 */
Usermod* UsermodManager::lookup(uint16_t mod_id) {
  for (unsigned i = 0; i < numMods; i++) {
    if (ums[i]->getId() == mod_id) {
      return ums[i];
    }
  }
  return nullptr;
}

//WLEDMM: used by Usermods in xml.cpp
Usermod* UsermodManager::lookupName(const char *mod_name) {
  //WLEDMM: hack to get the usermod object with the mod_name (better would be to store the usermod name in the class but that requires change to all usermods)
  for (unsigned i = 0; i < numMods; i++) {
    // StaticJsonDocument <1024> docx;
    JsonObject um = doc.createNestedObject("um"); //WLEDMM reuse the global doc variable here

    ums[i]->addToConfig(um);

    //check the name in the config of ums[i]
    for (JsonPair kv : um) {
      if (strcmp(kv.key().c_str(), mod_name) == 0) return ums[i]; //if same as mod_name, return this mod
    }
  }
  return nullptr;
}

bool UsermodManager::add(Usermod* um)
{
  if (numMods >= WLED_MAX_USERMODS || um == nullptr) return false;
  ums[numMods++] = um;
  return true;
}
