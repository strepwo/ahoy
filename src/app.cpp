//-----------------------------------------------------------------------------
// 2022 Ahoy, https://ahoydtu.de
// Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
//-----------------------------------------------------------------------------

#if defined(ESP32) && defined(F)
#undef F
#define F(sl) (sl)
#endif

#include "app.h"
#include <ArduinoJson.h>
#include "utils/sun.h"

//-----------------------------------------------------------------------------
app::app() : ah::Scheduler() {
    mWeb = NULL;
}


//-----------------------------------------------------------------------------
void app::setup(uint32_t timeout) {
    Serial.begin(115200);
    while (!Serial)
        yield();

    resetSystem();
    mSettings.setup();
    mSettings.getPtr(mConfig);
    DPRINTLN(DBG_INFO, F("Settings valid: ") + String((mSettings.getValid()) ? F("true") : F("false")));

    addListener(EVERY_SEC, std::bind(&app::tickSecond, this));
    addListener(EVERY_MIN, std::bind(&app::tickMinute, this));
    addListener(EVERY_12H, std::bind(&app::tickNtpUpdate, this));
    once(mConfig->nrf.sendInterval, std::bind(&app::tickSend, this), "tickSend");
    if((mConfig->sun.lat) && (mConfig->sun.lon)) {
        once(5, std::bind(&app::tickCalcSunrise, this));
        mCalculatedTimezoneOffset = (int8_t)((mConfig->sun.lon >= 0 ? mConfig->sun.lon + 7.5 : mConfig->sun.lon - 7.5) / 15) * 3600;
    }

    mSys = new HmSystemType();
    mSys->enableDebug();
    mSys->setup(mConfig->nrf.amplifierPower, mConfig->nrf.pinIrq, mConfig->nrf.pinCe, mConfig->nrf.pinCs);
    mSys->addInverters(&mConfig->inst);

    #if !defined(AP_ONLY)
    mMqtt.setup(&mConfig->mqtt, mConfig->sys.deviceName, mVersion, mSys, &mTimestamp, &mSunrise, &mSunset);
    #endif

    mWifi.setup(mConfig, &mTimestamp);

    mPayload.setup(mSys);
    mPayload.enableSerialDebug(mConfig->serial.debug);
    #if !defined(AP_ONLY)
    if (mConfig->mqtt.broker[0] > 0) {
        mPayload.addListener(std::bind(&PubMqttType::payloadEventListener, &mMqtt, std::placeholders::_1));
        addListener(EVERY_SEC, std::bind(&PubMqttType::tickerSecond, &mMqtt));
        addListener(EVERY_MIN, std::bind(&PubMqttType::tickerMinute, &mMqtt));
        mMqtt.setSubscriptionCb(std::bind(&app::mqttSubRxCb, this, std::placeholders::_1));

    }
    #endif
    setupLed();

    mWeb = new web(this, mConfig, &mStat, mVersion);
    mWeb->setup();
    mWeb->setProtection(strlen(mConfig->sys.adminPwd) != 0);
    addListener(EVERY_SEC, std::bind(&web::tickSecond, mWeb));

    // Plugins
    #if defined(ENA_NOKIA) || defined(ENA_SSD1306)
    mMonoDisplay.setup(mSys, &mTimestamp);
    mPayload.addListener(std::bind(&MonoDisplayType::payloadEventListener, &mMonoDisplay, std::placeholders::_1));
    addListener(EVERY_SEC, std::bind(&MonoDisplayType::tickerSecond, &mMonoDisplay));
    #endif

    //addListener(EVERY_MIN, std::bind(&PubSerialType::tickerMinute, &mPubSerial));
}

