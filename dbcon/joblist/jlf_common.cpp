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

//  $Id: jlf_common.cpp 8873 2012-09-07 19:58:19Z xlou $


#include "calpontsystemcatalog.h"
#include "aggregatecolumn.h"
#include "simplecolumn.h"
using namespace execplan;

#include "messagelog.h"
using namespace logging;

#include <boost/algorithm/string/case_conv.hpp>
namespace ba=boost::algorithm;

#include "dbrm.h"
#include "extentmap.h"
using namespace BRM;

#include "jlf_common.h"
using namespace joblist;

namespace
{

// @brief Returns unique key for a column, table, or expresssion.
uint uniqTupleKey(JobInfo& jobInfo,
					CalpontSystemCatalog::OID& o,
					CalpontSystemCatalog::OID& t,
					const std::string& cn,
					const std::string& ca,
					const std::string& sn,
					const std::string& tn,
					const std::string& ta,
					const std::string& vw,
					bool correlated=false)
{
	uint64_t subId = jobInfo.subId;
	if (correlated)
		subId = jobInfo.pSubId;
	
	std::string alias = ta;
//	if (!ca.empty())
//		alias += "." + ca;
	UniqId id(o, alias, vw, subId);
	TupleKeyMap::iterator iter = jobInfo.keyInfo->tupleKeyMap.find(id);
	if (iter != jobInfo.keyInfo->tupleKeyMap.end())
		return iter->second;

	uint newId = jobInfo.keyInfo->nextKey++;
	jobInfo.keyInfo->tupleKeyMap[id] = newId;
	jobInfo.keyInfo->tupleKeyVec.push_back(id);
	jobInfo.keyInfo->tupleKeyToTableOid.insert(std::make_pair(newId, t));

	std::string ss = vw;
	if (ss.length() > 0)
		ss += ".";

	if (sn.length() > 0)
		ss += sn + ".";

	if (o != t)
	{
		std::string name = cn;
		if (!ca.empty()) // has an alias
			name = ca;
		else if (ta.compare(0,5,"$sub_") && ta.compare(0,4,"$exp")) // compare != 0
			name = ss + ta + "." + name;

		jobInfo.keyInfo->tupleKeyToName.push_back(name);
		jobInfo.keyInfo->keyName.insert(std::make_pair(newId, cn));
	}
	else
	{
		std::string name = ta;
		bool useAlias = true;
		if (name.empty())
		{
			name = tn;
			useAlias = false;
		}

		if (tn.compare(0,4,"$sub") && tn.compare(0,4,"$exp"))
		{
			if (!useAlias)
				name = ss + name;
		}
		else if (tn.compare(0,4,"$sub") == 0)
		{
			name = "sub-query";
		}
		else
		{
			name = "expression";
		}

		jobInfo.keyInfo->tupleKeyToName.push_back(name);
		jobInfo.keyInfo->keyName.insert(std::make_pair(newId, tn));
	}

	return newId;
}


// @brief Returns a suitably fudged column width
uint fudgeWidth(const CalpontSystemCatalog::ColType& ict, CalpontSystemCatalog::OID oid)
{
	CalpontSystemCatalog::OID dictOid = isDictCol(ict);
	CalpontSystemCatalog::ColType ct = ict;
	if (ct.colDataType != CalpontSystemCatalog::VARBINARY)
	{
		if (ct.colDataType == CalpontSystemCatalog::VARCHAR)
			ct.colWidth++;

		//Round colWidth up
		if (ct.colWidth == 3)
			ct.colWidth = 4;
		else if (ct.colWidth == 5 || ct.colWidth == 6 || ct.colWidth == 7)
			ct.colWidth = 8;
		else if (dictOid > 0 && oid != dictOid)  // token column
			ct.colWidth = 8;
	}
	else
	{
		ct.colWidth += 2;  // insert the length bytes
	}

	return ct.colWidth;
}


// @brief Set some tuple info
TupleInfo setTupleInfo_(CalpontSystemCatalog::ColType ct,
						CalpontSystemCatalog::OID col_oid,
						JobInfo& jobInfo,
						CalpontSystemCatalog::OID tbl_oid,
						const std::string& col_name,
						const std::string& col_alias,
						const std::string& sch_name,
						const std::string& tbl_name,
						const std::string& tbl_alias,
						const std::string& vw_name,
						bool correlated = false)
{
	// get the unique tupleOids for this column
	uint tbl_key = uniqTupleKey(jobInfo, tbl_oid, tbl_oid,
						"", "", sch_name, tbl_name, tbl_alias, vw_name, correlated);
	uint col_key = uniqTupleKey(jobInfo, col_oid, tbl_oid,
						col_name, col_alias, sch_name, tbl_name, tbl_alias, vw_name, correlated);
	TupleInfo ti = TupleInfo(fudgeWidth(ct, col_oid), col_oid, col_key, tbl_key,
								ct.scale, ct.precision, ct.colDataType);
	//If this is the first time we've seen this col, add it to the tim
	TupleInfoMap::iterator i1;
	TupleInfoMap::mapped_type v;
	i1 = jobInfo.keyInfo->tupleInfoMap.find(tbl_key);
	if (i1 != jobInfo.keyInfo->tupleInfoMap.end())
	{
		//We've seen the table, now look for the col
		v = i1->second;
		TupleInfoMap::mapped_type::iterator it2 = v.begin();
		TupleInfoMap::mapped_type::iterator end2 = v.end();;
		bool found = false;
		while (it2 != end2)
		{
			if (it2->oid == ti.oid)
			{
				found = true;
				ti = *it2;
				break;
			}
			++it2;
		}

		if (!found)
		{
			v.push_back(ti);
			i1->second = v;
			jobInfo.keyInfo->colKeyToTblKey[col_key] = tbl_key;
		}
	}
	else
	{
		//Haven't even seen the table yet, much less this col
		v.push_back(ti);
		jobInfo.keyInfo->tupleInfoMap[tbl_key] = v;
		jobInfo.keyInfo->colKeyToTblKey[col_key] = tbl_key;
	}

	return ti;
}


uint getTupleKey_(const JobInfo& jobInfo,
				CalpontSystemCatalog::OID oid,
				const std::string& tblAlias,
				const std::string& view,
				const std::string& colAlias,
				bool correlated = false)
{
	uint64_t subId = jobInfo.subId;
	if (correlated)
		subId = jobInfo.pSubId;

	std::string alias = tblAlias;
//	if (!colAlias.empty())
//		alias += "." + colAlias;
	UniqId id(oid, alias, view, subId);
	TupleKeyMap::const_iterator iter = jobInfo.keyInfo->tupleKeyMap.find(id);
	if (iter != jobInfo.keyInfo->tupleKeyMap.end())
		return iter->second;

	// dictionaryscan tableOid is 0 in the tuplehashjoin.
	if (oid != 0)
	{
		std::ostringstream strstm;
		strstm << "(" << oid << ", ";
		if (!alias.empty())
			strstm << alias;
		if (!view.empty())
			strstm << ", " << view;
		strstm << ") not found in tuple info map.";

		Message::Args args;
		args.add(strstm.str());
		jobInfo.logger->logMessage(LOG_TYPE_DEBUG, LogMakeJobList, args,
			LoggingID(5, jobInfo.sessionId, jobInfo.txnId, 0));
		std::cerr << strstm.str() << std::endl;

		throw std::logic_error("column is not found in info map.");
	}

	return static_cast<uint>(-1);
}
}


namespace joblist
{

//------------------------------------------------------------------------------
// Returns the table alias for the specified column
//------------------------------------------------------------------------------
std::string extractTableAlias(const SimpleColumn* sc)
{
	return  ba::to_lower_copy(sc->tableAlias());
}

//------------------------------------------------------------------------------
// Returns the table alias for the specified column
//------------------------------------------------------------------------------
std::string extractTableAlias(const SSC& sc)
{
	return  ba::to_lower_copy(sc->tableAlias());
}

//------------------------------------------------------------------------------
// Returns OID associated with colType if it is a dictionary column, else
// the value returned for the OID is 0.
//------------------------------------------------------------------------------
CalpontSystemCatalog::OID isDictCol(const CalpontSystemCatalog::ColType& colType)
{
	if (colType.colWidth > 8) return colType.ddn.dictOID;
	if (colType.colDataType == CalpontSystemCatalog::VARCHAR &&
		colType.colWidth > 7) return colType.ddn.dictOID;
	if (colType.colDataType == CalpontSystemCatalog::VARBINARY)
		return colType.ddn.dictOID;

	return 0;
}

//------------------------------------------------------------------------------
// Determines if colType is a character column
//------------------------------------------------------------------------------
bool isCharCol(const CalpontSystemCatalog::ColType& colType)
{
	switch (colType.colDataType)
	{
	case CalpontSystemCatalog::VARCHAR:
	case CalpontSystemCatalog::CHAR:
	case CalpontSystemCatalog::BLOB:
	case CalpontSystemCatalog::CLOB:
		return true;
		break;
	default:
		return false;
		break;
	}
	return false;
}

//------------------------------------------------------------------------------
// Returns OID associated with a table
//------------------------------------------------------------------------------
CalpontSystemCatalog::OID tableOid(const SimpleColumn* sc, CalpontSystemCatalog* cat)
{
	if (sc->schemaName().empty())
		return execplan::CNX_VTABLE_ID;

	CalpontSystemCatalog::ROPair p = cat->tableRID(make_table(sc->schemaName(),
		sc->tableName()));
	return p.objnum;
}

uint getTupleKey(const JobInfo& jobInfo,
				const execplan::SimpleColumn* sc)
{
	return getTupleKey_(jobInfo, sc->oid(), extractTableAlias(sc), sc->viewName(), sc->alias(),
						((sc->joinInfo() & execplan::JOIN_CORRELATED) != 0));
}

uint getTupleKey(const JobInfo& jobInfo, CalpontSystemCatalog::OID oid,
				 const std::string& tblAlias, const std::string& view,
				 const std::string& colAlias)
{
	return getTupleKey_(jobInfo, oid, tblAlias, view, colAlias);
}

uint getTupleKey(const JobInfo& jobInfo, CalpontSystemCatalog::OID oid,
				 const std::string& alias, const std::string& view)
{
	return getTupleKey_(jobInfo, oid, alias, view, "");
}

uint getTableKey(const JobInfo& jobInfo, CalpontSystemCatalog::OID oid,
				 const std::string& alias, const std::string& view)
{
	uint colKey = getTupleKey(jobInfo, oid, alias, view, "");
	return getTableKey(jobInfo, colKey);
}

uint getTableKey(const JobInfo& jobInfo, uint cid)
{
	return jobInfo.keyInfo->colKeyToTblKey[cid];
}

uint tableKey(JobInfo& jobInfo,
				CalpontSystemCatalog::OID o,
				const std::string& sn,
				const std::string& tn,
				const std::string& ta,
				const std::string& vn)
{
	return uniqTupleKey(jobInfo, o, o, "", "", sn, tn, ta, vn);
}

uint tableKey(JobInfo& jobInfo,
				CalpontSystemCatalog::OID o,
				const std::string& ta,
				const std::string& vn)
{
	return uniqTupleKey(jobInfo, o, o, "", "", "", "", ta, vn);
}

TupleInfo getTupleInfo(uint tableKey, uint columnKey, const JobInfo& jobInfo)
{
	TupleInfoMap::const_iterator cit = jobInfo.keyInfo->tupleInfoMap.find(tableKey);
	assert (cit != jobInfo.keyInfo->tupleInfoMap.end());
	TupleInfoMap::mapped_type mt = cit->second;
	TupleInfoMap::mapped_type::iterator mtit = mt.begin();
	while (mtit != mt.end())
	{
		if (mtit->key == columnKey)
			break;
		++mtit;
	}

	if (mtit == mt.end())
	{
		std::ostringstream strstm;
		strstm << "TupleInfo for (" << jobInfo.keyInfo->tupleKeyVec[columnKey].fId << ","
			<< jobInfo.keyInfo->tupleKeyVec[columnKey].fAlias;
		if (jobInfo.keyInfo->tupleKeyVec[columnKey].fView.length() > 0)
			strstm << "," << jobInfo.keyInfo->tupleKeyVec[columnKey].fView;
		strstm << ") could not be found." << std::endl;
		std::cerr << strstm;

		Message::Args args;
		args.add(strstm.str());
		jobInfo.logger->logMessage(LOG_TYPE_DEBUG, LogMakeJobList, args,
			LoggingID(5, jobInfo.sessionId, jobInfo.txnId, 0));

		throw std::runtime_error("column's tuple info could not be found");
	}

	return *mtit;
}

TupleInfo setTupleInfo(execplan::CalpontSystemCatalog::ColType ct,
	execplan::CalpontSystemCatalog::OID col_oid,
	JobInfo& jobInfo,
	execplan::CalpontSystemCatalog::OID tbl_oid,
	const execplan::SimpleColumn* sc,
	const std::string& alias)
{
	return setTupleInfo_(ct, col_oid, jobInfo, tbl_oid, sc->columnName(), sc->alias(),
							sc->schemaName(), sc->tableName(), alias, sc->viewName(),
							((sc->joinInfo() & execplan::JOIN_CORRELATED) != 0));
}

TupleInfo setExpTupleInfo(execplan::CalpontSystemCatalog::ColType ct, uint64_t expressionId,
	const std::string& alias, JobInfo& jobInfo)
{
	// pretend all expressions belong to "virtual" table EXPRESSION, (3000, expression)
	// OID 3000 is not for user table or column, there will be no confilict in queries.
	std::stringstream ss;
	ss << "$exp";
	if (jobInfo.subLevel > 0)
		ss << "_" << jobInfo.subId << "_" << jobInfo.subLevel << "_" << jobInfo.subNum; 
	return setTupleInfo_(ct, expressionId, jobInfo, 3000, alias, alias, "", "$exp", ss.str(), "");
}

uint getExpTupleKey(const JobInfo& jobInfo, uint64_t eid)
{
	std::stringstream ss;
	ss << "$exp";
	if (jobInfo.subLevel > 0)
		ss << "_" << jobInfo.subId << "_" << jobInfo.subLevel << "_" << jobInfo.subNum; 
	return getTupleKey(jobInfo, eid, ss.str(), "", "");
}

TupleInfo getExpTupleInfo(uint expKey, const JobInfo& jobInfo)
{
	// first retrieve the expression virtual table key
	uint evtKey = getExpTupleKey(jobInfo, 3000);
	return getTupleInfo(evtKey, expKey, jobInfo);
}

void addAggregateColumn(AggregateColumn* agc, int idx, RetColsVector& vec, JobInfo& jobInfo)
{
	uint eid = agc->expressionId();
	setExpTupleInfo(agc->resultType(), eid, agc->alias(), jobInfo);

	std::vector<std::pair<int, int> >::iterator i;
	for (i = jobInfo.aggEidIndexList.begin(); i != jobInfo.aggEidIndexList.end(); ++i)
	{
		if (i->first == (int) eid)
			break;
	}

	if (idx < 0 && i != jobInfo.aggEidIndexList.end())
	{
		agc->inputIndex(i->second);
		jobInfo.cloneAggregateColMap.insert(std::make_pair(vec[i->second].get(), agc));
	}
	else
	{
		SRCP srcp;
		if (idx < 0)
		{
			srcp.reset(agc->clone());
			idx = vec.size();
			vec.push_back(srcp);
		}
		else
		{
			srcp = vec[idx];
		}

		jobInfo.aggEidIndexList.push_back(std::make_pair(eid, idx));
		agc->inputIndex(idx);
		jobInfo.cloneAggregateColMap.insert(std::make_pair(srcp.get(), agc));
	}
}


bool operator < (const struct UniqId& x, const struct UniqId& y)
{
	return ((x.fId < y.fId) ||
			(x.fId == y.fId && x.fAlias < y.fAlias) ||
			(x.fId == y.fId && x.fAlias == y.fAlias && x.fView < y.fView) ||
			(x.fId == y.fId && x.fAlias == y.fAlias && x.fView == y.fView && x.fSubId < y.fSubId));
}


bool operator == (const struct UniqId& x, const struct UniqId& y)
{
	return (x.fId == y.fId && x.fAlias == y.fAlias && x.fView == y.fView && x.fSubId == y.fSubId);
}


void updateDerivedColumn(JobInfo& jobInfo, SimpleColumn* sc, CalpontSystemCatalog::ColType& ct)
{
	sc->oid(tableOid(sc, jobInfo.csc) + 1 + sc->colPosition());

	std::map<UniqId, execplan::CalpontSystemCatalog::ColType>::iterator i =
		jobInfo.vtableColTypes.find(UniqId(sc->oid(), extractTableAlias(sc), sc->viewName()));
	if (i != jobInfo.vtableColTypes.end())
		ct = i->second;
}


bool filterWithDictionary(execplan::CalpontSystemCatalog::OID dictOid, uint64_t n)
{
	// if n == 0, no dictionary scan, alway filter with dictionary.
	if (n == 0)
		return true;

	// if n == ulong_max, always use dictionary scan
	if (n == ULONG_MAX)
		return false;

	std::vector<struct EMEntry> entries;
	DBRM dbrm;
	if (dbrm.getExtents(dictOid, entries) != 0)
		return false;  // Just do pdictionaryscan and let job step handle this.

	std::vector<struct EMEntry>::iterator it = entries.begin();
	bool ret = false;
	n--;  // HWM starts at 0
	while (it != entries.end())
	{
		if (it->HWM > n)
		{
			ret = true;
			break;
		}

		it++;
	}

	return ret;
}


}
// vim:ts=4 sw=4:

