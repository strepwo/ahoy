//-----------------------------------------------------------------------------
// 2024 Ahoy, https://ahoydtu.de
// Creative Commons - https://creativecommons.org/licenses/by-nc-sa/4.0/deed
//-----------------------------------------------------------------------------

#include <ArduinoJson.h>
#include "app.h"
#include "utils/sun.h"

#if !defined(ESP32)
    void esp_task_wdt_reset() {}
#endif


//-----------------------------------------------------------------------------
app::app() : ah::Scheduler {} {
    memset(mVersion, 0, sizeof(char) * 12);
    memset(mVersionModules, 0, sizeof(char) * 12);
}


//-----------------------------------------------------------------------------
void app::setup() {
    Serial.begin(115200);
    while (!Serial)
        yield();

    #if defined(ESP32)
        esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
        esp_task_wdt_add(NULL);
    #endif

    resetSystem();
    esp_task_wdt_reset();

    mSettings.setup(mConfig);
    ah::Scheduler::setup(mConfig->inst.startWithoutTime);
    DPRINT(DBG_INFO, F("Settings valid: "));
    DBGPRINTLN(mConfig->valid ? F("true") : F("false"));

    esp_task_wdt_reset();

    mNrfRadio.setup(&mConfig->serial.debug, &mConfig->serial.privacyLog, &mConfig->serial.printWholeTrace, &mConfig->nrf);
    #if defined(ESP32)
    mCmtRadio.setup(&mConfig->serial.debug, &mConfig->serial.privacyLog, &mConfig->serial.printWholeTrace, &mConfig->cmt, mConfig->sys.region);
    #endif

    #ifdef ETHERNET
        delay(1000);
        mNetwork = static_cast<AhoyNetwork*>(new AhoyEthernet());
    #else
        mNetwork = static_cast<AhoyNetwork*>(new AhoyWifi());
    #endif
    mNetwork->setup(mConfig, &mTimestamp, [this](bool gotIp) { this->onNetwork(gotIp); }, [this](bool gotTime) { this->onNtpUpdate(gotTime); });
    mNetwork->begin();

    esp_task_wdt_reset();

    mCommunication.setup(&mTimestamp, &mConfig->serial.debug, &mConfig->serial.privacyLog, &mConfig->serial.printWholeTrace);
    mCommunication.addPayloadListener([this] (uint8_t cmd, Inverter<> *iv) { payloadEventListener(cmd, iv); });
    #if defined(ENABLE_MQTT)
        mCommunication.addPowerLimitAckListener([this] (Inverter<> *iv) { mMqtt.setPowerLimitAck(iv); });
    #endif
    mSys.setup(&mTimestamp, &mConfig->inst, this);
    for (uint8_t i = 0; i < MAX_NUM_INVERTERS; i++) {
        initInverter(i);
    }

    if(mConfig->nrf.enabled) {
        if (!mNrfRadio.isChipConnected())
            DPRINTLN(DBG_WARN, F("WARNING! your NRF24 module can't be reached, check the wiring"));
    }

    esp_task_wdt_reset();

    // when WiFi is in client mode, then enable mqtt broker
    #if defined(ENABLE_MQTT)
    mMqttEnabled = (mConfig->mqtt.broker[0] > 0);
    if (mMqttEnabled) {
        mMqtt.setup(this, &mConfig->mqtt, mConfig->sys.deviceName, mVersion, &mSys, &mTimestamp, &mUptime);
        mMqtt.setSubscriptionCb(std::bind(&app::mqttSubRxCb, this, std::placeholders::_1));
        mCommunication.addAlarmListener([this](Inverter<> *iv) { mMqtt.alarmEvent(iv); });
    }
    #endif
    setupLed();

    esp_task_wdt_reset();

    mWeb.setup(this, &mSys, mConfig);
    mApi.setup(this, &mSys, mWeb.getWebSrvPtr(), mConfig);
    mProtection = Protection::getInstance(mConfig->sys.adminPwd);

    #ifdef ENABLE_SYSLOG
    mDbgSyslog.setup(mConfig); // be sure to init after mWeb.setup (webSerial uses also debug callback)
    #endif
    // Plugins
    #if defined(PLUGIN_DISPLAY)
    if (DISP_TYPE_T0_NONE != mConfig->plugin.display.type)
        #if defined(ESP32)
        mDisplay.setup(this, &mConfig->plugin.display, &mSys, &mNrfRadio, &mCmtRadio, &mTimestamp);
        #else
        mDisplay.setup(this, &mConfig->plugin.display, &mSys, &mNrfRadio, NULL, &mTimestamp);
        #endif
    #endif

    esp_task_wdt_reset();

    #if defined(ENABLE_HISTORY)
    mHistory.setup(this, &mSys, mConfig, &mTimestamp);
    #endif /*ENABLE_HISTORY*/

    mPubSerial.setup(mConfig, &mSys, &mTimestamp);

    #if !defined(ETHERNET)
    //mImprov.setup(this, mConfig->sys.deviceName, mVersion);
    #endif

    #if defined(ENABLE_SIMULATOR)
    mSimulator.setup(&mSys, &mTimestamp, 0);
    mSimulator.addPayloadListener([this](uint8_t cmd, Inverter<> *iv) { payloadEventListener(cmd, iv); });
    #endif /*ENABLE_SIMULATOR*/

    esp_task_wdt_reset();
    regularTickers();
}