//-----------------------------------------------------------------------------
void app::loop(void) {
    DPRINTLN(DBG_VERBOSE, F("app::loop"));

    ah::Scheduler::loop();

    #if !defined(AP_ONLY)
    mWifi.loop();
    #endif

    mWeb->loop();

    if (mFlagSendDiscoveryConfig) {
        mFlagSendDiscoveryConfig = false;
        mMqtt.sendMqttDiscoveryConfig(mConfig->mqtt.topic);
    }

    mSys->Radio.loop();

    yield();

    if (ah::checkTicker(&mRxTicker, 5)) {
        bool rxRdy = mSys->Radio.switchRxCh();

        if (!mSys->BufCtrl.empty()) {
            uint8_t len;
            packet_t *p = mSys->BufCtrl.getBack();

            if (mSys->Radio.checkPaketCrc(p->packet, &len, p->rxCh)) {
                if (mConfig->serial.debug) {
                    DPRINT(DBG_INFO, "RX " + String(len) + "B Ch" + String(p->rxCh) + " | ");
                    mSys->Radio.dumpBuf(NULL, p->packet, len);
                }
                mStat.frmCnt++;

                if (0 != len)
                    mPayload.add(p, len);
            }
            mSys->BufCtrl.popBack();
        }
        yield();

        if (rxRdy)
            mPayload.process(true, mConfig->nrf.maxRetransPerPyld, &mStat);
    }

    mMqtt.loop();
}

//-----------------------------------------------------------------------------
void app::tickCalcSunrise(void) {
    if (0 == mTimestamp) {
        once(5, std::bind(&app::tickCalcSunrise, this)); // check again in 5 secs
        return;
    }
    ah::calculateSunriseSunset(mTimestamp, mCalculatedTimezoneOffset, mConfig->sun.lat, mConfig->sun.lon, &mSunrise, &mSunset);

    uint32_t nxtTrig = mTimestamp - (mTimestamp % 86400) + 86400; // next midnight
    onceAt(nxtTrig, std::bind(&app::tickCalcSunrise, this), "calc sunrise");
    onceAt(mSunrise, std::bind(&app::tickSend, this), "tickSend"); // register next event
    if (mConfig->mqtt.broker[0] > 0) {
        once(1, std::bind(&PubMqttType::tickerSun, &mMqtt), "MQTT-tickerSun");
        onceAt(mSunset, std::bind(&PubMqttType::tickSunset, &mMqtt));
    }
}

//-----------------------------------------------------------------------------
void app::tickSend(void) {
    if ((mTimestamp > 0) && (!mConfig->sun.disNightCom || (mTimestamp >= mSunrise && mTimestamp <= mSunset))) {  // Timestamp is set and (inverter communication only during the day if the option is activated and sunrise/sunset is set)
        once(mConfig->nrf.sendInterval, std::bind(&app::tickSend, this), "tickSend"); // register next event
        if (mConfig->serial.debug)
            DPRINTLN(DBG_DEBUG, F("Free heap: 0x") + String(ESP.getFreeHeap(), HEX));

        if (!mSys->BufCtrl.empty()) {
            if (mConfig->serial.debug)
                DPRINTLN(DBG_DEBUG, F("recbuf not empty! #") + String(mSys->BufCtrl.getFill()));
        }

        int8_t maxLoop = MAX_NUM_INVERTERS;
        Inverter<> *iv = mSys->getInverterByPos(mSendLastIvId);
        do {
            mSendLastIvId = ((MAX_NUM_INVERTERS - 1) == mSendLastIvId) ? 0 : mSendLastIvId + 1;
            iv = mSys->getInverterByPos(mSendLastIvId);
        } while ((NULL == iv) && ((maxLoop--) > 0));

        if (NULL != iv) {
            if (!mPayload.isComplete(iv))
                mPayload.process(false, mConfig->nrf.maxRetransPerPyld, &mStat);

            if (!mPayload.isComplete(iv)) {
                if (0 == mPayload.getMaxPacketId(iv))
                    mStat.rxFailNoAnser++;
                else
                    mStat.rxFail++;

                iv->setQueuedCmdFinished();  // command failed
                if (mConfig->serial.debug)
                    DPRINTLN(DBG_INFO, F("enqueued cmd failed/timeout"));
                if (mConfig->serial.debug) {
                    DPRINT(DBG_INFO, F("(#") + String(iv->id) + ") ");
                    DPRINTLN(DBG_INFO, F("no Payload received! (retransmits: ") + String(mPayload.getRetransmits(iv)) + ")");
                }
            }

            mPayload.reset(iv, mTimestamp);
            mPayload.request(iv);

            yield();
            if (mConfig->serial.debug) {
                DPRINTLN(DBG_DEBUG, F("app:loop WiFi WiFi.status ") + String(WiFi.status()));
                DPRINTLN(DBG_INFO, F("(#") + String(iv->id) + F(") Requesting Inv SN ") + String(iv->config->serial.u64, HEX));
            }

            if (iv->devControlRequest) {
                if (mConfig->serial.debug)
                    DPRINTLN(DBG_INFO, F("(#") + String(iv->id) + F(") Devcontrol request ") + String(iv->devControlCmd) + F(" power limit ") + String(iv->powerLimit[0]));
                mSys->Radio.sendControlPacket(iv->radioId.u64, iv->devControlCmd, iv->powerLimit);
                mPayload.setTxCmd(iv, iv->devControlCmd);
                iv->clearCmdQueue();
                iv->enqueCommand<InfoCommand>(SystemConfigPara);
            } else {
                uint8_t cmd = iv->getQueuedCmd();
                DPRINTLN(DBG_INFO, F("(#") + String(iv->id) + F(") sendTimePacket"));
                mSys->Radio.sendTimePacket(iv->radioId.u64, cmd, mPayload.getTs(iv), iv->alarmMesIndex);
                mPayload.setTxCmd(iv, cmd);
                mRxTicker = 0;
            }
        }
    } else {
        once(3600, std::bind(&app::tickSend, this), "tickSend"); // register next event (one hour)
        if (mConfig->serial.debug)
            DPRINTLN(DBG_WARN, F("Time not set or it is night time, therefore no communication to the inverter!"));
    }
    yield();

    updateLed();
}

