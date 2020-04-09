/*!
 * \file WtEngine.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtEngine.h"
#include "WtDataManager.h"
#include "WtHelper.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostFile.hpp"
#include "../Share/JsonToVariant.hpp"
#include "../Share/DecimalHelper.h"
#include "../Share/CodeHelper.hpp"

#include "../Share/IBaseDataMgr.h"
#include "../Share/IHotMgr.h"

#include "../Share/WTSVariant.hpp"
#include "../Share/WTSContractInfo.hpp"
#include "../Share/WTSSessionInfo.hpp"

#include "../Share/WTSDataDef.hpp"
#include "../Share/WTSRiskDef.hpp"

#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

#ifdef _WIN32
#define my_stricmp _stricmp
#else
#define my_stricmp strcasecmp
#endif


USING_NS_OTP;

WtEngine::WtEngine()
	: _filter_timestamp(0)
	, _port_fund(NULL)
	, _risk_volscale(1.0)
	, _risk_date(0)
	, _terminated(false)
{
	TimeUtils::getDateTime(_cur_date, _cur_time);
	_cur_secs = _cur_time % 100000;
	_cur_time /= 100000;
	_cur_raw_time = _cur_time;

	WtHelper::setTime(_cur_date, _cur_time, _cur_secs);
}

void WtEngine::set_date_time(uint32_t curDate, uint32_t curTime, uint32_t curSecs /* = 0 */, uint32_t rawTime /* = 0 */)
{
	_cur_date = curDate;
	_cur_time = curTime;
	_cur_secs = curSecs;

	if (rawTime == 0)
		rawTime = curTime;

	_cur_raw_time = rawTime;

	WtHelper::setTime(_cur_date, _cur_raw_time, _cur_secs);
}

void WtEngine::set_trading_date(uint32_t curTDate)
{
	_cur_tdate = curTDate; 

	WtHelper::setTDate(curTDate);
}

WTSCommodityInfo* WtEngine::get_commodity_info(const char* stdCode)
{
	return _base_data_mgr->getCommodity(CodeHelper::stdCodeToStdCommID(stdCode).c_str());
}

WTSContractInfo* WtEngine::get_contract_info(const char* stdCode)
{
	std::string exchg, code, commID;
	bool isHot = false;
	CodeHelper::extractStdCode(stdCode, exchg, code, commID, isHot);
	return _base_data_mgr->getContract(code.c_str(), exchg.c_str());
}

WTSSessionInfo* WtEngine::get_session_info(const char* sid, bool isCode /* = false */)
{
	if (!isCode)
		return _base_data_mgr->getSession(sid);

	WTSCommodityInfo* cInfo = _base_data_mgr->getCommodity(CodeHelper::stdCodeToStdCommID(sid).c_str());
	if (cInfo == NULL)
		return NULL;

	return _base_data_mgr->getSession(cInfo->getSession());
}

void WtEngine::on_tick(const char* stdCode, WTSTickData* curTick)
{
	_price_map[stdCode] = curTick->price();

	//�ȼ���Ƿ�Ҫ�ź�Ҫ����
	{
		auto it = _sig_map.find(stdCode);
		if (it != _sig_map.end())
		{
			WTSSessionInfo* sInfo = get_session_info(stdCode, true);

			if (sInfo->isInTradingTime(_cur_raw_time, true))
			{
				const SigInfo& sInfo = it->second;
				int32_t pos = sInfo._volumn;
				std::string code = stdCode;
				push_task([this, code, pos](){
					do_set_position(code.c_str(), pos);
				});
				_sig_map.erase(it);
			}

		}
	}

	if (curTick->volumn() == 0)
		return;

	std::string code = stdCode;
	double price = curTick->price();
	push_task([this, code, price]{
		auto it = _pos_map.find(code);
		if (it == _pos_map.end())
			return;

		PosInfo& pInfo = it->second;
		if (pInfo._volumn == 0)
		{
			pInfo._dynprofit = 0;
		}
		else
		{
			WTSCommodityInfo* commInfo = get_commodity_info(code.c_str());
			double dynprofit = 0;
			for (auto pit = pInfo._details.begin(); pit != pInfo._details.end(); pit++)
			{
				DetailInfo& dInfo = *pit;
				dInfo._profit = dInfo._volumn*(price - dInfo._price)*commInfo->getVolScale()*(dInfo._long ? 1 : -1);
				dynprofit += dInfo._profit;
			}

			pInfo._dynprofit = dynprofit;
		}
	});
}

