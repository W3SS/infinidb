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

/*
 * $Id: ha_calpont_ddl.cpp 9096 2012-11-19 19:34:54Z rdempsey $
 */

#include <string>
#include <iostream>
#include <stack>
#ifdef _MSC_VER
#include <unordered_map>
#else
#include <tr1/unordered_map>
#endif
#include <fstream>
#include <sstream>
#include <cerrno>
#include <cstring>
#ifdef _MSC_VER
#include <unordered_set>
#else
#include <tr1/unordered_set>
#endif
#include <utility>
//#define NDEBUG
#include <cassert>
using namespace std;

#include <boost/shared_ptr.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/regex.hpp>
#include <boost/tokenizer.hpp>
using namespace boost;

#include "idb_mysql.h"

#include "ha_calpont_impl_if.h"
using namespace cal_impl_if;

#include "ddlpkg.h"
#include "sqlparser.h"
using namespace ddlpackage;

#include "ddlpackageprocessor.h"
using namespace ddlpackageprocessor;

#include "dataconvert.h"
using namespace dataconvert;

#include "bytestream.h"
using namespace messageqcpp;

#include "configcpp.h"
using namespace config;

#include "idbcompress.h"
using namespace compress;

#include "idberrorinfo.h"
#include "errorids.h"
using namespace logging;

#include "dbrm.h"
using namespace BRM;

#include "calpontsystemcatalog.h"
using namespace execplan;

namespace
{
const int MAX_INT = numeric_limits<int32_t>::max();
const short MAX_TINYINT = numeric_limits<int8_t>::max(); //127;
const short MAX_SMALLINT = numeric_limits<int16_t>::max(); //32767;
const long long MAX_BIGINT = numeric_limits<int64_t>::max();//9223372036854775807LL

#ifndef SKIP_AUTOI
#include "ha_autoi.cpp"
#endif

//convenience fcn
inline uint32_t tid2sid(const uint32_t tid)
{
	return CalpontSystemCatalog::idb_tid2sid(tid);
}

int parseCompressionComment ( std::string comment )
{
	algorithm::to_upper(comment);
	regex compat("[[:space:]]*COMPRESSION[[:space:]]*=[[:space:]]*", regex_constants::extended);
	int compressiontype = 0;
	boost::match_results<std::string::const_iterator> what;
	std::string::const_iterator start, end;
	start = comment.begin();
	end = comment.end();   
	boost::match_flag_type flags = boost::match_default;
	if (boost::regex_search(start, end, what, compat, flags)) 
	{
		//Find the pattern, now get the compression type
		string compType (&(*(what[0].second)));
		//; is the seperator between compression and autoincrement comments.
		unsigned i = compType.find_first_of(";");
		if ( i <= compType.length() )
		{
			compType = compType.substr( 0,i);
		}
		i = compType.find_last_not_of(" ");
		if ( i <= compType.length() )
		{
			compType = compType.substr( 0,i+1);
		}

		errno = 0;
		char *ep = NULL;
		const char *str = compType.c_str();
		compressiontype = strtoll(str, &ep, 10);
		//  (no digits) || (more chars)  || (other errors & value = 0)
		if ((ep == str) || (*ep != '\0') || (errno != 0 && compressiontype == 0))
		{
			compressiontype = -1;
		}
		
	}
	else
		compressiontype = MAX_INT;
	return compressiontype;
}


bool validateAutoincrementDatatype ( int type )
{
	bool validAutoType = false;
	switch (type)
	{
		case ddlpackage::DDL_INT:
		case ddlpackage::DDL_INTEGER:
		case ddlpackage::DDL_BIGINT:
		case ddlpackage::DDL_MEDINT:
		case ddlpackage::DDL_SMALLINT:
		case ddlpackage::DDL_TINYINT:
			validAutoType = true;
			break;
	}
	return validAutoType;
}

bool validateNextValue( int type, int64_t value )
{
	bool validValue = true;
	switch (type)
	{
		case ddlpackage::DDL_BIGINT:
			{
				if (value > MAX_BIGINT)
					validValue = false;
			}
			break;
		case ddlpackage::DDL_INT:
		case ddlpackage::DDL_INTEGER:
		case ddlpackage::DDL_MEDINT:
			{
				if (value > MAX_INT)
					validValue = false;
			}
			break;
		case ddlpackage::DDL_SMALLINT:
			{
				if (value > MAX_SMALLINT)
					validValue = false;
			}
			break;
		case ddlpackage::DDL_TINYINT:
			{
				if (value > MAX_TINYINT)
					validValue = false;
			}
			break;
	}
	return validValue;
}

int ProcessDDLStatement(string& ddlStatement, string& schema, const string& table, int sessionID,
	 string& emsg, int compressionTypeIn = 0, bool isAnyAutoincreCol = false, int64_t nextvalue = 1, std::string autoiColName = "")
{
  SqlParser parser;
  THD *thd = current_thd;
  
  parser.setDefaultSchema(schema);
  int rc = 0;
  IDBCompressInterface idbCompress;
  parser.Parse(ddlStatement.c_str());
  cal_connection_info* ci = reinterpret_cast<cal_connection_info*>(thd->infinidb_vtable.cal_conn_info);
  if(parser.Good())
  {
    const ddlpackage::ParseTree &ptree = parser.GetParseTree();
    SqlStatement &stmt = *ptree.fList[0];
	bool isVarbinaryAllowed = false;
	std::string valConfig = config::Config::makeConfig()->getConfig(
			"WriteEngine", "AllowVarbinary" );
	algorithm::to_upper(valConfig);
	if (valConfig.compare("YES") == 0)
		isVarbinaryAllowed = true;

    //@Bug 1771. error out for not supported feature.
    if ( typeid ( stmt ) == typeid ( CreateTableStatement ) )
    {
    	CreateTableStatement * createTable = dynamic_cast <CreateTableStatement *> ( &stmt );
    	bool matchedCol = false;
    	for ( unsigned i=0; i < createTable->fTableDef->fColumns.size(); i++ )
    	{
			// if there are any constraints other than 'DEFAULT NULL' (which is the default in IDB), kill
			//  the statement
			bool autoIncre = false;
			int64_t startValue = 1;
    		if (createTable->fTableDef->fColumns[i]->fConstraints.size() > 0 ||
				(createTable->fTableDef->fColumns[i]->fDefaultValue &&
					!createTable->fTableDef->fColumns[i]->fDefaultValue->fNull))
    		{
    			rc = 1;
  	 			thd->main_da.can_overwrite_status = true;

	 			thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CONSTRAINTS)).c_str());
				ci->alterTableState = cal_connection_info::NOT_ALTER;
				ci->isAlter = false;
	 			return rc;	
    		}
			
			//check varbinary data type
			if ((createTable->fTableDef->fColumns[i]->fType->fType == ddlpackage::DDL_VARBINARY) && !isVarbinaryAllowed)
			{
				rc = 1;
  	 			thd->main_da.can_overwrite_status = true;

	 			thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Varbinary is currently not supported by InfiniDB.");
				ci->alterTableState = cal_connection_info::NOT_ALTER;
				ci->isAlter = false;
	 			return rc;	
			}
			
			if ((createTable->fTableDef->fColumns[i]->fType->fType == ddlpackage::DDL_VARBINARY) && 
			((createTable->fTableDef->fColumns[i]->fType->fLength > 8000) || (createTable->fTableDef->fColumns[i]->fType->fLength < 8)))
			{
				rc = 1;
  	 			thd->main_da.can_overwrite_status = true;

	 			thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Varbinary length has to be between 8 and 8000.");
				ci->alterTableState = cal_connection_info::NOT_ALTER;
				ci->isAlter = false;
	 			return rc;	
			}
			//Parse the column comment
			string comment = createTable->fTableDef->fColumns[i]->fComment;
			int compressionType = compressionTypeIn;
			if ( comment.length() > 0 )
			{
				compressionType = parseCompressionComment( comment);
				if ( compressionType == MAX_INT )
				{
					compressionType = compressionTypeIn;
				}
				else if ( compressionType < 0 )
				{
					rc = 1;
					thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_COMPRESSION_TYPE)).c_str());
					ci->alterTableState = cal_connection_info::NOT_ALTER;
					ci->isAlter = false;
					return rc;
				}
				if (compressionType == 1) compressionType = 2;
				if (( compressionType > 0 ) && !(idbCompress.isCompressionAvail( compressionType )))
				{
					rc = 1;
					ci->alterTableState = cal_connection_info::NOT_ALTER;
					ci->isAlter = false;
#ifdef SKIP_IDB_COMPRESSION
		Message::Args args;
		thd->main_da.can_overwrite_status = true;
		args.add("The compression type");
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_ENTERPRISE_ONLY, args)).c_str());
#else
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_COMPRESSION_TYPE)).c_str());
#endif
					return rc;
				}
				
				try {
#ifndef SKIP_AUTOI
	autoIncre = parseAutoincrementColumnComment(comment, startValue);
#else
	algorithm::to_upper(comment);
	if ( comment.find("AUTOINCREMENT") != string::npos )
	{
		int rc = 1;
		thd->main_da.can_overwrite_status = true;
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CREATE_AUTOINCREMENT_NOT_SUPPORT)).c_str());
		ci->alterTableState = cal_connection_info::NOT_ALTER;
		ci->isAlter = false;
		return rc;
	}