//-----------------------------------------------------------------------------
void app::loop(void) {
    esp_task_wdt_reset();

    mNrfRadio.loop();

    #if defined(ESP32)
    mCmtRadio.loop();
    #endif

    ah::Scheduler::loop();
    mCommunication.loop();

    #if defined(ENABLE_MQTT)
    if (mMqttEnabled && mNetworkConnected)
        mMqtt.loop();
    #endif
    yield();
}

//-----------------------------------------------------------------------------
void app::onNetwork(bool gotIp) {
    mNetworkConnected = gotIp;
    ah::Scheduler::resetTicker();
    regularTickers(); //reinstall regular tickers
    every(std::bind(&app::tickSend, this), mConfig->inst.sendInterval, "tSend");
    mTickerInstallOnce = true;
    mSunrise = 0;  // needs to be set to 0, to reinstall sunrise and ivComm tickers!
    once(std::bind(&app::tickNtpUpdate, this), 2, "ntp2");
}

//-----------------------------------------------------------------------------
void app::regularTickers(void) {
    DPRINTLN(DBG_DEBUG, F("regularTickers"));
    everySec(std::bind(&WebType::tickSecond, &mWeb), "webSc");
    everySec([this]() { mProtection->tickSecond(); }, "prot");
    everySec([this]() {mNetwork->tickNetworkLoop(); }, "net");

    // Plugins
    #if defined(PLUGIN_DISPLAY)
    if (DISP_TYPE_T0_NONE != mConfig->plugin.display.type)
        everySec(std::bind(&DisplayType::tickerSecond, &mDisplay), "disp");
    #endif
    every(std::bind(&PubSerialType::tick, &mPubSerial), 5, "uart");
    #if !defined(ETHERNET)
    //everySec([this]() { mImprov.tickSerial(); }, "impro");
    #endif

    #if defined(ENABLE_HISTORY)
    everySec(std::bind(&HistoryType::tickerSecond, &mHistory), "hist");
    #endif /*ENABLE_HISTORY*/

    #if defined(ENABLE_SIMULATOR)
    every(std::bind(&SimulatorType::tick, &mSimulator), 5, "sim");
    #endif /*ENABLE_SIMULATOR*/
}

//-----------------------------------------------------------------------------
void app::onNtpUpdate(bool gotTime) {
    mNtpReceived = true;
    if ((0 == mSunrise) && (0.0 != mConfig->sun.lat) && (0.0 != mConfig->sun.lon)) {
        mCalculatedTimezoneOffset = (int8_t)((mConfig->sun.lon >= 0 ? mConfig->sun.lon + 7.5 : mConfig->sun.lon - 7.5) / 15) * 3600;
        tickCalcSunrise();
    }

    if (mTickerInstallOnce) {
        mTickerInstallOnce = false;
        #if defined(ENABLE_MQTT)
        if (mMqttEnabled) {
            mMqtt.tickerSecond();
            everySec(std::bind(&PubMqttType::tickerSecond, &mMqtt), "mqttS");
            everyMin(std::bind(&PubMqttType::tickerMinute, &mMqtt), "mqttM");
        }
        #endif /*ENABLE_MQTT*/

        if (mConfig->inst.rstValsNotAvail)
            everyMin(std::bind(&app::tickMinute, this), "tMin");

        if(mNtpReceived) {
            uint32_t localTime = gTimezone.toLocal(mTimestamp);
            uint32_t midTrig = gTimezone.toUTC(localTime - (localTime % 86400) + 86400);  // next midnight local time
            onceAt(std::bind(&app::tickMidnight, this), midTrig, "midNi");

            if (mConfig->sys.schedReboot) {
                uint32_t rebootTrig = gTimezone.toUTC(localTime - (localTime % 86400) + 86410);  // reboot 10 secs after midnght
                onceAt(std::bind(&app::tickReboot, this), rebootTrig, "midRe");
            }
        }
    }
}