void WtEngine::update_fund_dynprofit()
{
	WTSFundStruct& fundInfo = _port_fund->fundInfo();
	if (fundInfo._last_date == _cur_tdate)
	{
		//�ϴν������ڵ��ڵ�ǰ�����գ�˵���Ѿ����㣬���ٸ�����
		return;
	}

	double profit = 0.0;
	for(auto v : _pos_map)
	{
		const PosInfo& pItem = v.second;
		profit += pItem._dynprofit;
	}

	fundInfo._dynprofit = profit;
	double dynbal = fundInfo._balance + profit;
	if (fundInfo._max_dyn_bal == DBL_MAX || DecimalHelper::gt(dynbal, fundInfo._max_dyn_bal))
	{
		fundInfo._max_dyn_bal = dynbal;
		fundInfo._max_time = _cur_raw_time * 100000 + _cur_secs;
	}

	if (fundInfo._min_dyn_bal == DBL_MAX || DecimalHelper::lt(dynbal, fundInfo._min_dyn_bal))
	{
		fundInfo._min_dyn_bal = dynbal;
		fundInfo._min_time = _cur_raw_time * 100000 + _cur_secs;;
	}

	double dynbalance = fundInfo._balance + profit;
	if (fundInfo._max_md_dyn_bal._date == 0 || DecimalHelper::gt(dynbalance, fundInfo._max_md_dyn_bal._dyn_balance))
	{
		fundInfo._max_md_dyn_bal._dyn_balance = dynbalance;
		fundInfo._max_md_dyn_bal._date = _cur_tdate;
	}

	if (fundInfo._min_md_dyn_bal._date == 0 || DecimalHelper::lt(dynbalance, fundInfo._min_md_dyn_bal._dyn_balance))
	{
		fundInfo._min_md_dyn_bal._dyn_balance = dynbalance;
		fundInfo._min_md_dyn_bal._date = _cur_tdate;
	}
}

void otp::WtEngine::writeRiskLog(const char* fmt, ...)
{
	char szBuf[2048] = { 0 };
	uint32_t length = sprintf(szBuf, "[�ʽ���]");
	strcat(szBuf, fmt);
	va_list args;
	va_start(args, fmt);
	WTSLogger::log2_direct("risk", LL_INFO, szBuf, args);
	va_end(args);
}

uint32_t WtEngine::getCurDate()
{
	return _cur_date;
}

uint32_t WtEngine::getCurTime()
{
	return _cur_time;
}

uint32_t WtEngine::getTradingDate()
{
	return _cur_tdate;
}

bool otp::WtEngine::isInTrading()
{
	return false;
}

void otp::WtEngine::setVolScale(double scale)
{
	double oldScale = _risk_volscale;
	_risk_volscale = scale;
	_risk_date = _cur_tdate;

	WTSLogger::info2("risk", "��ز�λϵ���Ѹı�: %.1f - > %.1f", oldScale, scale);

	save_datas();
}

WTSPortFundInfo* otp::WtEngine::getFundInfo()
{
	update_fund_dynprofit();
	save_datas();

	return _port_fund;
}

void WtEngine::init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDataManager* dataMgr, IHotMgr* hotMgr)
{
	_base_data_mgr = bdMgr;
	_data_mgr = dataMgr;
	_hot_mgr = hotMgr;

	WTSLogger::info("ƽ̨����ģʽ: ����ģʽ");

	_filter_file = cfg->getCString("filters");
	load_filters();

	load_fees(cfg->getCString("fees"));

	load_datas();

	WTSVariant* cfgRisk = cfg->get("riskmon");
	if(cfgRisk)
	{
		init_riskmon(cfgRisk);
	}
}