#endif
	
					if (autoIncre)
					{
						//Check whether there is a column with autoincrement already
						if ((isAnyAutoincreCol) && !(boost::iequals(autoiColName, createTable->fTableDef->fColumns[i]->fName)))
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_NUMBER_AUTOINCREMENT)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						else
						{
							isAnyAutoincreCol = true;
							autoiColName = createTable->fTableDef->fColumns[i]->fName;
							matchedCol = true;
						}
					}
				}
				catch (runtime_error& ex)
				{
					rc = 1;
					thd->main_da.can_overwrite_status = true;
					thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, ex.what());
					ci->alterTableState = cal_connection_info::NOT_ALTER;
					ci->isAlter = false;
					return rc;
				}
				
			}
			if (!autoIncre &&  isAnyAutoincreCol && (boost::iequals(autoiColName, createTable->fTableDef->fColumns[i]->fName)))
			{
				autoIncre = true;
				matchedCol = true;
				startValue = nextvalue;
			}
			
			if (startValue <= 0)
			{
				rc = 1;
				thd->main_da.can_overwrite_status = true;
				thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_NEGATIVE_STARTVALUE)).c_str());
				ci->alterTableState = cal_connection_info::NOT_ALTER;
				ci->isAlter = false;
				return rc;
			}
		
			if(autoIncre)
            {
                    if (!validateAutoincrementDatatype(createTable->fTableDef->fColumns[i]->fType->fType))
                    {
                        rc = 1;
                        thd->main_da.can_overwrite_status = true;
                        thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_AUTOINCREMENT_TYPE)).c_str());
						ci->alterTableState = cal_connection_info::NOT_ALTER;
						ci->isAlter = false;
                        return rc;
                    }
             }
	
			if (!validateNextValue(createTable->fTableDef->fColumns[i]->fType->fType, startValue))
			{
				rc = 1;
				thd->main_da.can_overwrite_status = true;
				thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_STARTVALUE)).c_str());
				ci->alterTableState = cal_connection_info::NOT_ALTER;
				ci->isAlter = false;
				return rc;
			}
			(createTable->fTableDef->fColumns[i]->fType)->fCompressiontype = compressionType;
			if (autoIncre)
				(createTable->fTableDef->fColumns[i]->fType)->fAutoincrement = "y";
			else
				(createTable->fTableDef->fColumns[i]->fType)->fAutoincrement = "n";
				
			(createTable->fTableDef->fColumns[i]->fType)->fNextvalue = startValue;
		} 

		if (isAnyAutoincreCol && !matchedCol) //@Bug 3555 error out on invalid column
		{
			rc = 1;
			Message::Args args;
			thd->main_da.can_overwrite_status = true;
			args.add(autoiColName);
			thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED,(IDBErrorInfo::instance()->errorMsg(ERR_UNKNOWN_COL, args)).c_str());
			ci->alterTableState = cal_connection_info::NOT_ALTER;
			ci->isAlter = false;
			return rc;
		}
    }
    else if ( typeid ( stmt ) == typeid ( AlterTableStatement ) )
    {
    	AlterTableStatement * alterTable = dynamic_cast <AlterTableStatement *> ( &stmt );	
		if ( schema.length() == 0 )
		{
			schema = alterTable->fTableName->fSchema;
			if ( schema.length() == 0 )
			{
				rc = 1;
  	 			thd->main_da.can_overwrite_status = true;

	 			thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "No database selected.");
				ci->alterTableState = cal_connection_info::NOT_ALTER;
				ci->isAlter = false;
	 			return rc;	
			}
		}

    	ddlpackage::AlterTableActionList actionList = alterTable->fActions;
		if (actionList.size() > 1) //@bug 3753 we don't support multiple actions in alter table statement
		{
			rc = 1;
			thd->main_da.can_overwrite_status = true;

			thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Multiple actions in alter table statement is currently not supported by InfiniDB.");
	 		
			ci->alterTableState = cal_connection_info::NOT_ALTER;
			ci->isAlter = false;
			return rc;	
		}

    	for ( unsigned i=0; i < actionList.size(); i++ )
    	{
    		if ( ddlpackage::AtaAddColumn *addColumnPtr = dynamic_cast<AtaAddColumn*> (actionList[i]) )
    		{
				//check varbinary data type
				if ((addColumnPtr->fColumnDef->fType->fType == ddlpackage::DDL_VARBINARY) && !isVarbinaryAllowed)
				{
					rc = 1;
					thd->main_da.can_overwrite_status = true;
					thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Varbinary is currently not supported by InfiniDB.");
					ci->alterTableState = cal_connection_info::NOT_ALTER;
					ci->isAlter = false;
					return rc;	
				}
				
				if ((addColumnPtr->fColumnDef->fType->fType == ddlpackage::DDL_VARBINARY) && 
				((addColumnPtr->fColumnDef->fType->fLength > 8000) || (addColumnPtr->fColumnDef->fType->fLength < 8)))
				{
					rc = 1;
					thd->main_da.can_overwrite_status = true;
					thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Varbinary length has to be between 8 and 8000.");
					ci->alterTableState = cal_connection_info::NOT_ALTER; 	
					ci->isAlter = false;					
					return rc;	
				}
				int64_t startValue = 1;
				bool autoIncre  = false;
				if ( (addColumnPtr->fColumnDef->fConstraints.size() > 0 ) || addColumnPtr->fColumnDef->fDefaultValue )
				{
					rc = 1;
					thd->main_da.can_overwrite_status = true;
					thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CONSTRAINTS)).c_str());
					ci->alterTableState = cal_connection_info::NOT_ALTER;
					ci->isAlter = false;
					return rc;	
				}
				//Handle compression type
				string comment = addColumnPtr->fColumnDef->fComment;
				int compressionType = compressionTypeIn;
				if ( comment.length() > 0 )
				{
					//@Bug 3782 This is for synchronization after calonlinealter to use 
					algorithm::to_upper(comment);
					regex pat("[[:space:]]*SCHEMA[[:space:]]+SYNC[[:space:]]+ONLY", regex_constants::extended);
					if (regex_search(comment, pat))
					{
						return 0;
					}
					compressionType = parseCompressionComment( comment);
					if ( compressionType == MAX_INT )
					{
						compressionType = compressionTypeIn;
					}
					else if ( compressionType < 0 )
					{
						rc = 1;
						thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_COMPRESSION_TYPE)).c_str());
						ci->alterTableState = cal_connection_info::NOT_ALTER;
						ci->isAlter = false;
						return rc;
					}
					
					if (( compressionType > 0 ) && !(idbCompress.isCompressionAvail( compressionType )))
					{
						rc = 1;
#ifdef SKIP_IDB_COMPRESSION
		Message::Args args;
		thd->main_da.can_overwrite_status = true;
		args.add("The compression type");
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_ENTERPRISE_ONLY, args)).c_str());
#else
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_COMPRESSION_TYPE)).c_str());
#endif
						ci->alterTableState = cal_connection_info::NOT_ALTER;
						ci->isAlter = false;
						return rc;
					}
					
					try {
#ifndef SKIP_AUTOI
	autoIncre = parseAutoincrementColumnComment(comment, startValue);
#else
	algorithm::to_upper(comment);
	if ( comment.find("AUTOINCREMENT") != string::npos )
	{
		int rc = 1;
		thd->main_da.can_overwrite_status = true;
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CREATE_AUTOINCREMENT_NOT_SUPPORT)).c_str());
		ci->alterTableState = cal_connection_info::NOT_ALTER;
		ci->isAlter = false;
		return rc;
	}