//-----------------------------------------------------------------------------
void app::handleIntr(void) {
    DPRINTLN(DBG_VERBOSE, F("app::handleIntr"));
    mSys->Radio.handleIntr();
}

//-----------------------------------------------------------------------------
void app::scanAvailNetworks(void) {
    mWifi.scanAvailNetworks();
}

//-----------------------------------------------------------------------------
void app::getAvailNetworks(JsonObject obj) {
    mWifi.getAvailNetworks(obj);
}


//-----------------------------------------------------------------------------
void app::resetSystem(void) {
    snprintf(mVersion, 12, "%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);

    mShouldReboot = false;
    mUpdateNtp = false;
    mFlagSendDiscoveryConfig = false;

#ifdef AP_ONLY
    mTimestamp = 1;
#else
    mTimestamp = 0;
#endif

    mSunrise = 0;
    mSunset  = 0;

    mRxTicker = 0;

    mSendLastIvId = 0;

    mShowRebootRequest = false;

    memset(&mStat, 0, sizeof(statistics_t));
}

//-----------------------------------------------------------------------------
void app::mqttSubRxCb(JsonObject obj) {
    if(NULL != mWeb)
        mWeb->apiCtrlRequest(obj);
}

//-----------------------------------------------------------------------------
void app::setupLed(void) {
    /** LED connection diagram
     *          \\
     * PIN ---- |<----- 3.3V
     *
     * */
    if(mConfig->led.led0 != 0xff) {
        pinMode(mConfig->led.led0, OUTPUT);
        digitalWrite(mConfig->led.led0, HIGH); // LED off
    }
    if(mConfig->led.led1 != 0xff) {
        pinMode(mConfig->led.led1, OUTPUT);
        digitalWrite(mConfig->led.led1, HIGH); // LED off
    }
}

//-----------------------------------------------------------------------------
void app::updateLed(void) {
    if(mConfig->led.led0 != 0xff) {
        Inverter<> *iv = mSys->getInverterByPos(0);
        if (NULL != iv) {
            record_t<> *rec = iv->getRecordStruct(RealTimeRunData_Debug);
            if(iv->isProducing(mTimestamp, rec))
                digitalWrite(mConfig->led.led0, LOW); // LED on
            else
                digitalWrite(mConfig->led.led0, HIGH); // LED off
        }
    }
}