void WtEngine::on_session_end()
{
	//�ʽ����
	WTSFundStruct& fundInfo = _port_fund->fundInfo();
	if (fundInfo._last_date < _cur_tdate)
	{
		std::string filename = WtHelper::getPortifolioDir();
		filename += "funds.csv";
		BoostFilePtr fund_log(new BoostFile());
		{
			bool isNewFile = !BoostFile::exists(filename.c_str());
			fund_log->create_or_open_file(filename.c_str());
			if (isNewFile)
			{
				fund_log->write_file("date,predynbalance,prebalance,balance,closeprofit,dynprofit,fee,maxdynbalance,maxtime,mindynbalance,mintime,mdmaxbalance,mdmaxdate,mdminbalance,mdmindate\n");
			}
			else
			{
				fund_log->seek_to_end();
			}
		}

		//�������ﻹ��Ҫдһ���ʽ��¼
		//date,predynbalance,prebalance,balance,closeprofit,dynprofit,fee,maxdynbalance,maxtime,mindynbalance,mintime,mdmaxbalance,mdmaxdate,mdminbalance,mdmindate
		fund_log->write_file(StrUtil::printf("%u,%f,%f,%f,%f,%f,%f,%f,%u,%f,%u,%f,%u,%f,%u\n", 
			_cur_tdate, fundInfo._predynbal, fundInfo._prebalance, fundInfo._balance, 
			fundInfo._profit, fundInfo._dynprofit, fundInfo._fees, fundInfo._max_dyn_bal,
			fundInfo._max_time, fundInfo._min_dyn_bal, fundInfo._min_time,
			fundInfo._max_md_dyn_bal._dyn_balance, fundInfo._max_md_dyn_bal._date,
			fundInfo._min_md_dyn_bal._dyn_balance, fundInfo._min_md_dyn_bal._date));

		fundInfo._last_date = _cur_tdate;
		fundInfo._predynbal = fundInfo._balance + fundInfo._dynprofit;
		fundInfo._prebalance = fundInfo._balance;
		fundInfo._profit = 0;
		fundInfo._fees = 0;
		fundInfo._max_dyn_bal = DBL_MAX;
		fundInfo._min_dyn_bal = DBL_MAX;
		fundInfo._max_time = 0;
		fundInfo._min_time = 0;
	}
	save_datas();
}

void WtEngine::on_session_begin()
{

}

void WtEngine::save_datas()
{
	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();

	if (_port_fund != NULL)
	{//�����ʽ�����
		const WTSFundStruct& fundInfo = _port_fund->fundInfo();
		rj::Value jFund(rj::kObjectType);
		jFund.AddMember("predynbal", fundInfo._predynbal, allocator);
		jFund.AddMember("balance", fundInfo._balance, allocator);
		jFund.AddMember("prebalance", fundInfo._prebalance, allocator);
		jFund.AddMember("profit", fundInfo._profit, allocator);
		jFund.AddMember("dynprofit", fundInfo._dynprofit, allocator);
		jFund.AddMember("fees", fundInfo._fees, allocator);

		jFund.AddMember("max_dyn_bal", fundInfo._max_dyn_bal, allocator);
		jFund.AddMember("max_time", fundInfo._max_time, allocator);
		jFund.AddMember("min_dyn_bal", fundInfo._min_dyn_bal, allocator);
		jFund.AddMember("min_time", fundInfo._min_time, allocator);

		jFund.AddMember("last_date", fundInfo._last_date, allocator);
		jFund.AddMember("date", _cur_tdate, allocator);

		rj::Value jMaxMD(rj::kObjectType);
		jMaxMD.AddMember("date", fundInfo._max_md_dyn_bal._date, allocator);
		jMaxMD.AddMember("dyn_balance", fundInfo._max_md_dyn_bal._dyn_balance, allocator);

		rj::Value jMinMD(rj::kObjectType);
		jMinMD.AddMember("date", fundInfo._min_md_dyn_bal._date, allocator);
		jMinMD.AddMember("dyn_balance", fundInfo._min_md_dyn_bal._dyn_balance, allocator);

		jFund.AddMember("maxmd", jMaxMD, allocator);
		jFund.AddMember("minmd", jMinMD, allocator);

		root.AddMember("fund", jFund, allocator);
	}

	{//�ֲ����ݱ���
		rj::Value jPos(rj::kArrayType);

		for (auto it = _pos_map.begin(); it != _pos_map.end(); it++)
		{
			const char* stdCode = it->first.c_str();
			const PosInfo& pInfo = it->second;

			rj::Value pItem(rj::kObjectType);
			pItem.AddMember("code", rj::Value(stdCode, allocator), allocator);
			pItem.AddMember("volumn", pInfo._volumn, allocator);
			pItem.AddMember("closeprofit", pInfo._closeprofit, allocator);
			pItem.AddMember("dynprofit", pInfo._dynprofit, allocator);

			rj::Value details(rj::kArrayType);
			for (auto dit = pInfo._details.begin(); dit != pInfo._details.end(); dit++)
			{
				const DetailInfo& dInfo = *dit;
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("long", dInfo._long, allocator);
				dItem.AddMember("price", dInfo._price, allocator);
				dItem.AddMember("volumn", dInfo._volumn, allocator);
				dItem.AddMember("opentime", dInfo._opentime, allocator);
				dItem.AddMember("opentdate", dInfo._opentdate, allocator);

				dItem.AddMember("profit", dInfo._profit, allocator);

				details.PushBack(dItem, allocator);
			}

			pItem.AddMember("details", details, allocator);

			jPos.PushBack(pItem, allocator);
		}

		root.AddMember("positions", jPos, allocator);
	}

	//��ز�������
	{
		rj::Value jRisk(rj::kObjectType);

		jRisk.AddMember("scale", _risk_volscale, allocator);
		jRisk.AddMember("date", _risk_date, allocator);

		root.AddMember("riskmon", jRisk, allocator);
	}

	{
		std::string filename = WtHelper::getPortifolioDir();
		filename += "datas.json";

		BoostFile bf;
		if (bf.create_new_file(filename.c_str()))
		{
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);
			bf.write_file(sb.GetString());
			bf.close_file();
		}
	}
}