#endif

					}
					catch (runtime_error& ex)
					{
						rc = 1;
						thd->main_da.can_overwrite_status = true;
						thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, ex.what());
						ci->alterTableState = cal_connection_info::NOT_ALTER;
						ci->isAlter = false;
						return rc;
					}
					if (autoIncre)
					{
						//Check if the table already has autoincrement column
						CalpontSystemCatalog* csc = CalpontSystemCatalog::makeCalpontSystemCatalog(sessionID);
						CalpontSystemCatalog::TableName tableName;
						tableName.schema = alterTable->fTableName->fSchema;
						tableName.table = alterTable->fTableName->fName;
						CalpontSystemCatalog::TableInfo tblInfo;
						try {
							tblInfo = csc->tableInfo(tableName);	
						}
						catch (runtime_error& ex)
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, ex.what());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						
						if (tblInfo.tablewithautoincr == 1)
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_NUMBER_AUTOINCREMENT)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						
						if (!validateAutoincrementDatatype(addColumnPtr->fColumnDef->fType->fType))
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_AUTOINCREMENT_TYPE)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						if (!validateNextValue(addColumnPtr->fColumnDef->fType->fType, startValue))
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_STARTVALUE)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						
						if (startValue <= 0)
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_NEGATIVE_STARTVALUE)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}		
					}
				}
				addColumnPtr->fColumnDef->fType->fCompressiontype = compressionType;
				if (autoIncre)
					addColumnPtr->fColumnDef->fType->fAutoincrement = "y";
				else
					addColumnPtr->fColumnDef->fType->fAutoincrement = "n";
				
				addColumnPtr->fColumnDef->fType->fNextvalue = startValue;
				
    		}
    		else if ( dynamic_cast<AtaAddTableConstraint*> (actionList[i]))
    		{
    			rc = 1;
  	 			thd->main_da.can_overwrite_status = true;

	 			thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CONSTRAINTS)).c_str());
				ci->alterTableState = cal_connection_info::NOT_ALTER;
				ci->isAlter = false;
	 			return rc;
    		}
    		else if ( dynamic_cast<AtaSetColumnDefault*> (actionList[i]) )
    		{
    			rc = 1;
  	 			thd->main_da.can_overwrite_status = true;
	 			thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CONSTRAINTS)).c_str());
				ci->alterTableState = cal_connection_info::NOT_ALTER;
				ci->isAlter = false;
	 			return rc;
    		}  
			else if ( ddlpackage::AtaAddColumns *addColumnsPtr = dynamic_cast<AtaAddColumns*>(actionList[i]))
			{
				if ((addColumnsPtr->fColumns).size() > 1)
				{
					rc = 1;
					thd->main_da.can_overwrite_status = true;
					thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Multiple actions in alter table statement is currently not supported by InfiniDB.");
					ci->alterTableState = cal_connection_info::NOT_ALTER;
					ci->isAlter = false;
					return rc;
				}
				
				//check varbinary data type
				if ((addColumnsPtr->fColumns[0]->fType->fType == ddlpackage::DDL_VARBINARY) && !isVarbinaryAllowed)
				{
					rc = 1;
					thd->main_da.can_overwrite_status = true;
					thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Varbinary is currently not supported by InfiniDB.");
					ci->alterTableState = cal_connection_info::NOT_ALTER;
					ci->isAlter = false;
					return rc;	
				}
				
				
				if ((addColumnsPtr->fColumns[0]->fType->fType == ddlpackage::DDL_VARBINARY) && 
				((addColumnsPtr->fColumns[0]->fType->fLength > 8000) || (addColumnsPtr->fColumns[0]->fType->fLength < 8)))
				{
					rc = 1;
					thd->main_da.can_overwrite_status = true;
					thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Varbinary length has to be between 8 and 8000.");
					ci->alterTableState = cal_connection_info::NOT_ALTER;
					ci->isAlter = false;
					return rc;	
				}
				int64_t startValue = 1;
				bool autoIncre  = false;
    			if ( (addColumnsPtr->fColumns[0]->fConstraints.size() > 0 ) || addColumnsPtr->fColumns[0]->fDefaultValue )
    			{
    				rc = 1;
  	 				thd->main_da.can_overwrite_status = true;
	 				thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CONSTRAINTS)).c_str());
					ci->alterTableState = cal_connection_info::NOT_ALTER;
					ci->isAlter = false;
	 				return rc;	
    			}
				//Handle compression type
				string comment = addColumnsPtr->fColumns[0]->fComment;
				int compressionType = compressionTypeIn;
				if ( comment.length() > 0 )
				{
					compressionType = parseCompressionComment( comment);
					if ( compressionType == MAX_INT )
					{
						compressionType = compressionTypeIn;
					}
					else if ( compressionType < 0 )
					{
						rc = 1;
						thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_COMPRESSION_TYPE)).c_str());
						ci->alterTableState = cal_connection_info::NOT_ALTER;
						ci->isAlter = false;
						return rc;
					}
					
					if (( compressionType > 0 ) && !(idbCompress.isCompressionAvail( compressionType )))
					{
						rc = 1;
#ifdef SKIP_IDB_COMPRESSION
		Message::Args args;
		thd->main_da.can_overwrite_status = true;
		args.add("The compression type");
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_ENTERPRISE_ONLY, args)).c_str());
#else
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_COMPRESSION_TYPE)).c_str());
#endif
						ci->alterTableState = cal_connection_info::NOT_ALTER;
						ci->isAlter = false;
						return rc;
					}
					
					try {
#ifndef SKIP_AUTOI
	autoIncre = parseAutoincrementColumnComment(comment, startValue);
#else
	algorithm::to_upper(comment);
	if ( comment.find("AUTOINCREMENT") != string::npos )
	{
		int rc = 1;
		thd->main_da.can_overwrite_status = true;
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CREATE_AUTOINCREMENT_NOT_SUPPORT)).c_str());
		ci->alterTableState = cal_connection_info::NOT_ALTER;
		ci->isAlter = false;
		return rc;
	}