//-----------------------------------------------------------------------------
void app::updateNtp(void) {
    if(mNtpReceived)
        onNtpUpdate(true);
}

//-----------------------------------------------------------------------------
void app::tickNtpUpdate(void) {
    uint32_t nxtTrig = 5;  // default: check again in 5 sec

    if (!mNtpReceived)
        mNetwork->updateNtpTime();
    else {
        nxtTrig = mConfig->ntp.interval * 60;  // check again in configured interval
        mNtpReceived = false;
    }

    updateNtp();

    once(std::bind(&app::tickNtpUpdate, this), nxtTrig, "ntp");
}

//-----------------------------------------------------------------------------
void app::tickCalcSunrise(void) {
    if (mSunrise == 0)                                          // on boot/reboot calc sun values for current time
        ah::calculateSunriseSunset(mTimestamp, mCalculatedTimezoneOffset, mConfig->sun.lat, mConfig->sun.lon, &mSunrise, &mSunset);

    if (mTimestamp > (mSunset + mConfig->sun.offsetSecEvening))        // current time is past communication stop, calc sun values for next day
        ah::calculateSunriseSunset(mTimestamp + 86400, mCalculatedTimezoneOffset, mConfig->sun.lat, mConfig->sun.lon, &mSunrise, &mSunset);

    tickIVCommunication();

    uint32_t nxtTrig = mSunset + mConfig->sun.offsetSecEvening + 60;    // set next trigger to communication stop, +60 for safety that it is certain past communication stop
    onceAt(std::bind(&app::tickCalcSunrise, this), nxtTrig, "Sunri");
    if (mMqttEnabled) {
        tickSun();
        nxtTrig = mSunrise + mConfig->sun.offsetSecMorning + 1;         // one second safety to trigger correctly
        onceAt(std::bind(&app::tickSunrise, this), nxtTrig, "mqSr");    // trigger on sunrise to update 'dis_night_comm'
    }
}

//-----------------------------------------------------------------------------
void app::tickIVCommunication(void) {
    bool restartTick = false;
    bool zeroValues = false;
    uint32_t nxtTrig = 0;

    for(uint8_t i = 0; i < MAX_NUM_INVERTERS; i ++) {
        Inverter<> *iv = mSys.getInverterByPos(i);
        if(NULL == iv)
            continue;

        iv->commEnabled = !iv->config->disNightCom; // if sun.disNightCom is false, communication is always on
        if (!iv->commEnabled) {  // inverter communication only during the day
            if (mTimestamp < (mSunrise + mConfig->sun.offsetSecMorning)) { // current time is before communication start, set next trigger to communication start
                nxtTrig = mSunrise + mConfig->sun.offsetSecMorning;
            } else {
                if (mTimestamp >= (mSunset + mConfig->sun.offsetSecEvening)) { // current time is past communication stop, nothing to do. Next update will be done at midnight by tickCalcSunrise
                    nxtTrig = 0;
                } else { // current time lies within communication start/stop time, set next trigger to communication stop
                    iv->commEnabled = true;
                    nxtTrig = mSunset + mConfig->sun.offsetSecEvening;
                }
            }
            if (nxtTrig != 0)
                restartTick = true;
        }

        if ((!iv->commEnabled) && (mConfig->inst.rstValsCommStop))
            zeroValues = true;
    }

    if(restartTick) // at least one inverter
        onceAt(std::bind(&app::tickIVCommunication, this), nxtTrig, "ivCom");

    if (zeroValues) // at least one inverter
        once(std::bind(&app::tickZeroValues, this), mConfig->inst.sendInterval, "tZero");
}

//-----------------------------------------------------------------------------
void app::tickSun(void) {
    // only used and enabled by MQTT (see setup())
    #if defined(ENABLE_MQTT)
    if (!mMqtt.tickerSun(mSunrise, mSunset, mConfig->sun.offsetSecMorning, mConfig->sun.offsetSecEvening))
        once(std::bind(&app::tickSun, this), 1, "mqSun");  // MQTT not connected, retry
    #endif
}

//-----------------------------------------------------------------------------
void app::tickSunrise(void) {
    // only used and enabled by MQTT (see setup())
    #if defined(ENABLE_MQTT)
    if (!mMqtt.tickerSun(mSunrise, mSunset, mConfig->sun.offsetSecMorning, mConfig->sun.offsetSecEvening, true))
        once(std::bind(&app::tickSun, this), 1, "mqSun");  // MQTT not connected, retry
    #endif
}

//-----------------------------------------------------------------------------
void app::notAvailChanged(void) {
    #if defined(ENABLE_MQTT)
    if (mMqttEnabled)
        mMqtt.notAvailChanged(mAllIvNotAvail);
    #endif
}

//-----------------------------------------------------------------------------
void app::tickZeroValues(void) {
    zeroIvValues(!CHECK_AVAIL, SKIP_YIELD_DAY);
}

//-----------------------------------------------------------------------------
void app::tickMinute(void) {
    // only triggered if 'reset values on no avail is enabled'

    zeroIvValues(CHECK_AVAIL, SKIP_YIELD_DAY);
}

//-----------------------------------------------------------------------------
void app::tickMidnight(void) {
    uint32_t localTime = gTimezone.toLocal(mTimestamp);
    uint32_t nxtTrig = gTimezone.toUTC(localTime - (localTime % 86400) + 86400);  // next midnight local time
    onceAt(std::bind(&app::tickMidnight, this), nxtTrig, "mid2");

    Inverter<> *iv;
    for (uint8_t id = 0; id < mSys.getNumInverters(); id++) {
        iv = mSys.getInverterByPos(id);
        if (NULL == iv)
            continue;  // skip to next inverter

        // reset alarms
        if(InverterStatus::OFF == iv->getStatus())
            iv->resetAlarms();

        // clear max values
        if(mConfig->inst.rstMaxValsMidNight) {
            record_t<> *rec = iv->getRecordStruct(RealTimeRunData_Debug);
            for(uint8_t i = 0; i <= iv->channels; i++) {
                uint8_t pos = iv->getPosByChFld(i, FLD_MP, rec);
                iv->setValue(pos, rec, 0.0f);
            }
        }

        iv->historyMidnight(mTimestamp);
    }

    if (mConfig->inst.rstYieldMidNight) {
        zeroIvValues(!CHECK_AVAIL, !SKIP_YIELD_DAY);

        #if defined(ENABLE_MQTT)
        if (mMqttEnabled)
            mMqtt.tickerMidnight();
        #endif
    }
}

//-----------------------------------------------------------------------------
void app::tickSend(void) {
    bool notAvail = true;
    uint8_t fill = mCommunication.getFillState();
    uint8_t max = mCommunication.getMaxFill();
    if((max-MAX_NUM_INVERTERS) <= fill) {
        DPRINT(DBG_WARN, F("send queue almost full, consider to increase interval, "));
        DBGPRINT(String(fill));
        DBGPRINT(F(" of "));
        DBGPRINT(String(max));
        DBGPRINTLN(F(" entries used"));
    }

    for (uint8_t i = 0; i < MAX_NUM_INVERTERS; i++) {
        Inverter<> *iv = mSys.getInverterByPos(i);
        if(!sendIv(iv))
            notAvail = false;
    }

    if(mAllIvNotAvail != notAvail)
        once(std::bind(&app::notAvailChanged, this), 1, "avail");
    mAllIvNotAvail = notAvail;

    updateLed();
}

//-----------------------------------------------------------------------------
bool app::sendIv(Inverter<> *iv) {
    if(NULL == iv)
        return true;

    if(!iv->config->enabled)
        return true;

    if(!iv->commEnabled) {
        DPRINT_IVID(DBG_INFO, iv->id);
        DBGPRINTLN(F("no communication to the inverter (night time)"));
        return true;
    }

    if(!iv->radio->isChipConnected())
        return true;

    bool notAvail = true;
    if(InverterStatus::OFF != iv->status)
        notAvail = false;

    iv->tickSend([this, iv](uint8_t cmd, bool isDevControl) {
        if(isDevControl)
            mCommunication.addImportant(iv, cmd);
        else
            mCommunication.add(iv, cmd);
    });

    return notAvail;
}

//-----------------------------------------------------------------------------
void app:: zeroIvValues(bool checkAvail, bool skipYieldDay) {
    Inverter<> *iv;
    bool changed = false;
    // set values to zero, except yields
    for (uint8_t id = 0; id < mSys.getNumInverters(); id++) {
        iv = mSys.getInverterByPos(id);
        if (NULL == iv)
            continue;  // skip to next inverter
        if (!iv->config->enabled)
            continue;  // skip to next inverter

        if (checkAvail) {
            if (iv->isAvailable())
                continue;
        }

        changed = true;
        record_t<> *rec = iv->getRecordStruct(RealTimeRunData_Debug);
        for(uint8_t ch = 0; ch <= iv->channels; ch++) {
            uint8_t pos = 0;
            for(uint8_t fld = 0; fld < FLD_EVT; fld++) {
                switch(fld) {
                    case FLD_YD:
                        if(skipYieldDay)
                            continue;
                        else
                            break;
                    case FLD_YT:
                        continue;
                }
                pos = iv->getPosByChFld(ch, fld, rec);
                iv->setValue(pos, rec, 0.0f);
            }
            // zero max power
            if(!skipYieldDay) {
                pos = iv->getPosByChFld(ch, FLD_MP, rec);
                iv->setValue(pos, rec, 0.0f);
            }
            iv->resetAlarms();
            iv->doCalculations();
        }
    }

    if(changed)
        payloadEventListener(RealTimeRunData_Debug, NULL);
}

//-----------------------------------------------------------------------------
void app::resetSystem(void) {
    snprintf(mVersion, sizeof(mVersion), "%d.%d.%d", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
    snprintf(mVersionModules, sizeof(mVersionModules), "%s",
    #ifdef ENABLE_PROMETHEUS_EP
        "P"
    #endif

    #ifdef ENABLE_MQTT
        "M"
    #endif

    #ifdef PLUGIN_DISPLAY
        "D"
    #endif

    #ifdef ENABLE_HISTORY
        "H"
    #endif

    #ifdef AP_ONLY
        "A"
    #endif

    #ifdef ENABLE_SYSLOG
        "Y"
    #endif

    #ifdef ENABLE_SIMULATOR
        "S"
    #endif

        "-"
    #ifdef LANG_DE
        "de"
    #else
        "en"
    #endif
    );

#ifdef AP_ONLY
    mTimestamp = 1;
#endif

    mAllIvNotAvail = true;

    mSunrise = 0;
    mSunset  = 0;

    mMqttEnabled = false;

    mSendLastIvId = 0;
    mShowRebootRequest = false;
    mSavePending = false;
    mSaveReboot = false;

    mNetworkConnected = false;
    mNtpReceived = false;
    mTickerInstallOnce = false;
}

//-----------------------------------------------------------------------------
void app::mqttSubRxCb(JsonObject obj) {
    mApi.ctrlRequest(obj);
}

//-----------------------------------------------------------------------------
void app::setupLed(void) {
    uint8_t led_off = (mConfig->led.high_active) ? 0 : 255;
    for(uint8_t i = 0; i < 3; i ++) {
        if (mConfig->led.led[i] != DEF_PIN_OFF) {
            pinMode(mConfig->led.led[i], OUTPUT);
            analogWrite(mConfig->led.led[i], led_off);
        }
    }
}

//-----------------------------------------------------------------------------
void app::updateLed(void) {
    uint8_t led_off = (mConfig->led.high_active) ? 0 : 255;
    uint8_t led_on = (mConfig->led.high_active) ? (mConfig->led.luminance) : (255-mConfig->led.luminance);

    if (mConfig->led.led[0] != DEF_PIN_OFF) {
        for (uint8_t id = 0; id < mSys.getNumInverters(); id++) {
            Inverter<> *iv = mSys.getInverterByPos(id);
            if (NULL != iv) {
                if (iv->isProducing()) {
                    // turn on when at least one inverter is producing
                    analogWrite(mConfig->led.led[0], led_on);
                    break;
                }
                else if(iv->config->enabled)
                    analogWrite(mConfig->led.led[0], led_off);
            }
        }
    }

    if (mConfig->led.led[1] != DEF_PIN_OFF) {
        if (getMqttIsConnected()) {
            analogWrite(mConfig->led.led[1], led_on);
        } else {
            analogWrite(mConfig->led.led[1], led_off);
        }
    }

    if (mConfig->led.led[2] != DEF_PIN_OFF) {
        if((mTimestamp > (mSunset + mConfig->sun.offsetSecEvening)) || (mTimestamp < (mSunrise + mConfig->sun.offsetSecMorning)))
            analogWrite(mConfig->led.led[2], led_on);
        else
            analogWrite(mConfig->led.led[2], led_off);
    }
}