void WtEngine::load_datas()
{
	_port_fund = WTSPortFundInfo::create();

	std::string filename = WtHelper::getPortifolioDir();
	filename += "datas.json";

	if (!StdFile::exists(filename.c_str()))
	{
		return;
	}

	std::string content;
	StdFile::read_file_content(filename.c_str(), content);
	if (content.empty())
		return;

	rj::Document root;
	root.Parse(content.c_str());

	if (root.HasParseError())
		return;

	//��ȡ�ʽ�
	{
		const rj::Value& jFund = root["fund"];
		if (!jFund.IsNull() && jFund.IsObject())
		{
			WTSFundStruct& fundInfo = _port_fund->fundInfo();
			fundInfo._predynbal = jFund["predynbal"].GetDouble();
			fundInfo._balance = jFund["balance"].GetDouble();
			fundInfo._prebalance = jFund["prebalance"].GetDouble();
			fundInfo._profit = jFund["profit"].GetDouble();
			fundInfo._dynprofit = jFund["dynprofit"].GetDouble();
			fundInfo._fees = jFund["fees"].GetDouble();
			fundInfo._last_date = jFund["last_date"].GetUint();
			fundInfo._max_dyn_bal = jFund["max_dyn_bal"].GetDouble();
			fundInfo._max_time = jFund["max_time"].GetUint();
			fundInfo._min_dyn_bal = jFund["min_dyn_bal"].GetDouble();
			fundInfo._min_time = jFund["min_time"].GetUint();

			const rj::Value& jMaxMD = jFund["maxmd"];
			fundInfo._max_md_dyn_bal._dyn_balance = jMaxMD["dyn_balance"].GetDouble();
			fundInfo._max_md_dyn_bal._date = jMaxMD["date"].GetUint();

			const rj::Value& jMinMD = jFund["minmd"];
			fundInfo._min_md_dyn_bal._dyn_balance = jMinMD["dyn_balance"].GetDouble();
			fundInfo._min_md_dyn_bal._date = jMinMD["date"].GetUint();
		}
	}

	{//��ȡ��λ
		double total_profit = 0;
		double total_dynprofit = 0;
		const rj::Value& jPos = root["positions"];
		if (!jPos.IsNull() && jPos.IsArray())
		{
			for (const rj::Value& pItem : jPos.GetArray())
			{
				const char* stdCode = pItem["code"].GetString();
				PosInfo& pInfo = _pos_map[stdCode];
				pInfo._closeprofit = pItem["closeprofit"].GetDouble();
				pInfo._volumn = pItem["volumn"].GetInt();
				if (pInfo._volumn == 0)
					pInfo._dynprofit = 0;
				else
					pInfo._dynprofit = pItem["dynprofit"].GetDouble();

				total_profit += pInfo._closeprofit;
				total_dynprofit += pInfo._dynprofit;

				const rj::Value& details = pItem["details"];
				if (details.IsNull() || !details.IsArray() || details.Size() == 0)
					continue;

				pInfo._details.resize(details.Size());

				for (uint32_t i = 0; i < details.Size(); i++)
				{
					const rj::Value& dItem = details[i];
					DetailInfo& dInfo = pInfo._details[i];
					dInfo._long = dItem["long"].GetBool();
					dInfo._price = dItem["price"].GetDouble();
					dInfo._volumn = dItem["volumn"].GetInt();
					dInfo._opentime = dItem["opentime"].GetUint64();
					if (dItem.HasMember("opentdate"))
						dInfo._opentdate = dItem["opentdate"].GetUint();

					dInfo._profit = dItem["profit"].GetDouble();

				}
			}
		}

		WTSFundStruct& fundInfo = _port_fund->fundInfo();
		fundInfo._dynprofit = total_dynprofit;
	}

	if(root.HasMember("riskmon"))
	{
		//��ȡ��ز���
		const rj::Value& jRisk = root["riskmon"];
		if (!jRisk.IsNull() && jRisk.IsObject())
		{
			_risk_date = jRisk["date"].GetUint();
			_risk_volscale = jRisk["scale"].GetDouble();
		}
	}
}

