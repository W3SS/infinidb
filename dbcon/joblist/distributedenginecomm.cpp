/* Copyright (C) 2014 InfiniDB, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */

//
// $Id: distributedenginecomm.cpp 7759 2011-06-09 13:57:36Z dhill $
//
// C++ Implementation: distributedenginecomm
//
// Description:
//
//
// Author:  <pfigg@calpont.com>, (C) 2006
//
// Copyright: See COPYING file that comes with this distribution
//
//

#include <sstream>
#include <stdexcept>
#include <cassert>
#include <ctime>
#include <algorithm>
#include <unistd.h>
#ifndef _MSC_VER
#include <arpa/inet.h>
#else
#include <intrin.h>
#endif
#if __FreeBSD__
#include <sys/socket.h>
#endif
using namespace std;

#include <boost/scoped_array.hpp>
#include <boost/thread.hpp>
using namespace boost;

#define DISTRIBUTEDENGINECOMM_DLLEXPORT
#include "distributedenginecomm.h"
#undef DISTRIBUTEDENGINECOMM_DLLEXPORT

#include "messagequeue.h"
#include "bytestream.h"
using namespace messageqcpp;

#include "configcpp.h"
using namespace config;

#include "errorids.h"
#include "exceptclasses.h"
#include "messagelog.h"
#include "messageobj.h"
#include "loggingid.h"
using namespace logging;

#include "liboamcpp.h"
#include "snmpmanager.h"
using namespace snmpmanager;
using namespace oam;

#ifdef SHARED_NOTHING_DEMO
#include "mastersegmenttable.h"
#include "extentmap.h"
#include "dbrm.h"
#endif

#include "jobstep.h"
using namespace joblist;

#if defined(_MSC_VER) && !defined(_WIN64)
#  ifndef InterlockedAdd
#    define InterlockedAdd64 InterlockedAdd
#    define InterlockedAdd(x, y) ((x) + (y))
#  endif
#endif

namespace
{

  void  writeToLog(const char* file, int line, const string& msg, LOG_TYPE logto = LOG_TYPE_INFO)
  {
        LoggingID lid(05);
        MessageLog ml(lid);
        Message::Args args;
        Message m(0);
        args.add(file); 
        args.add("@");
        args.add(line);
        args.add(msg);
        m.format(args); 
	switch (logto)
	{
        	case LOG_TYPE_DEBUG:	ml.logDebugMessage(m); break;		
        	case LOG_TYPE_INFO: 	ml.logInfoMessage(m); break;	
        	case LOG_TYPE_WARNING:	ml.logWarningMessage(m); break;	
        	case LOG_TYPE_ERROR:	ml.logWarningMessage(m); break;	
        	case LOG_TYPE_CRITICAL:	ml.logCriticalMessage(m); break;	
	}
  }
  
  // @bug 1463. this function is added for PM failover. for dual/more nic PM,
  // this function is used to get the module name
  string getModuleNameByIPAddr(oam::ModuleTypeConfig moduletypeconfig, 
				 string ipAddress)
  {
  	string modulename = "";
  	DeviceNetworkList::iterator pt = moduletypeconfig.ModuleNetworkList.begin();
		for( ; pt != moduletypeconfig.ModuleNetworkList.end() ; pt++)
		{
			modulename = (*pt).DeviceName;
			HostConfigList::iterator pt1 = (*pt).hostConfigList.begin();
			for( ; pt1 != (*pt).hostConfigList.end() ; pt1++)
			{
				if (ipAddress == (*pt1).IPAddr)
					return modulename;
			}
		}
		return modulename;
  }

  struct EngineCommRunner
  {
    EngineCommRunner(joblist::DistributedEngineComm *jl,
		boost::shared_ptr<MessageQueueClient> cl, uint connectionIndex) : jbl(jl), client(cl),
		connIndex(connectionIndex) {}
    joblist::DistributedEngineComm *jbl;
    boost::shared_ptr<MessageQueueClient> client;
	uint connIndex;
    void operator()()
    {
      //cout << "Listening on client at 0x" << hex << (ptrdiff_t)client << dec << endl;
      try
      {
        jbl->Listen(client, connIndex);
      }
      catch(std::exception& ex)
      {
        string what(ex.what());
        cerr << "exception caught in EngineCommRunner: " << what << endl;
        if (what.find("St9bad_alloc") != string::npos)
        {
	  writeToLog(__FILE__, __LINE__, what, LOG_TYPE_CRITICAL);
//           abort();
        }
	else  writeToLog(__FILE__, __LINE__, what);
      }
      catch(...)
      {
	string msg("exception caught in EngineCommRunner.");
	writeToLog(__FILE__, __LINE__, msg);
        cerr << msg << endl;
      }
    }
  };

