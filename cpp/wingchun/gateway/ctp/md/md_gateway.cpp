//
// Created by qlu on 2019/1/10.
//

#include "md_gateway.h"
#include "assert.h"
#include <iostream>
#include <stdio.h>
#include <cstring>
#include "Timer.h"

#include "macro.h"
#include "util/include/code_convert.h"
#include "fmt/format.h"
#include "../type_convert.h"
#include "../serialize.h"

namespace kungfu
{
    namespace ctp
    {
        const std::unordered_map<int, std::string> MdGateway::kDisconnectedReasonMap{
                {0x1001, "网络读失败"},
                {0x1002, "网络写失败"},
                {0x2001, "接收心跳超时"},
                {0x2002, "发送心跳失败"},
                {0x2003, "收到错误报文"}
        };

        void MdGateway::start()
        {
            api_ = CThostFtdcMdApi::CreateFtdcMdApi();
            api_->RegisterSpi(this);
            api_->RegisterFront((char*)front_uri_.c_str());
            api_->Init();

            GatewayImpl::start();

            api_->Join();
        }

        bool MdGateway::login()
        {
            CThostFtdcReqUserLoginField login_field = {};
            strcpy(login_field.UserID, account_id_.c_str());
            strcpy(login_field.BrokerID, broker_id_.c_str());
            strcpy(login_field.Password, password_.c_str());

            int rtn = api_->ReqUserLogin(&login_field, ++request_id_);

            return rtn == 0;

        }

        bool MdGateway::subscribe(const std::vector<Instrument>& instruments, bool is_level2)
        {
            std::vector<std::string> insts;
            for (const auto& ins : instruments)
            {
                insts.push_back(ins.instrument_id);
            }
            return subscribe(insts);
        }

        bool MdGateway::unsubscribe(const std::vector<Instrument>& instruments)
        {
            std::vector<std::string> insts;
            for (const auto& ins : instruments)
            {
                insts.push_back(ins.instrument_id);
            }
            return unsubscribe(insts);
        }

        bool MdGateway::subscribe(const std::vector<std::string> &instrument_ids)
        {
            const int count = instrument_ids.size();
            char** insts = new char*[count];
            for (int i = 0; i < count; i ++)
            {
                insts[i] = (char*)instrument_ids[i].c_str();
            }
            int rtn = api_->SubscribeMarketData(insts, count);
            delete[] insts;
            return rtn == 0;
        }

        bool MdGateway::unsubscribe(const std::vector<std::string> &instrument_ids)
        {
            int count = instrument_ids.size();
            char** insts = new char*[count];
            for (int i = 0; i < count; i ++)
            {
                insts[i] = (char*)instrument_ids[i].c_str();
            }
            int rtn = api_->UnSubscribeMarketData(insts, count);
            delete[] insts;
            return rtn == 0;
        }

        void MdGateway::OnFrontConnected()
        {
            CONNECT_INFO();
            set_state(GatewayState::Connected);
            login();
        }

        void MdGateway::OnFrontDisconnected(int nReason)
        {
            auto it = kDisconnectedReasonMap.find(nReason);
            if (it == kDisconnectedReasonMap.end())
            {
                DISCONNECTED_ERROR(fmt::format("(nReason) {} (Info) {}", nReason, it->second));
                set_state(GatewayState::DisConnected, it->second);
            }
            else
            {
                DISCONNECTED_ERROR(fmt::format("(nReason) {}", nReason));
                set_state(GatewayState::DisConnected);
            }
        }

        void MdGateway::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin, CThostFtdcRspInfoField *pRspInfo,
                                       int nRequestID, bool bIsLast)
        {
            if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
            {
                std::string utf_msg = gbk2utf8(pRspInfo->ErrorMsg);
                LOGIN_ERROR(fmt::format("(ErrorId) {} (ErrorMsg) {}", pRspInfo->ErrorID, utf_msg));
                set_state(GatewayState::LoggedInFailed, utf_msg);
            }
            else
            {
                LOGIN_INFO(fmt::format("(BrokerID) {} (UserID) {} (TradingDay) {} ", pRspUserLogin->BrokerID, pRspUserLogin->UserID, pRspUserLogin->TradingDay));
                set_state(GatewayState::Ready);

                auto instruments = get_subscriptions();
                subscribe(instruments);
            }
        }

        void MdGateway::OnRspUserLogout(CThostFtdcUserLogoutField *pUserLogout, CThostFtdcRspInfoField *pRspInfo,
                                        int nRequestID, bool bIsLast)
        {

        }

        void MdGateway::OnRspSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument,
                                           CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
        {
            if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
            {
                SUBSCRIBE_ERROR(fmt::format("(error_id) {} (error_msg) {}" , pRspInfo->ErrorID, gbk2utf8(pRspInfo->ErrorMsg))) ;
            }
            else
            {
                SUBSCRIBE_INFO(fmt::format("(Inst) {} (bIsLast) {}", pSpecificInstrument->InstrumentID == nullptr ? "": pSpecificInstrument->InstrumentID, bIsLast));
            }
        }

        void MdGateway::OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField *pSpecificInstrument,
                                             CThostFtdcRspInfoField *pRspInfo, int nRequestID, bool bIsLast)
        {
            if (pRspInfo != nullptr && pRspInfo->ErrorID != 0)
            {
                UNSUBSCRIBE_ERROR(fmt::format("(error_id) {} (error_msg) {}" , pRspInfo->ErrorID, gbk2utf8(pRspInfo->ErrorMsg)));
            }
            else
            {
                UNSUBSCRIBE_INFO(fmt::format("(Inst) {} (bIsLast) {}", pSpecificInstrument->InstrumentID, bIsLast));
            }
        }

        void MdGateway::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField *pDepthMarketData)
        {
            if (pDepthMarketData != nullptr)
            {
                QUOTE_TRACE(to_string(*pDepthMarketData));

                Quote quote = {};
                from_ctp(*pDepthMarketData, quote);
                quote.rcv_time = kungfu::yijinjing::getNanoTime();
                on_quote(quote);
            }
        }
    }
}