WTSHisTickData* WtEngine::get_ticks(uint32_t sid, const char* stdCode, uint32_t count)
{
	return _data_mgr->get_ticks(stdCode, count);
}

WTSTickSlice* WtEngine::get_tick_slice(uint32_t sid, const char* code, uint32_t count)
{
	return _data_mgr->get_tick_slice(code, count);
}

WTSTickData* WtEngine::get_last_tick(uint32_t sid, const char* stdCode)
{
	return _data_mgr->grab_last_tick(stdCode);
}

WTSKlineSlice* WtEngine::get_kline_slice(uint32_t sid, const char* stdCode, const char* period, uint32_t count, uint32_t times /* = 1 */)
{
	WTSCommodityInfo* cInfo = _base_data_mgr->getCommodity(CodeHelper::stdCodeToStdCommID(stdCode).c_str());
	if (cInfo == NULL)
		return NULL;

	WTSSessionInfo* sInfo = _base_data_mgr->getSession(cInfo->getSession());
	if (sInfo == NULL)
		return NULL;

	std::string key = StrUtil::printf("%s-%s-%u", stdCode, period, times);
	SIDSet& sids = _bar_sub_map[key];
	sids.insert(sid);

	WTSKlinePeriod kp;
	if (strcmp(period, "m") == 0)
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			times /= 5;
		}
		else
			kp = KP_Minute1;
	}
	else
	{
		kp = KP_DAY;
	}

	return _data_mgr->get_kline_slice(stdCode, kp, times, count);
}


void WtEngine::handle_push_quote(WTSTickData* curTick, bool isHot)
{
	std::string stdCode = curTick->code();
	_data_mgr->handle_push_quote(stdCode.c_str(), curTick);
	on_tick(stdCode.c_str(), curTick);

	double price = curTick->price();

	if(isHot)
	{
		std::string hotCode = CodeHelper::stdCodeToStdHotCode(stdCode.c_str());
		WTSTickData* hotTick = WTSTickData::create(curTick->getTickStruct());
		hotTick->setCode(hotCode.c_str());
		
		_data_mgr->handle_push_quote(hotCode.c_str(), hotTick);
		on_tick(hotCode.c_str(), hotTick);

		hotTick->release();
	}
}

double WtEngine::get_cur_price(const char* stdCode)
{
	auto it = _price_map.find(stdCode);
	if(it == _price_map.end())
	{
		WTSTickData* lastTick = _data_mgr->grab_last_tick(stdCode);
		if (lastTick == NULL)
			return 0.0;

		double ret = lastTick->price();
		lastTick->release();
		_price_map[stdCode] = ret;
		return ret;
	}
	else
	{
		return it->second;
	}
}

void WtEngine::sub_tick(uint32_t sid, const char* stdCode)
{
	//�����������Լ����, ��SHFE.ag.HOT, ��ôҪת����ԭ��Լ����, SHFE.ag.1912
	//��Ϊִ����ֻʶ��ԭ��Լ����
	if (CodeHelper::isStdFutHotCode(stdCode))
	{
		SIDSet& sids = _tick_sub_map[stdCode];
		sids.insert(sid);

		CodeHelper::CodeInfo cInfo;
		CodeHelper::extractStdFutCode(stdCode, cInfo);
		std::string rawCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, _cur_tdate);
		std::string stdRawCode = CodeHelper::bscFutCodeToStdCode(rawCode.c_str(), cInfo._exchg);
		_subed_raw_codes.insert(stdRawCode);
	}
	else
	{
		std::size_t length = strlen(stdCode);
		if (stdCode[length - 1] == 'Q')
			length--;

		SIDSet& sids = _tick_sub_map[std::string(stdCode, length)];
		sids.insert(sid);

		_subed_raw_codes.insert(std::string(stdCode, length));
	}
}