  uint64_t getInterleaveData(ISMPacketHeader *data)
  {
    ISMPacketHeader *MsgIn = data;
    int Command = MsgIn->Command;

	switch (Command) {
		case BATCH_PRIMITIVE_RUN:
		case DICT_TOKEN_BY_SCAN_COMPARE:
			return MsgIn->Reserve;
		case COL_BY_SCAN: {
			ColByScanRequestHeader * CRH;
			CRH = (ColByScanRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
		case COL_BY_SCAN_RANGE: {
			ColByScanRangeRequestHeader * CRH = (ColByScanRangeRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
		case COL_BY_RID: {
			ColByRIDRequestHeader * CRH;
			CRH = (ColByRIDRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
		case COL_AGG_BY_SCAN: {
			ColAggByScanRequestHeader * CRH;
			CRH = (ColAggByScanRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
		case COL_AGG_BY_RID: {
			ColAggByRIDRequestHeader * CRH;
			CRH = (ColAggByRIDRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
		case INDEX_BY_SCAN: {
			IndexByScanRequestHeader * CRH;
			CRH = (IndexByScanRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
		case INDEX_BY_COMPARE: {
			IndexByCompareRequestHeader * CRH;
			CRH = (IndexByCompareRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
		case DICT_TOKEN_BY_INDEX_COMPARE: {
			DictTokenByIndexRequestHeader * CRH;
			CRH = (DictTokenByIndexRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
#if 0
		case DICT_TOKEN_BY_SCAN_COMPARE: {
			DictTokenByScanRequestHeader * CRH;
			CRH = (DictTokenByScanRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
#endif
		case DICT_SIGNATURE: {
			DictSignatureRequestHeader * CRH;
			CRH = (DictSignatureRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
		case DICT_SIGNATURE_RANGE: {
			DictSignatureRangeRequestHeader * CRH;
			CRH = (DictSignatureRangeRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
		case DICT_AGGREGATE: {
			DictAggregateRequestHeader * CRH;
			CRH = (DictAggregateRequestHeader *) (MsgIn+1);
			return CRH->LBID;
		}
		case INDEX_WALK: {
			IndexWalkHeader *iwh = (IndexWalkHeader *)MsgIn;
			return iwh->LBID;
		}
		case INDEX_LIST: {
			IndexListHeader *ilh = (IndexListHeader *)MsgIn;
			return ilh->LBID;
		}
		case COL_LOOPBACK: {
			return (uint64_t)NULL;
		}
		default:
			cout << "DEC::getInterleaveData(): unknown primitive command " << Command
				<< endl;
			throw logic_error("DEC::getInterleaveData(): unknown primitive command");
	}
}

template <typename T>
struct QueueShutdown : public unary_function<T&, void>
{
	void operator()(T& x)
	{
		x.shutdown();
	}
};

#ifdef _MSC_VER
mutex inet_ntoa_mutex;
#endif

inline const string sin_addr2String(const in_addr src)
{
	string s;
#ifdef _MSC_VER
	mutex::scoped_lock lk(inet_ntoa_mutex);
	s = inet_ntoa(src);
#else
	char dst[INET_ADDRSTRLEN];
	s = inet_ntop(AF_INET, &src, dst, INET_ADDRSTRLEN);
#endif
	return s;
}

}

/** Debug macro */
#define THROTTLE_DEBUG 0
#if THROTTLE_DEBUG
#define THROTTLEDEBUG std::cout
#else
#define THROTTLEDEBUG if (false) std::cout
#endif

namespace joblist
{
  DistributedEngineComm* DistributedEngineComm::fInstance = 0;
  
  /*static*/
  DistributedEngineComm* DistributedEngineComm::instance(ResourceManager& rm)
  {
    if (fInstance == 0)
        fInstance = new DistributedEngineComm(rm);

    return fInstance;
  }

  DistributedEngineComm::DistributedEngineComm(ResourceManager& rm) : 
	fRm(rm),
	fLBIDShift(fRm.getPsLBID_Shift()),
	pmCount(0),
	fMulticast(rm.getPsMulticast()),
	fMulticastSender()
  {
    Setup();
  }

  DistributedEngineComm::~DistributedEngineComm()
  {
    Close();
  }

void DistributedEngineComm::Setup()
{
    makeBusy(true);

	throttleThreshold = fRm.getDECThrottleThreshold();
    uint newPmCount = fRm.getPsCount();
    int cpp = fRm.getPsConnectionsPerPrimProc();
    tbpsThreadCount = fRm.getJlNumScanReceiveThreads();
    unsigned numConnections = newPmCount * cpp;
    oam::Oam oam;
    string ipAddress;
    ModuleTypeConfig moduletypeconfig; 
	try {
    	oam.getSystemConfig("pm", moduletypeconfig);
	} catch (...) {
		writeToLog(__FILE__, __LINE__, "oam.getSystemConfig error, unknown exception", LOG_TYPE_ERROR);
		throw runtime_error("Setup failed");
	}

    //This needs to make sense when compared to the extent size
    //     fLBIDShift = static_cast<unsigned>(config::Config::uFromText(fConfig->getConfig(section, "LBID_Shift")));

    char buff[25];

    for (unsigned i = 0; i < numConnections; i++) {
        sprintf(buff, "PMS%u",i+1);
        string fServer (buff);

        boost::shared_ptr<MessageQueueClient>
			cl(new MessageQueueClient(fServer, fRm.getConfig()));
        boost::shared_ptr<boost::mutex> nl(new boost::mutex());
        try {
            if (cl->connect()) {
                newClients.push_back(cl);
                // assign the module name
                ipAddress = sin_addr2String(cl->serv_addr().sin_addr);
				cl->moduleName(getModuleNameByIPAddr(moduletypeconfig, ipAddress));
                newLocks.push_back(nl);
                StartClientListener(cl, i);
            } else {
                throw runtime_error("Connection refused");
            }
        } catch (std::exception& ex) {
			if (i < newPmCount)
				newPmCount--;
            writeToLog(__FILE__, __LINE__, "Could not connect to " + fServer + ": " + ex.what(), LOG_TYPE_ERROR);
            cerr << "Could not connect to " << fServer << ": " << ex.what() << endl;
        } catch (...) {
			if (i < newPmCount)
				newPmCount--;
            writeToLog(__FILE__, __LINE__, "Could not connect to " + fServer, LOG_TYPE_ERROR);
        }
    }
    // for every entry in newClients up to newPmCount, scan for the same ip in the
    // first pmCount.  If there is no match, it's a new node,
    //    call the event listeners' newPMOnline() callbacks.
    mutex::scoped_lock lock(eventListenerLock);
    for (uint i = 0; i < newPmCount; i++) {
#ifdef _MSC_VER
		LONG j;
#else
        uint j;
#endif
        for (j = 0; j < pmCount; j++) {
            if (newClients[i]->serv_addr().sin_addr.s_addr ==
			  fPmConnections[j]->serv_addr().sin_addr.s_addr)
                break;
        }
        if (j == pmCount)
            for (uint k = 0; k < eventListeners.size(); k++)
                eventListeners[k]->newPMOnline(i);
    }
    lock.unlock();

    fWlock.swap(newLocks);
    fPmConnections.swap(newClients);
    // memory barrier to prevent the pmCount assignment migrating upward
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)
    __sync_synchronize();
#elif defined(_MSC_VER)
	_InterlockedOr(&pmCount, 0);
#endif
    pmCount = newPmCount;

    newLocks.clear();
    newClients.clear();
}

int DistributedEngineComm::Close()
  {
    //cout << "DistributedEngineComm::Close() called" << endl;

    makeBusy(false);
    // for each MessageQueueClient in pmConnections delete the MessageQueueClient;
    fPmConnections.clear();
    fPmReader.clear();
    return 0;
  }


void DistributedEngineComm::Listen ( boost::shared_ptr<MessageQueueClient> client, uint connIndex)
{
	SBS sbs;

	try {
		while ( Busy() )
		{
			//TODO: This call blocks so setting Busy() in another thread doesn't work here...
			sbs = client->read();
			if ( sbs->length() != 0 )
				addDataToOutput(sbs, connIndex);
			else // got zero bytes on read, nothing more will come
				goto Error;
		}
		return;
	} catch (std::exception& e)
	{
		cerr << "DEC Caught EXCEPTION: " << e.what() << endl;
		goto Error;
	}
	catch (...)
	{
		cerr << "DEC Caught UNKNOWN EXCEPT" << endl;
		goto Error;
	}
Error:
	// @bug 488 - error condition! push 0 length bs to messagequeuemap and
	// eventually let jobstep error out.
	mutex::scoped_lock lk(fMlock);
	//cout << "WARNING: DEC READ 0 LENGTH BS FROM " << client->otherEnd()<< endl;

	MessageQueueMap::iterator map_tok;
	sbs.reset(new ByteStream(0));

	for (map_tok = fSessionMessages.begin(); map_tok != fSessionMessages.end(); ++map_tok)
	{
		map_tok->second->queue.clear();
#ifdef _MSC_VER
		InterlockedIncrement(&map_tok->second->unackedWork[0]);
#else
		__sync_add_and_fetch(&map_tok->second->unackedWork[0], 1);  // prevent an error msg
#endif
		map_tok->second->queue.push(sbs);
	}
	lk.unlock();

	// reset the pmconnection vector
	ClientList tempConns;

	{
		mutex::scoped_lock onErrLock(fOnErrMutex);
		string moduleName = client->moduleName();
		//cout << "moduleName=" << moduleName << endl;
		for ( uint i = 0; i < fPmConnections.size(); i++)
		{
			if (moduleName != fPmConnections[i]->moduleName())
				tempConns.push_back(fPmConnections[i]);
			//else
			//cout << "DEC remove PM" << fPmConnections[i]->otherEnd() << " moduleName=" << fPmConnections[i]->moduleName() << endl;
		}

		if (tempConns.size() == fPmConnections.size()) return;

		fPmConnections.swap(tempConns);
		pmCount = (pmCount == 0 ? 0 : pmCount - 1);
		//cout << "PMCOUNT=" << pmCount << endl;

		// send alarm
		SNMPManager alarmMgr;
		string alarmItem = sin_addr2String(client->serv_addr().sin_addr);
		alarmItem.append(" PrimProc");
		alarmMgr.sendAlarmReport(alarmItem.c_str(), oam::CONN_FAILURE, SET);
	}
	return;
}

void DistributedEngineComm::addQueue(uint32_t key, bool sendACKs)
{
	bool b;

	mutex* lock = new mutex();
	condition* cond = new condition();
	boost::shared_ptr<MQE> mqe(new MQE(pmCount));
	
	mqe->queue = StepMsgQueue(lock, cond);
	mqe->sendACKs = sendACKs;
	mqe->throttled = false;
	
	mutex::scoped_lock lk ( fMlock );
	b = fSessionMessages.insert(pair<uint32_t, boost::shared_ptr<MQE> >(key, mqe)).second;
	if (!b) {
		ostringstream os;
		os << "DEC: attempt to add a queue with a duplicate ID " << key << endl;
		throw runtime_error(os.str());
	}
}

  void DistributedEngineComm::removeQueue(uint32_t key)
  {
	mutex::scoped_lock lk(fMlock);
	MessageQueueMap::iterator map_tok = fSessionMessages.find(key);
	if (map_tok == fSessionMessages.end())
		return;
	map_tok->second->queue.shutdown();
	map_tok->second->queue.clear();
	fSessionMessages.erase(map_tok);
  }

  void DistributedEngineComm::shutdownQueue(uint32_t key)
  {
	  mutex::scoped_lock lk(fMlock);
	  MessageQueueMap::iterator map_tok = fSessionMessages.find(key);
	  if (map_tok == fSessionMessages.end())
		  return;
	  map_tok->second->queue.shutdown();
	  map_tok->second->queue.clear();
  }

void DistributedEngineComm::read(uint32_t key, SBS &bs)
{
	boost::shared_ptr<MQE> mqe;
	
	//Find the StepMsgQueueList for this session
    mutex::scoped_lock lk(fMlock);
    MessageQueueMap::iterator map_tok = fSessionMessages.find(key);
    if(map_tok == fSessionMessages.end())
    {
      ostringstream os;

      os << "DEC: attempt to read(bs) from a nonexistent queue\n";
      throw runtime_error(os.str());
    }
	
    mqe = map_tok->second;
 	lk.unlock();

    //this method can block: you can't hold any locks here...
    TSQSize_t queueSize = mqe->queue.pop(&bs);
	
	if (bs && mqe->sendACKs) {
		mutex::scoped_lock lk(ackLock);
		if (mqe->throttled && !mqe->hasBigMsgs && queueSize.size <= disableThreshold)
			setFlowControl(false, key, mqe);
		vector<SBS> v;
		v.push_back(bs);
		sendAcks(key, v, mqe, queueSize.size);
	}
	if (!bs)
		bs.reset(new ByteStream());
}

  const ByteStream DistributedEngineComm::read(uint32_t key)
  {
	SBS sbs;
	boost::shared_ptr<MQE> mqe;
	
    //Find the StepMsgQueueList for this session
    mutex::scoped_lock lk(fMlock);
    MessageQueueMap::iterator map_tok = fSessionMessages.find(key);
    if(map_tok == fSessionMessages.end())
    {
      ostringstream os;

      os << "DEC: read(): attempt to read from a nonexistent queue\n";
      throw runtime_error(os.str());
    }
	
    mqe = map_tok->second;
 	lk.unlock();

    TSQSize_t queueSize = mqe->queue.pop(&sbs);
	
	if (sbs && mqe->sendACKs) {
		mutex::scoped_lock lk(ackLock);
		if (mqe->throttled && !mqe->hasBigMsgs && queueSize.size <= disableThreshold)
			setFlowControl(false, key, mqe);
		vector<SBS> v;
		v.push_back(sbs);
		sendAcks(key, v, mqe, queueSize.size);
	}
	if (!sbs)
		sbs.reset(new ByteStream());
    return *sbs;
  }

  void DistributedEngineComm::read_all(uint32_t key, vector<SBS> &v)
  {
	boost::shared_ptr<MQE> mqe;
	
	mutex::scoped_lock lk(fMlock);
	MessageQueueMap::iterator map_tok = fSessionMessages.find(key);
    if(map_tok == fSessionMessages.end())
    {
      ostringstream os;
      os << "DEC: read_all(): attempt to read from a nonexistent queue\n";
      throw runtime_error(os.str());
    }
 
	mqe = map_tok->second;
 	lk.unlock();

	mqe->queue.pop_all(v);

	if (mqe->sendACKs) {
		mutex::scoped_lock lk(ackLock);
		sendAcks(key, v, mqe, 0);
	}
  }
	
  void DistributedEngineComm::read_some(uint32_t key, uint divisor, vector<SBS> &v)
  {
	boost::shared_ptr<MQE> mqe;
	
	mutex::scoped_lock lk(fMlock);
	MessageQueueMap::iterator map_tok = fSessionMessages.find(key);
    if(map_tok == fSessionMessages.end())
    {
      ostringstream os;

      os << "DEC: read_some(): attempt to read from a nonexistent queue\n";
      throw runtime_error(os.str());
    }
	
	mqe = map_tok->second;
	lk.unlock();

	TSQSize_t queueSize = mqe->queue.pop_some(divisor, v, 1);   // need to play with the min #

	if (mqe->sendACKs) {
		mutex::scoped_lock lk(ackLock);
		if (mqe->throttled && !mqe->hasBigMsgs && queueSize.size <= disableThreshold)
			setFlowControl(false, key, mqe);	
		sendAcks(key, v, mqe, queueSize.size);
	}
  }

void DistributedEngineComm::sendAcks(uint32_t uniqueID, const vector<SBS> &msgs,
	boost::shared_ptr<MQE> mqe, size_t queueSize)
{
	ISMPacketHeader *ism;
	uint32_t l_msgCount = msgs.size();

	/* If the current queue size > target, do nothing.
	 * If the original queue size > target, ACK the msgs below the target.
	 */
	if (!mqe->throttled || queueSize >= mqe->targetQueueSize) {
		/* no acks will be sent, but update unackedwork to keep the #s accurate */
		uint16_t dummy16;
		uint32_t dummy32;
		while (l_msgCount > 0) {
			nextPMToACK(mqe, l_msgCount, &dummy32, &dummy16);
			assert(dummy16 <= l_msgCount);
			l_msgCount -= dummy16;
		}
		return;
	}

	size_t totalMsgSize = 0;
	for (uint i = 0; i < msgs.size(); i++)
		totalMsgSize += msgs[i]->lengthWithHdrOverhead();
	
	if (queueSize + totalMsgSize > mqe->targetQueueSize) {
		/* update unackedwork for the overage that will never be acked */
		int64_t overage = queueSize + totalMsgSize - mqe->targetQueueSize;
		uint16_t dummy16;
		uint32_t dummy32;
		uint msgsToIgnore;
		for (msgsToIgnore = 0; overage >= 0; msgsToIgnore++)
			overage -= msgs[msgsToIgnore]->lengthWithHdrOverhead();
		if (overage < 0)
			msgsToIgnore--;
		l_msgCount = msgs.size() - msgsToIgnore;  // this num gets acked
		while (msgsToIgnore > 0) {
			nextPMToACK(mqe, msgsToIgnore, &dummy32, &dummy16);
			assert(dummy16 <= msgsToIgnore);
			msgsToIgnore -= dummy16;
		}
	}
	
	if (l_msgCount > 0) {
		ByteStream msg(sizeof(ISMPacketHeader));
		uint16_t *toAck;
		
		ism = (ISMPacketHeader *) msg.getInputPtr();
		// The only var checked by ReadThread is the Command var.  The others
		// are wasted space.  We hijack the Size, Reserve, & Flags fields for the 
		// params to the ACK msg.
		
		*((uint32_t *) &ism->Reserve) = uniqueID;
		ism->Command = BATCH_PRIMITIVE_ACK;
		toAck = &ism->Size;

		msg.advanceInputPtr(sizeof(ISMPacketHeader));
		
		while (l_msgCount > 0) {
			/* could have to send up to pmCount ACKs */
			uint32_t sockIndex;
		
			/* This will reset the ACK field in the Bytestream directly, and nothing
			 * else needs to change if multiple msgs are sent. */
			nextPMToACK(mqe, l_msgCount, &sockIndex, toAck);
			assert(*toAck <= l_msgCount);
			l_msgCount -= *toAck;
			writeToClient(sockIndex, msg);
		}
	}
}

void DistributedEngineComm::nextPMToACK(boost::shared_ptr<MQE> mqe, uint16_t maxAck,
	uint32_t *sockIndex, uint16_t *numToAck)
{
#ifdef _MSC_VER
	LONG i;
	uint &nextIndex = mqe->ackSocketIndex;
#else
	uint i;
	uint32_t &nextIndex = mqe->ackSocketIndex;
#endif
		
	/* Other threads can be touching mqe->unackedWork at the same time, but because of 
	 * the locking env, mqe->unackedWork can only grow; whatever gets latched in this fcn
	 * is a safe minimum at the point of use. */
	
	if (mqe->unackedWork[nextIndex] >= maxAck) {
#ifdef _MSC_VER
		InterlockedAdd(&mqe->unackedWork[nextIndex], -maxAck);
#else
		__sync_sub_and_fetch(&mqe->unackedWork[nextIndex], maxAck);
#endif
		*sockIndex = nextIndex;
		*numToAck = maxAck;
		if (pmCount > 0)
			nextIndex = (nextIndex + 1) % pmCount;
		return;
	}
	else {
		for (i = 0; i < pmCount; i++) {
			uint32_t curVal = mqe->unackedWork[nextIndex];
			uint32_t unackedWork = (curVal > maxAck ? maxAck : curVal);
			if (unackedWork > 0) {
#ifdef _MSC_VER
				int32_t tmp = static_cast<int32_t>(unackedWork);
				InterlockedAdd(&mqe->unackedWork[nextIndex], -tmp);
#else
				__sync_sub_and_fetch(&mqe->unackedWork[nextIndex], unackedWork);
#endif
				*sockIndex = nextIndex;
				*numToAck = unackedWork;
				if (pmCount > 0)
					nextIndex = (nextIndex + 1) % pmCount;
				return;
			}
			if (pmCount > 0)
				nextIndex = (nextIndex + 1) % pmCount;
		}
		cerr << "DEC::nextPMToACK(): Couldn't find a PM to ACK! ";
		for (i = 0; i < pmCount; i++)
			cerr << mqe->unackedWork[i] << " ";
		cerr << " max: " << maxAck;
		cerr << endl;
		//make sure the returned vars are legitimate
		*sockIndex = nextIndex;
		*numToAck = maxAck/pmCount;
		if (pmCount > 0)
			nextIndex = (nextIndex + 1) % pmCount;
		return;
	}
}

void DistributedEngineComm::setFlowControl(bool enabled, uint32_t uniqueID, boost::shared_ptr<MQE> mqe)
{
	mqe->throttled = enabled;
	ByteStream msg(sizeof(ISMPacketHeader));
	ISMPacketHeader *ism = (ISMPacketHeader *) msg.getInputPtr();

	*((uint32_t *) &ism->Reserve) = uniqueID;
	ism->Command = BATCH_PRIMITIVE_ACK;
	ism->Size = (enabled ? 0 : -1);
	msg.advanceInputPtr(sizeof(ISMPacketHeader));
	
	for (uint i = 0; i < mqe->pmCount; i++)
		writeToClient(i, msg);
}

#ifdef SHARED_NOTHING_DEMO
  void DistributedEngineComm::write(ByteStream& msg, BRM::OID_t oid)
#else
  void DistributedEngineComm::write(ByteStream& msg)
#endif
  {
	ISMPacketHeader *ism = (ISMPacketHeader *) msg.buf();
	uint64_t idx;
	uint numConn = fPmConnections.size();
	int connectionIndex;

    if (numConn > 0) {
		switch (ism->Command) {
			case BATCH_PRIMITIVE_CREATE:
				/* Disable flow control initially */
				msg << (uint32_t) -1;
			case BATCH_PRIMITIVE_DESTROY:
			case BATCH_PRIMITIVE_ADD_JOINER:
			case BATCH_PRIMITIVE_END_JOINER:
			case BATCH_PRIMITIVE_ABORT:
			case DICT_CREATE_EQUALITY_FILTER:
			case DICT_DESTROY_EQUALITY_FILTER:
				if (fMulticast)
				try
				{
					mutex::scoped_lock lk(fMulticastLock);
					fMulticastSender.send(msg);
				}
				catch (const MulticastException&)
				{
					writeToLog(__FILE__, __LINE__, " Error in setting up Multicast. Turning Multicast send off.", LOG_TYPE_WARNING);
					fMulticast = false;
#ifdef _MSC_VER
					LONG i;
#else
					uint i;
#endif
					for (i = 0; i < pmCount; i++)
						writeToClient(i, msg);
					
				}
				else {
					/* XXXPAT: This relies on the assumption that the first pmCount "PMS*"
					entries in the config file point to unique PMs */
#ifdef _MSC_VER
					LONG i;
#else
					uint i;
#endif
					for (i = 0; i < pmCount; i++)
						writeToClient(i, msg);
					return;
				}
			case BATCH_PRIMITIVE_RUN:
			case DICT_TOKEN_BY_SCAN_COMPARE:
				idx = getInterleaveData(ism);
				connectionIndex = idx % numConn;
 				THROTTLEDEBUG << "DEC: sending BPR for idx " << idx << " to PM " << connectionIndex << endl;
				break;
			default:
				idx = getInterleaveData(ism);
				connectionIndex = (idx >> fLBIDShift) % numConn;
		}
#ifdef SHARED_NOTHING_DEMO
		if (oid == 0)
		{
			uint32_t fbo;
			BRM::DBRM brm;
			brm.lookup(idx, 2000000000, false, oid, fbo);
		}
		connectionIndex = oid % pmCount;
		//cout << "DEC: " << pmCount << ' ' << oid << ' ' << connectionIndex << endl;
#endif
        writeToClient(connectionIndex, msg);
    }
	else
	{
		throw IDBExcept(ERR_NO_PRIMPROC);
	}
  }

void DistributedEngineComm::write(const messageqcpp::ByteStream &msg, uint connection)
{
	newClients[connection]->write(msg);
}

  void DistributedEngineComm::StartClientListener(boost::shared_ptr<MessageQueueClient> cl, uint connIndex)
  {
    boost::thread *thrd = new boost::thread(EngineCommRunner(this, cl, connIndex));
    fPmReader.push_back(thrd);
  }

  void DistributedEngineComm::addDataToOutput(SBS sbs, uint connIndex)
  {
    ISMPacketHeader *hdr = (ISMPacketHeader*)(sbs->buf());
    PrimitiveHeader *p = (PrimitiveHeader *)(hdr+1);
	uint32_t uniqueId = p->UniqueID;
	boost::shared_ptr<MQE> mqe;
 
    mutex::scoped_lock lk(fMlock);
    MessageQueueMap::iterator map_tok = fSessionMessages.find(uniqueId);
    if(map_tok == fSessionMessages.end())
    {
    	// For debugging...
        //cerr << "DistributedEngineComm::AddDataToOutput: tried to add a message to a dead session: " << uniqueId << ", size " << sbs->length() << ", step id " << p->StepID << endl;
        return;
    }
 	mqe = map_tok->second;
	lk.unlock();
    
	if (pmCount > 0) {
#ifdef _MSC_VER
		InterlockedIncrement(&mqe->unackedWork[connIndex % pmCount]);
#else
		__sync_add_and_fetch(&mqe->unackedWork[connIndex % pmCount], 1);
#endif
	}
	TSQSize_t queueSize = mqe->queue.push(sbs);
	
	if (mqe->sendACKs) {
		mutex::scoped_lock lk(ackLock);
		uint64_t msgSize = sbs->lengthWithHdrOverhead();
		if (!mqe->throttled && msgSize > (targetRecvQueueSize/2))
			doHasBigMsgs(mqe, (300*1024*1024 > 3*msgSize ?
			  300*1024*1024 : 3*msgSize));  //buffer at least 3 big msgs
		if (!mqe->throttled && queueSize.size >= mqe->targetQueueSize)
			setFlowControl(true, uniqueId, mqe);
	}
  }

void DistributedEngineComm::doHasBigMsgs(boost::shared_ptr<MQE> mqe, uint64_t targetSize)
{
	mqe->hasBigMsgs = true;
	if (mqe->targetQueueSize < targetSize)
		mqe->targetQueueSize = targetSize;
}

int DistributedEngineComm::writeToClient(size_t index, const ByteStream& bs)
{
	try
	{
		MessageQueueMap::iterator recvQueue = fSessionMessages.end();
		
		// @bug 488. fPmConnections may be shrinked already due to PM node failure.
		if (index >= fPmConnections.size() ) return 0;
		ClientList::value_type client = fPmConnections[index];
		if (!client->isAvailable()) return 0;

		mutex::scoped_lock lk(*(fWlock[index]));
		client->write(bs);
		return 0;
	}
	catch(...)
	{
		// @bug 488. error out under such condition instead of re-trying other connection, 
		// by pushing 0 size bytestream to messagequeue and throw excpetion
		SBS sbs;
		mutex::scoped_lock lk(fMlock);
		//cout << "WARNING: DEC WRITE BROKEN PIPE. PMS index = " << index << endl;
		MessageQueueMap::iterator map_tok;
		sbs.reset(new ByteStream(0));

		for (map_tok = fSessionMessages.begin(); map_tok != fSessionMessages.end(); ++map_tok)
		{
			map_tok->second->queue.clear();
#ifdef _MSC_VER
			InterlockedIncrement(&map_tok->second->unackedWork[0]);
#else
			__sync_add_and_fetch(&map_tok->second->unackedWork[0], 1);   // prevent an error msg
#endif
			map_tok->second->queue.push(sbs);
		}

		lk.unlock();

		// reconfig the connection array
		ClientList tempConns;		
		{
			//cout << "WARNING: DEC WRITE BROKEN PIPE " << fPmConnections[index]->otherEnd()<< endl;
			mutex::scoped_lock onErrLock(fOnErrMutex);
			string moduleName = fPmConnections[index]->moduleName();
			//cout << "module name = " << moduleName << endl;
			if (index >= fPmConnections.size()) return 0;

			for (uint i = 0; i < fPmConnections.size(); i++)
			{
				if (moduleName != fPmConnections[i]->moduleName())              
					tempConns.push_back(fPmConnections[i]);
			}
			if (tempConns.size() == fPmConnections.size()) return 0;
			fPmConnections.swap(tempConns);
			pmCount = (pmCount == 0 ? 0 : pmCount - 1);
		}

		// send alarm
		SNMPManager alarmMgr;
		string alarmItem("UNKNOWN");
		if (index < fPmConnections.size())
			alarmItem = sin_addr2String(fPmConnections[index]->serv_addr().sin_addr);
		alarmItem.append(" PrimProc");
		alarmMgr.sendAlarmReport(alarmItem.c_str(), oam::CONN_FAILURE, SET);
		throw runtime_error("DistributedEngineComm::write: Broken Pipe error");
	}
}

uint DistributedEngineComm::size(uint32_t key)
{
	mutex::scoped_lock lk(fMlock);
	MessageQueueMap::iterator map_tok = fSessionMessages.find(key);
    if(map_tok == fSessionMessages.end())
      throw runtime_error("DEC::size() attempt to get the size of a nonexistant queue!");
	boost::shared_ptr<MQE> mqe = map_tok->second;
    //TODO: should probably check that this is a valid iter...
 	lk.unlock();
	return mqe->queue.size().count;
}

void DistributedEngineComm::addDECEventListener(DECEventListener *l)
{
	mutex::scoped_lock lk(eventListenerLock);
	eventListeners.push_back(l);
}

void DistributedEngineComm::removeDECEventListener(DECEventListener *l)
{
	mutex::scoped_lock lk(eventListenerLock);
	std::vector<DECEventListener *> newListeners;
	uint s = eventListeners.size();

	for (uint i = 0; i < s; i++)
		if (eventListeners[i] != l)
			newListeners.push_back(eventListeners[i]);
	eventListeners.swap(newListeners);
}

}
// vim:ts=4 sw=4:
