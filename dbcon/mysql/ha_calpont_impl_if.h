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

// $Id: ha_calpont_impl_if.h 8326 2012-02-15 18:58:10Z xlou $
/** @file */
#ifndef HA_CALPONT_IMPL_IF_H__
#define HA_CALPONT_IMPL_IF_H__
#include <string>
#include <stdint.h>
#ifdef _MSC_VER
#include <unordered_map>
#else
#include <tr1/unordered_map>
#endif
#include <iosfwd>
#include <boost/shared_ptr.hpp>
#include <stack>
#include <vector>

#include "idb_mysql.h"

struct st_table;
struct st_ha_create_information;

#include "idberrorinfo.h"
#include "calpontselectexecutionplan.h"
#include "sm.h"

/** Debug macro */
#if INFINIDB_DEBUG
#define IDEBUG(x) {x;}
#else
#define IDEBUG(x) {}
#endif

namespace execplan
{
	class ReturnedColumn;
	class SimpleColumn;
	class SimpleFilter;
	class AggregateColumn;
	class FunctionColumn;
}

namespace cal_impl_if
{
class SubQuery;

struct JoinInfo
{
	execplan::CalpontSystemCatalog::TableAliasName tn;
	uint joinTimes;
	std::vector<uint> IDs;	
};

enum ClauseType
{
	INIT = 0,
	SELECT,
	FROM,
	WHERE,
	HAVING,
	GROUP_BY,
	ORDER_BY
};

typedef std::vector<JoinInfo> JoinInfoVec;
typedef std::map<execplan::CalpontSystemCatalog::TableAliasName, int> TableMap;

struct gp_walk_info
{
	std::vector <std::string> selectCols;
	execplan::CalpontSelectExecutionPlan::ReturnedColumnList returnedCols;
	execplan::CalpontSelectExecutionPlan::ReturnedColumnList groupByCols;
	execplan::CalpontSelectExecutionPlan::ReturnedColumnList orderByCols;
	execplan::CalpontSelectExecutionPlan::ColumnMap columnMap;
	std::stack<execplan::ReturnedColumn*> rcWorkStack;
	std::stack<execplan::ParseTree*> ptWorkStack;
	boost::shared_ptr<execplan::SimpleColumn> scsp;
	uint32_t sessionid;
	bool fatalParseError;
	std::string parseErrorText;
	// for outer join walk. the column that is not of the outerTable has the returnAll flag set.
	std::set<execplan::CalpontSystemCatalog::TableAliasName> innerTables;
	// the followinig members are used for table mode
	bool condPush;
	bool dropCond;
	std::string funcName;
	uint expressionId; // for F&E
	std::vector<execplan::AggregateColumn*> count_asterisk_list;
	std::vector<execplan::FunctionColumn*> no_parm_func_list;
	TableMap tableMap;
	execplan::CalpontSystemCatalog *csc;
	int8_t internalDecimalScale;
	THD* thd;
	uint64_t subSelectType; // the type of sub select filter that owns the gwi
	SubQuery* subQuery;
	execplan::CalpontSelectExecutionPlan::SelectList derivedTbList;
	execplan::CalpontSelectExecutionPlan::TableList tbList;
	std::vector<execplan::CalpontSystemCatalog::TableAliasName> correlatedTbNameVec;
	std::vector<execplan::CalpontSystemCatalog::TableAliasName> viewList;
	ClauseType clauseType;
	execplan::CalpontSystemCatalog::TableAliasName viewName;


	gp_walk_info() : sessionid(0), 
		               fatalParseError(false), 
		               condPush(false), 
		               dropCond(false), 
		               expressionId(0), 
		               csc(0), 
		               internalDecimalScale(4),
		               thd(0),
		               subSelectType(uint64_t(-1)),
		               subQuery(0),
		               clauseType(INIT)
	{ }
};

struct cal_table_info
{
	enum RowSources { FROM_ENGINE, FROM_FILE };

	cal_table_info() : tpl_ctx(0), 
		                 tpl_scan_ctx(0), 
		                 c(0), 
		                 msTablePtr(0), 
		                 conn_hndl(0),
		                 condInfo(0),
		                 csep(0),
		                 moreRows(false)
	{ }
	sm::cpsm_tplh_t* tpl_ctx;
	sm::cpsm_tplsch_t* tpl_scan_ctx;
	unsigned c;	// for debug purpose
	st_table* msTablePtr;
	sm::cpsm_conhdl_t* conn_hndl; 
	gp_walk_info* condInfo;
	execplan::CalpontSelectExecutionPlan* csep;
	bool moreRows; //are there more rows to consume (b/c of limit)
};

typedef std::tr1::unordered_map<TABLE*, cal_table_info> CalTableMap;
typedef std::vector<std::string> ColValuesList;
typedef std::vector<std::string> ColNameList;
typedef std::map<uint32_t, ColValuesList> TableValuesMap;
struct cal_connection_info
{
	enum AlterTableState { NOT_ALTER, ALTER_SECOND_RENAME, ALTER_FIRST_RENAME };
	cal_connection_info() : cal_conn_hndl(0), queryState(0), currentTable(0), traceFlags(0), alterTableState(NOT_ALTER), isAlter(false), 
	bulkInsertRows(0), singleInsert(true), isLoaddataInfile( false ), dmlProc(0), rowsHaveInserted(0), rc(0) 
	{ }

	sm::cpsm_conhdl_t* cal_conn_hndl;
	int queryState;
	CalTableMap tableMap;
	sm::tableid_t currentTable;
	uint32_t traceFlags;
	std::string queryStats;
	AlterTableState alterTableState;
	bool isAlter;
	ha_rows  bulkInsertRows;
	bool singleInsert;
	bool isLoaddataInfile;
	std::string extendedStats;
	std::string miniStats;
	messageqcpp::MessageQueueClient* dmlProc;
	ha_rows rowsHaveInserted;
	ColNameList colNameList;
	TableValuesMap tableValuesMap;
	int rc;
};

typedef std::tr1::unordered_map<int, cal_connection_info> CalConnMap;

const std::string infinidb_err_msg = "\nThe query includes syntax that is not supported by InfiniDB. Use 'show warnings;' to get more information. Review the Calpont InfiniDB Syntax guide for additional information on supported distributed syntax or consider changing the InfiniDB Operating Mode (infinidb_vtable_mode).";

int cp_get_plan(THD* thd, execplan::CalpontSelectExecutionPlan& csep);
int cp_get_table_plan(THD* thd, execplan::CalpontSelectExecutionPlan& csep, cal_impl_if::cal_table_info& ti);
int getSelectPlan(gp_walk_info& gwi, SELECT_LEX& select_lex, execplan::CalpontSelectExecutionPlan& csep, bool isUnion=false);
void setError(THD* thd, uint errcode, const std::string errmsg);
void gp_walk(const Item *item, void *arg);
void parse_item (Item *item, std::vector<Item_field*>& field_vec, bool& hasNonSupportItem, uint16& parseInfo);
execplan::ReturnedColumn* buildReturnedColumn(Item* item, gp_walk_info& gwi, bool& nonSupport);
const std::string bestTableName(const Item_field* ifp);
#ifdef DEBUG_WALK_COND
void debug_walk(const Item *item, void *arg);
#endif
}

#endif

