//-----------------------------------------------------------------------------
// 2024 Ahoy, https://github.com/lumpapu/ahoy
// Creative Commons - https://creativecommons.org/licenses/by-nc-sa/4.0/deed
//-----------------------------------------------------------------------------

#ifndef __HMS_RADIO_H__
#define __HMS_RADIO_H__

#include "cmt2300a.h"
#include "../hm/Radio.h"

//#define CMT_SWITCH_CHANNEL_CYCLE    5

template<uint32_t DTU_SN = 0x81001765>
class CmtRadio : public Radio {
    typedef Cmt2300a CmtType;
    public:
        void setup(bool *serialDebug, bool *privacyMode, bool *printWholeTrace, cfgCmt_t *cfg, uint8_t region = 0, bool genDtuSn = true) {
            mCfg = cfg;

            if(!cfg->enabled)
                return;

            mPrivacyMode = privacyMode;
            mSerialDebug = serialDebug;
            mPrintWholeTrace = printWholeTrace;
            mTxBuf.fill(0);

            mCmt.setup(cfg->pinSclk, cfg->pinSdio, cfg->pinCsb, cfg->pinFcsb);
            reset(genDtuSn, static_cast<RegionCfg>(region));
        }

        void loop() override {
            if(!mCfg->enabled)
                return;

            mCmt.loop();
            if((!mIrqRcvd) && (!mRqstGetRx))
                return;
            getRx();
            if(CmtStatus::SUCCESS == mCmt.goRx()) {
                mIrqRcvd   = false;
                mRqstGetRx = false;
            }
            return;
        }

        bool isChipConnected(void) const override {
            return mCmtAvail;
        }

        void sendControlPacket(Inverter<> *iv, uint8_t cmd, uint16_t *data, bool isRetransmit) override {
            if(!mCfg->enabled)
                return;

            DPRINT(DBG_INFO, F("sendControlPacket cmd: "));
            DBGHEXLN(cmd);
            initPacket(iv->radioId.u64, TX_REQ_DEVCONTROL, SINGLE_FRAME);
            uint8_t cnt = 10;

            mTxBuf[cnt++] = cmd; // cmd -> 0 on, 1 off, 2 restart, 11 active power, 12 reactive power, 13 power factor
            mTxBuf[cnt++] = 0x00;
            if(cmd >= ActivePowerContr && cmd <= PFSet) { // ActivePowerContr, ReactivePowerContr, PFSet
                mTxBuf[cnt++] = (data[0] >> 8) & 0xff; // power limit, multiplied by 10 (because of fraction)
                mTxBuf[cnt++] = (data[0]     ) & 0xff; // power limit
                mTxBuf[cnt++] = (data[1] >> 8) & 0xff; // setting for persistens handlings
                mTxBuf[cnt++] = (data[1]     ) & 0xff; // setting for persistens handling
            }

            sendPacket(iv, cnt, isRetransmit);
        }

        bool switchFrequency(Inverter<> *iv, uint32_t fromkHz, uint32_t tokHz) override {
            if(!isChipConnected())
                return false;

            uint8_t fromCh = mCmt.freq2Chan(fromkHz);
            uint8_t toCh = mCmt.freq2Chan(tokHz);

            return switchFrequencyCh(iv, fromCh, toCh);
        }

        bool switchFrequencyCh(Inverter<> *iv, uint8_t fromCh, uint8_t toCh) override {
            if((0xff == fromCh) || (0xff == toCh))
                return false;
            if(!isChipConnected())
                return false;

            mCmt.switchChannel(fromCh);
            sendSwitchChCmd(iv, toCh);
            mCmt.switchChannel(toCh);
            return true;
        }

        uint16_t getBaseFreqMhz(void) override {
            return mCmt.getBaseFreqMhz();
        }

        uint16_t getBootFreqMhz(void) override {
            return mCmt.getBootFreqMhz();
        }

        std::pair<uint16_t,uint16_t> getFreqRangeMhz(void) override {
            return mCmt.getFreqRangeMhz();
        }

    private:

        void sendPacket(Inverter<> *iv, uint8_t len, bool isRetransmit, bool appendCrc16=true) override {
            // inverters have maybe different settings regarding frequency
            if(mCmt.getCurrentChannel() != iv->config->frequency)
                mCmt.switchChannel(iv->config->frequency);

            updateCrcs(&len, appendCrc16);

            if(*mSerialDebug) {
                DPRINT_IVID(DBG_INFO, iv->id);
                DBGPRINT(F("TX "));
                DBGPRINT(String(mCmt.getFreqKhz()/1000.0f));
                DBGPRINT(F("Mhz | "));
                if(*mPrintWholeTrace) {
                    if(*mPrivacyMode)
                        ah::dumpBuf(mTxBuf.data(), len, 1, 4);
                    else
                        ah::dumpBuf(mTxBuf.data(), len);
                } else {
                    DHEX(mTxBuf[0]);
                    DBGPRINT(F(" "));
                    DHEX(mTxBuf[10]);
                    DBGPRINT(F(" "));
                    DBGHEXLN(mTxBuf[9]);
                }
            }

            CmtStatus status = mCmt.tx(mTxBuf.data(), len);
            mMillis = millis();
            if(CmtStatus::SUCCESS != status) {
                DPRINT(DBG_WARN, F("CMT TX failed, code: "));
                DBGPRINTLN(String(static_cast<uint8_t>(status)));
                if(CmtStatus::ERR_RX_IN_FIFO == status)
                    mIrqRcvd = true;
            }
            iv->mDtuTxCnt++;
        }

        uint64_t getIvId(Inverter<> *iv) const override {
            return iv->radioId.u64;
        }

        uint8_t getIvGen(Inverter<> *iv) const override {
            return iv->ivGen;
        }

        inline void reset(bool genDtuSn, RegionCfg region) {
            if(genDtuSn)
                generateDtuSn();
            if(!mCmt.reset(region)) {
                mCmtAvail = false;
                DPRINTLN(DBG_WARN, F("Initializing CMT2300A failed!"));
            } else {
                mCmtAvail = true;
                mCmt.goRx();
            }

            mIrqRcvd   = false;
            mRqstGetRx = false;
        }

        inline void sendSwitchChCmd(Inverter<> *iv, uint8_t ch) {
            //if(CMT_SWITCH_CHANNEL_CYCLE > ++mSwitchCycle)
            //    return;
            //mSwitchCycle = 0;

            /** ch:
             * 0x00: 860.00 MHz
             * 0x01: 860.25 MHz
             * 0x02: 860.50 MHz
             * ...
             * 0x14: 865.00 MHz
             * ...
             * 0x28: 870.00 MHz
             * */
            initPacket(iv->radioId.u64, 0x56, 0x02);
            mTxBuf[10] = 0x15;
            mTxBuf[11] = 0x21;
            mTxBuf[12] = ch;
            mTxBuf[13] = 0x14;
            sendPacket(iv, 14, false);
            mRqstGetRx = true;
        }

        inline void getRx(void) {
            packet_t p;
            p.millis = millis() - mMillis;
            if(CmtStatus::SUCCESS == mCmt.getRx(p.packet, &p.len, 28, &p.rssi)) {
                //mSwitchCycle = 0;
                p.ch = 0; // not used for CMT inverters
                mBufCtrl.push(p);
            }

            if(p.packet[9] > ALL_FRAMES) { // indicates last frame
                setExpectedFrames(p.packet[9] - ALL_FRAMES);
                mRadioWaitTime.startTimeMonitor(2); // let the inverter first get back to rx mode?
            }
        }

        CmtType mCmt;
        cfgCmt_t *mCfg = nullptr;
        bool mCmtAvail = false;
        bool mRqstGetRx = false;
        uint32_t mMillis = 0;
        //uint8_t mSwitchCycle = 0;
};

#endif /*__HMS_RADIO_H__*/