void WtEngine::load_filters()
{
	if (_filter_file.empty())
		return;

	if (!StdFile::exists(_filter_file.c_str()))
	{
		WTSLogger::error("�����������ļ�%s������", _filter_file.c_str());
		return;
	}

	uint64_t lastModTime = boost::filesystem::last_write_time(boost::filesystem::path(_filter_file));
	if (lastModTime <= _filter_timestamp)
		return;

	WTSLogger::info("�����������ļ�%s���޸�, ��Ҫ���¼���", _filter_file.c_str());

	std::string content;
	StdFile::read_file_content(_filter_file.c_str(), content);
	if (content.empty())
	{
		WTSLogger::error("�����������ļ�%sΪ��", _filter_file.c_str());
		return;
	}

	rj::Document root;
	root.Parse(content.c_str());

	if (root.HasParseError())
	{
		WTSLogger::error("�����������ļ�%s����ʧ��", _filter_file.c_str());
		return;
	}

	WTSVariant* cfg = WTSVariant::createObject();
	if(!jsonToVariant(root, cfg))
	{
		WTSLogger::error("�����������ļ�%sת��ʧ��", _filter_file.c_str());
		return;
	}

	_filter_timestamp = lastModTime;

	_stra_filters.clear();
	_code_filters.clear();

	//�����Թ�����
	WTSVariant* filterStra = cfg->get("strategy_filters");
	if(filterStra)
	{
		auto keys = filterStra->memberNames();
		for(const std::string& key : keys)
		{
			WTSVariant* cfgItem = filterStra->get(key.c_str());
			const char* action = cfgItem->getCString("action");
			FilterAction fAct = FA_None;
			if (my_stricmp(action, "ignore") == 0)
				fAct = FA_Ignore;
			else if (my_stricmp(action, "redirect") == 0)
				fAct = FA_Redirect;				

			if(fAct == FA_None)
			{
				WTSLogger::error("���Թ�����%s����%s����ʶ��", key.c_str(), action);
				continue;
			}

			FilterItem& fItem = _stra_filters[key];
			fItem._key = key;
			fItem._action = fAct;
			fItem._target = cfgItem->getInt32("target");

			WTSLogger::info("���Թ�����%s�Ѽ���", key.c_str());
		}
	}

	//�����������
	WTSVariant* filterCode = cfg->get("code_filters");
	if (filterCode)
	{
		auto keys = filterCode->memberNames();
		for (const std::string& key : keys)
		{
			WTSVariant* cfgItem = filterCode->get(key.c_str());
			const char* action = cfgItem->getCString("action");
			FilterAction fAct = FA_None;
			if (my_stricmp(action, "ignore") == 0)
				fAct = FA_Ignore;
			else if (my_stricmp(action, "redirect") == 0)
				fAct = FA_Redirect;

			if (fAct == FA_None)
			{
				WTSLogger::error("���������%s����%s����ʶ��", key.c_str(), action);
				continue;
			}

			FilterItem& fItem = _code_filters[key];
			fItem._key = key;
			fItem._action = fAct;
			fItem._target = cfgItem->getInt32("target");

			WTSLogger::info("���������%s�Ѽ���", key.c_str());
		}
	}

	cfg->release();
}

const char* FLTACT_NAMEs[] = 
{
	"����",
	"�ض���"
};

bool WtEngine::is_filtered(const char* sname, const char* stdCode, int32_t& targetPos)
{
	auto it = _stra_filters.find(sname);
	if(it != _stra_filters.end())
	{
		const FilterItem& fItem = it->second;
		WTSLogger::info("[������] ���Թ�����%s����, ���˲���: %s", sname, fItem._action <= FA_Redirect ? FLTACT_NAMEs[fItem._action] : "δ֪");
		if (fItem._action == FA_Ignore)
		{
			return true;
		}
		else if(fItem._action == FA_Redirect)
		{
			targetPos = fItem._target;
		}

		return false;
	}

	it = _code_filters.find(stdCode);
	if (it != _code_filters.end())
	{
		const FilterItem& fItem = it->second;
		WTSLogger::info("[������] ��Լ������%s����, ���˲���: %s", stdCode, fItem._action <= FA_Redirect ? FLTACT_NAMEs[fItem._action] : "δ֪");
		if (fItem._action == FA_Ignore)
		{
			return true;
		}
		else if (fItem._action == FA_Redirect)
		{
			targetPos = fItem._target;
		}

		return false;
	}

	std::string stdPID = CodeHelper::stdCodeToStdCommID(stdCode);
	it = _code_filters.find(stdPID);
	if (it != _code_filters.end())
	{
		const FilterItem& fItem = it->second;
		WTSLogger::info("[������] Ʒ�ֹ�����%s����, ���˲���: %s", stdPID.c_str(), fItem._action <= FA_Redirect ? FLTACT_NAMEs[fItem._action] : "δ֪");
		if (fItem._action == FA_Ignore)
		{
			return true;
		}
		else if (fItem._action == FA_Redirect)
		{
			targetPos = fItem._target;
		}

		return false;
	}

	return false;
}

void WtEngine::load_fees(const char* filename)
{
	if (strlen(filename) == 0)
		return;

	if (!StdFile::exists(filename))
	{
		WTSLogger::error("������ģ���ļ�%s������", filename);
		return;
	}


	std::string content;
	StdFile::read_file_content(filename, content);
	if (content.empty())
	{
		WTSLogger::error("������ģ���ļ�%sΪ��", filename);
		return;
	}

	rj::Document root;
	root.Parse(content.c_str());

	if (root.HasParseError())
	{
		WTSLogger::error("������ģ���ļ�%s����ʧ��", filename);
		return;
	}

	WTSVariant* cfg = WTSVariant::createObject();
	if (!jsonToVariant(root, cfg))
	{
		WTSLogger::error("������ģ���ļ�%sת��ʧ��", filename);
		return;
	}

	auto keys = cfg->memberNames();
	for (const std::string& key : keys)
	{
		WTSVariant* cfgItem = cfg->get(key.c_str());
		FeeItem& fItem = _fee_map[key];
		fItem._by_volumn = cfgItem->getBoolean("byvolumn");
		fItem._open = cfgItem->getDouble("open");
		fItem._close = cfgItem->getDouble("close");
		fItem._close_today = cfgItem->getDouble("closetoday");
	}

	cfg->release();

	WTSLogger::info("������%u��������ģ��", _fee_map.size());
}

double WtEngine::calc_fee(const char* stdCode, double price, int32_t qty, uint32_t offset)
{
	std::string stdPID = CodeHelper::stdCodeToStdCommID(stdCode);
	auto it = _fee_map.find(stdPID);
	if (it == _fee_map.end())
		return 0.0;

	double ret = 0.0;
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(stdPID.c_str());
	const FeeItem& fItem = it->second;
	if(fItem._by_volumn)
	{
		switch (offset)
		{
		case 0: ret = fItem._open*qty; break;
		case 1: ret = fItem._close*qty; break;
		case 2: ret = fItem._close_today*qty; break;
		default: ret = 0.0; break;
		}
	}
	else
	{
		double amount = price*qty*commInfo->getVolScale();
		switch (offset)
		{
		case 0: ret = fItem._open*amount; break;
		case 1: ret = fItem._close*amount; break;
		case 2: ret = fItem._close_today*amount; break;
		default: ret = 0.0; break;
		}
	}

	return (int32_t)(ret * 100 + 0.5) / 100.0;
}

void WtEngine::append_signal(const char* stdCode, int32_t qty)
{
	double curPx = get_cur_price(stdCode);
	if(DecimalHelper::equal(curPx, 0.0))
	{
		SigInfo& sInfo = _sig_map[stdCode];
		sInfo._volumn = qty;
		sInfo._gentime = (uint64_t)_cur_date * 1000000000 + (uint64_t)_cur_raw_time * 100000 + _cur_secs;
	}
	else
	{
		do_set_position(stdCode, qty);
	}
}

void WtEngine::do_set_position(const char* stdCode, int32_t qty)
{
	PosInfo& pInfo = _pos_map[stdCode];
	double curPx = get_cur_price(stdCode);
	uint64_t curTm = (uint64_t)_cur_date * 10000 + _cur_time;
	uint32_t curTDate = _cur_tdate;

	if (pInfo._volumn == qty)
		return;

	int32_t diff = qty - pInfo._volumn;

	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(CodeHelper::stdCodeToStdCommID(stdCode).c_str());

	WTSFundStruct& fundInfo = _port_fund->fundInfo();

	if (pInfo._volumn*diff > 0)//��ǰ�ֲֺ�Ŀ���λ����һ��, ����һ����ϸ, ������������
	{
		pInfo._volumn = qty;

		DetailInfo dInfo;
		dInfo._long = qty > 0;
		dInfo._price = curPx;
		dInfo._volumn = abs(diff);
		dInfo._opentime = curTm;
		dInfo._opentdate = curTDate;
		pInfo._details.push_back(dInfo);

		double fee = calc_fee(stdCode, curPx, abs(qty), 0);
		fundInfo._fees += fee;
		fundInfo._balance -= fee;
	}
	else
	{//�ֲַ����Ŀ���λ����һ��, ��Ҫƽ��
		int32_t left = abs(diff);

		pInfo._volumn = qty;
		if (pInfo._volumn == 0)
			pInfo._dynprofit = 0;
		uint32_t count = 0;
		for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
		{
			DetailInfo& dInfo = *it;
			int32_t maxQty = min(dInfo._volumn, left);
			if (maxQty == 0)
				continue;

			dInfo._volumn -= maxQty;
			left -= maxQty;

			if (dInfo._volumn == 0)
				count++;

			double profit = (curPx - dInfo._price) * maxQty * commInfo->getVolScale();
			if (!dInfo._long)
				profit *= -1;
			pInfo._closeprofit += profit;
			pInfo._dynprofit = pInfo._dynprofit*dInfo._volumn / (dInfo._volumn + maxQty);//��ӯҲҪ���ȱ�����
			fundInfo._profit += profit;
			fundInfo._balance += profit;

			double fee = calc_fee(stdCode, curPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			fundInfo._fees += fee;
			fundInfo._balance -= fee;

			if (left == 0)
				break;
		}

		//��Ҫ�������Ѿ�ƽ�������ϸ
		while (count > 0)
		{
			auto it = pInfo._details.begin();
			pInfo._details.erase(it);
			count--;
		}

		//���, �������ʣ���, ����Ҫ������
		if (left > 0)
		{
			left = left * qty / abs(qty);

			DetailInfo dInfo;
			dInfo._long = qty > 0;
			dInfo._price = curPx;
			dInfo._volumn = abs(left);
			dInfo._opentime = curTm;
			dInfo._opentdate = curTDate;
			pInfo._details.push_back(dInfo);

			//���ﻹ��Ҫдһ�ʳɽ���¼
			double fee = calc_fee(stdCode, curPx, abs(qty), 0);
			fundInfo._fees += fee;
			fundInfo._balance -= fee;
		}
	}

	save_datas();
}

void WtEngine::push_task(TaskItem task)
{
	{
		BoostUniqueLock lock(_mtx_task);
		_task_queue.push(task);
	}
	

	if (_thrd_task == NULL)
	{
		_thrd_task.reset(new BoostThread([this]{
			task_loop();
		}));
	}

	_cond_task.notify_all();
}

void WtEngine::task_loop()
{
	while (!_terminated)
	{
		TaskQueue temp;
		{
			BoostUniqueLock lock(_mtx_task);
			if(_task_queue.empty())
			{
				_cond_task.wait(_mtx_task);
				continue;
			}

			temp.swap(_task_queue);
		}

		for (;;)
		{
			if(temp.empty())
				break;

			TaskItem& item = temp.front();
			item();
			temp.pop();
		}
	}
}

bool WtEngine::init_riskmon(WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;

	if (!cfg->getBoolean("active"))
		return false;

	const char* module = cfg->getCString("module");

	DllHandle hInst = DLLHelper::load_library(module);
	if (hInst == NULL)
	{
		WTSLogger::info2("risk", "���ģ��%s����ʧ��", module);
		return false;
	}

	FuncCreateRiskMonFact creator = (FuncCreateRiskMonFact)DLLHelper::get_symbol(hInst, "createRiskMonFact");
	if (creator == NULL)
	{
		DLLHelper::free_library(hInst);
		WTSLogger::info2("risk", "���ģ��%s������ȷ�ķ��ģ��", module);
		return false;
	}

	_risk_fact._module_inst = hInst;
	_risk_fact._module_path = module;
	_risk_fact._creator = creator;
	_risk_fact._remover = (FuncDeleteRiskMonFact)DLLHelper::get_symbol(hInst, "deleteRiskMonFact");
	_risk_fact._fact = _risk_fact._creator();

	const char* name = cfg->getCString("name");
	
	_risk_mon.reset(new WtRiskMonWrapper(_risk_fact._fact->createRiskMonotor(name), _risk_fact._fact));
	_risk_mon->self()->init(this, cfg);

	return true;
}