#endif

					}
					catch (runtime_error& ex)
					{
						rc = 1;
						thd->main_da.can_overwrite_status = true;
						thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, ex.what());
						ci->alterTableState = cal_connection_info::NOT_ALTER;
						ci->isAlter = false;
						return rc;
					}
					if (autoIncre)
					{
						//Check if the table already has autoincrement column
						CalpontSystemCatalog* csc = CalpontSystemCatalog::makeCalpontSystemCatalog(sessionID);
						CalpontSystemCatalog::TableName tableName;
						tableName.schema = alterTable->fTableName->fSchema;
						tableName.table = alterTable->fTableName->fName;
						CalpontSystemCatalog::TableInfo tblInfo;
						try {
							tblInfo = csc->tableInfo(tableName);	
						}
						catch (runtime_error& ex)
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, ex.what());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						
						if (tblInfo.tablewithautoincr == 1)
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_NUMBER_AUTOINCREMENT)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						
						if (!validateAutoincrementDatatype(addColumnsPtr->fColumns[0]->fType->fType))
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_AUTOINCREMENT_TYPE)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						if (!validateNextValue(addColumnsPtr->fColumns[0]->fType->fType, startValue))
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_STARTVALUE)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						if (startValue <= 0)
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_NEGATIVE_STARTVALUE)).c_str());
							return rc;
						}					
					}
				}
				addColumnsPtr->fColumns[0]->fType->fCompressiontype = compressionType;
				if (autoIncre)
					addColumnsPtr->fColumns[0]->fType->fAutoincrement = "y";
				else
					addColumnsPtr->fColumns[0]->fType->fAutoincrement = "n";
				
				addColumnsPtr->fColumns[0]->fType->fNextvalue = startValue;
			}
			else if (ddlpackage::AtaRenameColumn* renameColumnsPtr = dynamic_cast<AtaRenameColumn*>(actionList[i]))
			{
				//cout << "Rename a column" << endl;
				int64_t startValue = 1;
				bool autoIncre  = false;
    			//@Bug 3746 Handle compression type
				string comment = renameColumnsPtr->fComment;
				int compressionType = compressionTypeIn;
				if ( comment.length() > 0 )
				{
					compressionType = parseCompressionComment( comment);
					if ( compressionType == MAX_INT )
					{
						compressionType = compressionTypeIn;
					}
					else if ( compressionType < 0 )
					{
						rc = 1;
						thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_COMPRESSION_TYPE)).c_str());
						ci->alterTableState = cal_connection_info::NOT_ALTER;
						ci->isAlter = false;
						return rc;
					}
					
					if (( compressionType > 0 ) && !(idbCompress.isCompressionAvail( compressionType )))
					{
						rc = 1;
#ifdef SKIP_IDB_COMPRESSION
		Message::Args args;
		thd->main_da.can_overwrite_status = true;
		args.add("The compression type");
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_ENTERPRISE_ONLY, args)).c_str());
#else
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_COMPRESSION_TYPE)).c_str());
#endif
						ci->alterTableState = cal_connection_info::NOT_ALTER;
						ci->isAlter = false;
						return rc;
					}
				}
				
				//Handle autoincrement
				if ( comment.length() > 0 )
				{					
					try {
#ifndef SKIP_AUTOI
	autoIncre = parseAutoincrementColumnComment(comment, startValue);
#else
	algorithm::to_upper(comment);
	if ( comment.find("AUTOINCREMENT") != string::npos )
	{
		int rc = 1;
		thd->main_da.can_overwrite_status = true;
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CREATE_AUTOINCREMENT_NOT_SUPPORT)).c_str());
		ci->alterTableState = cal_connection_info::NOT_ALTER;
		ci->isAlter = false;
		return rc;
	}
