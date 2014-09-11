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

// $Id: filterstep.cpp 7396 2011-02-03 17:54:36Z rdempsey $

#include <string>
#include <sstream>
#include <cassert>
#include <stdexcept>
#include <ctime>
using namespace std;

#include "calpontsystemcatalog.h"
using namespace execplan;

#include "jobstep.h"
#include "elementtype.h"
#include "filteroperation.h"
#include "timeset.h"

using namespace joblist;

// move to header file
//const uint32_t defaultFlushInterval = 0x2000;
namespace joblist
{

//@bug 686. Make filterstep doing jobs in seperate thread and return to main immediately.
// So the other job steps can start.
struct FSRunner
{
	FSRunner(FilterStep* p) : joiner(p)
    {}
	FilterStep *joiner;
    void operator()()
    {
        try {
			joiner->doFilter();
		}
		catch (std::exception &e) {
			std::cout << "FilterStep caught: " << e.what() << std::endl;
			catchHandler(e.what());
		}
		catch (...) {
			string msg("FSRunner caught something not an exception!");
			std::cout << msg << std::endl;
			catchHandler(msg);
		}
    }
};
    
FilterStep::FilterStep(uint32_t sessionId,
		uint32_t txnId,
		uint32_t statementId,
		execplan::CalpontSystemCatalog::ColType colType) :
	fSessionId(sessionId),
	fTxnId(txnId),
	fStepId(0),
	fStatementId(statementId),
	fTableOID(0),
	fColType(colType)
{
}

FilterStep::~FilterStep()
{
}

void FilterStep::join()
{
    runner->join();
}

void FilterStep::setBOP(int8_t b)
{
	fBOP = b;
}


void FilterStep::run()
{
	if (traceOn())
	{
		syslogStartStep(16,             // exemgr subsystem
			std::string("FilterStep")); // step name
	}

    runner.reset(new boost::thread(FSRunner(this)));
}

void FilterStep::doFilter()
{
	assert(fInputJobStepAssociation.outSize() == 2);
	assert(fOutputJobStepAssociation.outSize() == 1);
 	StringFifoDataList* fStrFAp = 0;
 	StringFifoDataList* fStrFBp = 0;
 	StringFifoDataList* strFifo = 0;
 	StrDataList* strResult = 0;
 	
 	FifoDataList* fFBP = 0;
	FifoDataList* fFAP = 0;
	FifoDataList* fifo = 0;
	DataList_t* result = 0;
	
	TimeSet timer;
try
{

	fFAP = fInputJobStepAssociation.outAt(0)->fifoDL();
	if ( !fFAP )
	{
		fStrFAp = fInputJobStepAssociation.outAt(0)->stringDL();
		assert(fStrFAp);
		
		fStrFBp = fInputJobStepAssociation.outAt(1)->stringDL();
		assert(fStrFBp);
		
		strFifo = fOutputJobStepAssociation.outAt(0)->stringDL();
		
		strResult = fOutputJobStepAssociation.outAt(0)->stringDataList();
	}
	else
	{		
		fFBP = fInputJobStepAssociation.outAt(1)->fifoDL();
		assert(fFBP);
		fifo = fOutputJobStepAssociation.outAt(0)->fifoDL();
		
		result = fOutputJobStepAssociation.outAt(0)->dataList();
	}
	ostringstream ss;  //tester
	ss << "Filter step id " << fStepId << " threw an exception";
	throw runtime_error(ss.str());
	resultCount = 0;
	if (fTableOID >= 3000 && dlTimes.FirstReadTime().tv_sec==0)
	{
		dlTimes.setFirstReadTime();
	}
	uint                cop = BOP();
		
	if (0 < fInputJobStepAssociation.status())
	{
		fOutputJobStepAssociation.status(fInputJobStepAssociation.status());
	}
	else
	{
		FilterOperation filterOP;
		if ( !fFAP )
		{
			if ( strFifo )
			{
				filterOP.filter( cop, *fStrFAp, *fStrFBp, *strFifo, resultCount, timer);
			}
			else
			{
				filterOP.filter( cop, *fStrFAp, *fStrFBp, *strResult, resultCount, timer);
			}
		}
		else
		{
			if ( fifo )
			{
				filterOP.filter( cop, *fFAP, *fFBP, *fifo, resultCount, timer);
			}
			else
			{
				filterOP.filter( cop, *fFAP, *fFBP, *result,resultCount, timer );
			}
		}
	} //else fInputJobStepAssociation.status() == 0
}//try
catch (std::exception &e) 
{
	std::cout << "FilterStep caught: " << e.what() << std::endl;
	unblockDataLists(fifo, strFifo, strResult, result);
	catchHandler(e.what());
	fOutputJobStepAssociation.status(logging::filterStepErr);
}
catch (...) 
{
	string msg("FSRunner caught something not an exception!");
	std::cout << msg << std::endl;
	unblockDataLists(fifo, strFifo, strResult, result);
	catchHandler(msg);
	fOutputJobStepAssociation.status(logging::filterStepErr);
}

		
	if (fTableOID >= 3000)
		dlTimes.setEndOfInputTime();
	//...Print job step completion information
	if (fTableOID >= 3000 && traceOn())
	{
		time_t finTime = time(0);
		char finTimeString[50];
		ctime_r(&finTime, finTimeString);
		finTimeString[ strlen(finTimeString)-1 ] = '\0';
		
		ostringstream logStr;
		logStr << "ses:" << fSessionId << " st: " << fStepId <<
			" finished at " << finTimeString <<
			"; 1st read " << dlTimes.FirstReadTimeString() <<
			"; EOI " << dlTimes.EndOfInputTimeString()
		<<"; Output:"<<resultCount
		<< "\n\trun time: " << JSTimeStamp::tsdiffstr(dlTimes.EndOfInputTime(), dlTimes.FirstReadTime())
		<< "s, " << filterCompare << timer.totalTime(filterCompare) 
		<< "s, " << filterInsert << timer.totalTime(filterInsert) 
		<< "s, " << filterFinish << timer.totalTime(filterFinish)
		<< "s\n\t" << "Job completion status: " << fOutputJobStepAssociation.status() << endl;
		
		logEnd(logStr.str().c_str());

			syslogEndStep(16, // exemgr subsystem
				0,            // no blocked datalist input  to report
				0);           // no blocked datalist output to report
	} 
}	

void FilterStep::unblockDataLists(FifoDataList* fifo, StringFifoDataList* strFifo, StrDataList* strResult, DataList_t* result )
{
	if (fifo) fifo->endOfInput();
	else if (strFifo) strFifo->endOfInput();
	else if (strResult) strResult->endOfInput();
	else if (result) result->endOfInput();
}

const string FilterStep::toString() const
{
	ostringstream oss;
	size_t idlsz;

	idlsz = fInputJobStepAssociation.outSize();
	assert(idlsz == 2);
	
	oss << "FilterStep      ses:" << fSessionId << " txn:" << fTxnId <<
		" st:" << fStepId;
	oss << " in  tb/col1:" << fTableOID << "/";
	oss << " " << fInputJobStepAssociation.outAt(0); // output will include oid
	oss << " in  tb/col2:" << fTableOID << "/";
	oss << " " << fInputJobStepAssociation.outAt(1);

	idlsz = fOutputJobStepAssociation.outSize();
	assert(idlsz == 1);

	oss << endl << "                     out tb/col:" << fTableOID << "/";
	oss << " " << fOutputJobStepAssociation.outAt(0);// output will include oid

	return oss.str();
}

}