#endif
						
					}
					catch (runtime_error& ex)
					{
						rc = 1;
						thd->main_da.can_overwrite_status = true;
						thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, ex.what());
						ci->alterTableState = cal_connection_info::NOT_ALTER;
						ci->isAlter = false;
						return rc;
					}
					if (autoIncre)
					{
						//Check if the table already has autoincrement column
						CalpontSystemCatalog* csc = CalpontSystemCatalog::makeCalpontSystemCatalog(sessionID);
						CalpontSystemCatalog::TableName tableName;
						tableName.schema = alterTable->fTableName->fSchema;
						tableName.table = alterTable->fTableName->fName;
						CalpontSystemCatalog::TableInfo tblInfo = csc->tableInfo(tableName);	
						
						if (tblInfo.tablewithautoincr == 1)
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_NUMBER_AUTOINCREMENT)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;ci->isAlter = false;
							return rc;
						}
						
						if (!validateAutoincrementDatatype(renameColumnsPtr->fNewType->fType))
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_AUTOINCREMENT_TYPE)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						if (!validateNextValue(renameColumnsPtr->fNewType->fType, startValue))
						{
							rc = 1;
							thd->main_da.can_overwrite_status = true;
							thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_STARTVALUE)).c_str());
							ci->alterTableState = cal_connection_info::NOT_ALTER;
							ci->isAlter = false;
							return rc;
						}
						
					}
				}
		
				if (autoIncre)
					renameColumnsPtr->fNewType->fAutoincrement = "y";
				else
					renameColumnsPtr->fNewType->fAutoincrement = "n";
				
				renameColumnsPtr->fNewType->fNextvalue = startValue;
				renameColumnsPtr->fNewType->fCompressiontype = compressionType;
			}
			else
			{
			}
    	}
    }
    else
	{
	}
	//@Bug 4387
	scoped_ptr<DBRM> dbrmp(new DBRM());
	int rc = dbrmp->isReadWrite();
	if (rc != 0 )
	{
		rc = 1;
		thd->main_da.can_overwrite_status = true;
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Cannot execute the statement. DBRM is read only!");
		ci->alterTableState = cal_connection_info::NOT_ALTER;
		ci->isAlter = false;
		return rc;	
	}

    stmt.fSessionID = sessionID;
    stmt.fSql = ddlStatement;
	stmt.fOwner = schema;
	stmt.fTableWithAutoi = isAnyAutoincreCol;
	//cout << "Sending to DDLProc" << endl;
    ByteStream bytestream;
    bytestream << stmt.fSessionID;
    stmt.serialize(bytestream);
    MessageQueueClient mq("DDLProc");
    ByteStream::byte b=0;
    try
    {
      mq.write(bytestream);
      bytestream = mq.read();
	  if ( bytestream.length() == 0 )
	  {
		rc = 1;
      	thd->main_da.can_overwrite_status = true;

      	thd->main_da.set_error_status(thd, HA_ERR_INTERNAL_ERROR, "Lost connection to DDLProc");	
		ci->alterTableState = cal_connection_info::NOT_ALTER;
		ci->isAlter = false;
	  }
	  else
	  {
      	bytestream >> b;
      	bytestream >> emsg;
      	rc = b;
	  }
    }
    catch (runtime_error&)
    {
      rc =1;
      thd->main_da.can_overwrite_status = true;

	  thd->main_da.set_error_status(thd, HA_ERR_INTERNAL_ERROR, "Lost connection to DDLProc");
	  ci->alterTableState = cal_connection_info::NOT_ALTER;
	  ci->isAlter = false;
    }
    catch (...)
    {
      rc = 1;
      thd->main_da.can_overwrite_status = true;

	  thd->main_da.set_error_status(thd, HA_ERR_INTERNAL_ERROR, "Unknown error caught");
	  ci->alterTableState = cal_connection_info::NOT_ALTER;
	  ci->isAlter = false;
    }

    if ((b != 0) && (b!=ddlpackageprocessor::DDLPackageProcessor::WARNING))
    {
      thd->main_da.can_overwrite_status = true;

	  thd->main_da.set_error_status(thd, HA_ERR_INTERNAL_ERROR, emsg.c_str());
    }

	if (b ==ddlpackageprocessor::DDLPackageProcessor::WARNING)
	{
		rc = 0;
		string errmsg ("Error occured during file deletion. Restart DDLProc or use command tool ddlcleanup to clean up. " );
		push_warning(thd, MYSQL_ERROR::WARN_LEVEL_WARN, 9999, errmsg.c_str());
	}
    return rc;

  }
  else
  {
  	 rc = 1;
  	 thd->main_da.can_overwrite_status = true;
	 //@Bug 3602. Error message for MySql syntax for autoincrement
	 algorithm::to_upper(ddlStatement);
	 if (ddlStatement.find("AUTO_INCREMENT") != string::npos)
	 {
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Use of the MySQL auto_increment syntax is not supported in InfiniDB. If you wish to create an auto increment column in InfiniDB, please consult the InfiniDB SQL Syntax Guide for the correct usage.");
		ci->alterTableState = cal_connection_info::NOT_ALTER;
		ci->isAlter = false;
	 }
	 else
	 {
		//@Bug 1888,1885. update error message
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "The syntax or the data type(s) is not supported by InfiniDB. Please check the InfiniDB syntax guide for supported syntax or data types.");
		ci->alterTableState = cal_connection_info::NOT_ALTER;
		ci->isAlter = false;
	 }
  }
  return rc;
}

pair<string, string> parseTableName(const string& tn)
{
	string db;
	string tb;
	typedef boost::tokenizer<boost::char_separator<char> > tokenizer;
#ifdef _MSC_VER
	boost::char_separator<char> sep("\\");
#else
	boost::char_separator<char> sep("/");
#endif
	tokenizer tokens(tn, sep);
	tokenizer::iterator tok_iter = tokens.begin();
	++tok_iter;
	assert(tok_iter != tokens.end());
	db = *tok_iter;
	++tok_iter;
	assert(tok_iter != tokens.end());
	tb = *tok_iter;
	++tok_iter;
	assert(tok_iter == tokens.end());
	return make_pair(db, tb);
}

}

int ha_calpont_impl_create_(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info, cal_connection_info& ci)
{
#ifdef INFINIDB_DEBUG
    cout << "ha_calpont_impl_create_: " << name << endl;
#endif
    THD *thd = current_thd;

    string stmt = thd->query;
    stmt += ";";
    algorithm::to_upper(stmt);	

    string db = table_arg->s->db.str;
    string tbl = table_arg->s->table_name.str;
	string tablecomment;
	bool isAnyAutoincreCol = false;
	std::string columnName("");
	int64_t startValue = 1;
	if (table_arg->s->comment.length > 0 )
	{
		tablecomment = table_arg->s->comment.str;
		try {
#ifndef SKIP_AUTOI
	isAnyAutoincreCol = parseAutoincrementTableComment(tablecomment, startValue, columnName);
#else
	algorithm::to_upper(tablecomment);
	if ( tablecomment.find("AUTOINCREMENT") != string::npos )
	{
		int rc = 1;
		thd->main_da.can_overwrite_status = true;
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CREATE_AUTOINCREMENT_NOT_SUPPORT)).c_str());
		return rc;
	}
#endif
			
		}
		catch (runtime_error& ex)
		{
			thd->main_da.can_overwrite_status = true;
			thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, ex.what());
			return 1;
		}
		algorithm::to_upper(tablecomment);
	}
	//@Bug 2553 Add a parenthesis around option to group them together
	string alterpatstr("ALTER[[:space:]]+TABLE[[:space:]]+.*[[:space:]]+((ADD)|(DROP)|(CHANGE))[[:space:]]+");
	string createpatstr("CREATE[[:space:]]+TABLE[[:space:]]");
	bool schemaSyncOnly = false;
	bool isCreate = true;

    // relate to bug 1793. Make sure this is not for a select statement because 
    if (db == "calpontsys" && thd->infinidb_vtable.vtable_state == THD::INFINIDB_INIT
    	  && tbl != "systable" 
    	  && tbl != "syscolumn" && tbl != "sysindex"
    	  && tbl != "sysconstraint" && tbl != "sysindexcol"
    	  && tbl != "sysconstraintcol" )
    {
			thd->main_da.can_overwrite_status = true;
			thd->main_da.set_error_status(thd, HA_ERR_INTERNAL_ERROR, "Can not create non-system Calpont tables in calpontsys database");
			return 1;
    }

    regex pat("[[:space:]]*SCHEMA[[:space:]]+SYNC[[:space:]]+ONLY", regex_constants::extended);
    if (regex_search(tablecomment, pat))
    {
		schemaSyncOnly = true;
		pat = createpatstr;
		if (!regex_search(stmt, pat)) {
			isCreate = false;
		}
	
		if (isCreate)
		{
#ifdef INFINIDB_DEBUG
			cout << "ha_calpont_impl_create_: SCHEMA SYNC ONLY found, returning" << endl;
#endif
		  return 0;
		}
		else if (thd->infinidb_vtable.vtable_state == THD::INFINIDB_ALTER_VTABLE) //check if it is select
		{
			return 0;
		}
    }
    else
    {
    	if (db == "calpontsys")
	    {
				thd->main_da.can_overwrite_status = true;
				thd->main_da.set_error_status(thd, HA_ERR_INTERNAL_ERROR, "Calpont system tables can only be created with 'SCHEMA SYNC ONLY'");
				return 1;
	    }
		else if ( db == "infinidb_vtable") //@bug 3540. table created in infinidb_vtable schema could be dropped when select statement happen to have same tablename.
		{
			thd->main_da.can_overwrite_status = true;
			thd->main_da.set_error_status(thd, HA_ERR_INTERNAL_ERROR, "Table creation is not allowed in infinidb_vtable schema.");
			return 1;
		}
    }
    
    pat = alterpatstr;
    if (regex_search(stmt, pat)) {
		ci.isAlter = true;
        ci.alterTableState = cal_connection_info::ALTER_FIRST_RENAME;
#ifdef INFINIDB_DEBUG
		cout << "ha_calpont_impl_create_: now in state ALTER_FIRST_RENAME" << endl;
#endif
	}


    string emsg;
	stmt = thd->query;
    stmt += ";";
    int rc = 0;

	// Don't do the DDL (only for create table if this is SSO. Should only get here on ATAC w/SSO.
	if ( schemaSyncOnly && isCreate)
		return rc;
		
	// @bug 3908. error out primary key for now.
	if (table_arg->key_info && table_arg->key_info->name && string(table_arg->key_info->name) == "PRIMARY")
	{
		rc = 1;
		Message::Args args;
		thd->main_da.can_overwrite_status = true;
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_CONSTRAINTS, args)).c_str());
		ci.alterTableState = cal_connection_info::NOT_ALTER;
		ci.isAlter = false;
		return rc;
	}

	int compressiontype = thd->variables.infinidb_compression_type;
	if (compressiontype == 1) compressiontype = 2;
	//string tablecomment;
	if (table_arg->s->comment.length > 0 )
	{
		tablecomment = table_arg->s->comment.str;
		compressiontype = parseCompressionComment( tablecomment );
	}
	if ( compressiontype == MAX_INT )
		compressiontype = thd->variables.infinidb_compression_type;
	else if ( compressiontype < 0 )
	{
		rc = 1;
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_COMPRESSION_TYPE)).c_str());
		ci.alterTableState = cal_connection_info::NOT_ALTER;
		ci.isAlter = false;
		return rc;
	}	
	if (compressiontype == 1) compressiontype = 2;
	
	IDBCompressInterface idbCompress;
	if ( ( compressiontype > 0 ) && !(idbCompress.isCompressionAvail( compressiontype )) )
	{
		rc = 1;
#ifdef SKIP_IDB_COMPRESSION
		Message::Args args;
		thd->main_da.can_overwrite_status = true;
		args.add("The compression type");
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_ENTERPRISE_ONLY, args)).c_str());
#else
		thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, (IDBErrorInfo::instance()->errorMsg(ERR_INVALID_COMPRESSION_TYPE)).c_str());
#endif
		ci.alterTableState = cal_connection_info::NOT_ALTER;
		ci.isAlter = false;
		return rc;
	}	
			
	rc = ProcessDDLStatement(stmt, db, tbl, tid2sid(thd->thread_id), emsg, compressiontype, isAnyAutoincreCol, startValue, columnName);
	
    if (rc != 0) {
      push_warning(thd, MYSQL_ERROR::WARN_LEVEL_NOTE, 9999, emsg.c_str());
      //Bug 1705 reset the flag if error occurs
      ci.alterTableState = cal_connection_info::NOT_ALTER;
	  ci.isAlter = false;
#ifdef INFINIDB_DEBUG
		cout << "ha_calpont_impl_create_: ProcessDDL error, now in state NOT_ALTER" << endl;
#endif
    }
    return rc;
}

int ha_calpont_impl_delete_table_(const char *name, cal_connection_info& ci)
{
#ifdef INFINIDB_DEBUG
		cout << "ha_calpont_impl_delete_table: " << name << endl;
#endif
    THD *thd = current_thd;
    std::string tbl(name);
    std::string stmt(thd->query);
    algorithm::to_upper(stmt);
	// @bug 4158 allow table name with 'restrict' in it (but not by itself)
	std::string::size_type fpos;
	fpos = stmt.rfind(" RESTRICT");
	if ((fpos != std::string::npos) && ((stmt.size() - fpos) == 9)) //last 9 chars of stmt are " RESTRICT"
    {
        return 0;
    }

    TABLE_LIST *first_table= (TABLE_LIST*) thd->lex->select_lex.table_list.first;
		string db = first_table->table->s->db.str;
    string emsg;
	stmt = thd->query;
    stmt += ";";
    int rc = ProcessDDLStatement(stmt, db, tbl, tid2sid(thd->thread_id), emsg);
    if (rc != 0)
      push_warning(thd, MYSQL_ERROR::WARN_LEVEL_NOTE, 9999, emsg.c_str());
    return rc;
}

int ha_calpont_impl_rename_table_(const char* from, const char* to, cal_connection_info& ci)
{
    THD *thd = current_thd;
	string emsg;

	ostringstream stmt1;
	pair<string, string> fromPair;
	pair<string, string> toPair;
	string stmt;

	fromPair = parseTableName(from);
	toPair = parseTableName(to);

	if (fromPair.first != toPair.first)
	{
		thd->main_da.can_overwrite_status = true;

	 	thd->main_da.set_error_status(thd, HA_ERR_UNSUPPORTED, "Both tables must be in the same database to use RENAME TABLE");
		return -1;
	}

	stmt1 << "alter table " << fromPair.second << " rename to " << toPair.second << ";";

	stmt = stmt1.str();
	string db;
	if ( thd->db )
		db = thd->db;
	else if ( fromPair.first.length() !=0 )
		db = fromPair.first;
	else
		db = toPair.first; 
		
	int rc = ProcessDDLStatement(stmt, db, "", tid2sid(thd->thread_id), emsg);
	if (rc != 0)
		push_warning(thd, MYSQL_ERROR::WARN_LEVEL_ERROR, 9999, emsg.c_str());

	return rc;
}


extern "C"
{

#ifdef _MSC_VER
__declspec(dllexport)
#endif
long long calonlinealter(UDF_INIT* initid, UDF_ARGS* args,
					char* result, unsigned long* length,
					char* is_null, char* error)
{
	string stmt(args->args[0], args->lengths[0]);

	string emsg;
    THD *thd = current_thd;
	string db("");
	if ( thd->db )
		db = thd->db;
		
	int rc = ProcessDDLStatement(stmt, db, "", tid2sid(thd->thread_id), emsg);
	if (rc != 0)
		push_warning(thd, MYSQL_ERROR::WARN_LEVEL_ERROR, 9999, emsg.c_str());

	return rc;
}

#ifdef _MSC_VER
__declspec(dllexport)
#endif
my_bool calonlinealter_init(UDF_INIT* initid, UDF_ARGS* args, char* message)
{
	if (args->arg_count != 1 || args->arg_type[0] != STRING_RESULT)
	{
		strcpy(message,"CALONLINEALTER() requires one string argument");
		return 1;
	}
	return 0;
}

#ifdef _MSC_VER
__declspec(dllexport)
#endif
void calonlinealter_deinit(UDF_INIT* initid)
{
}

}

// vim:ts=4 sw=4:
