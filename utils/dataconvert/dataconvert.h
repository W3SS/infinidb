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

/****************************************************************************
* $Id: dataconvert.h 3256 2012-09-07 15:58:18Z xlou $
*
*
****************************************************************************/
/** @file */

#ifndef DATACONVERT_H
#define DATACONVERT_H

#include <string>
#include <boost/any.hpp>
#include <vector>
#ifdef _MSC_VER
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#else
#include <netinet/in.h>
#endif
#include <boost/regex.hpp>

#include "calpontsystemcatalog.h"
#include "columnresult.h"

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

// remove this block if the htonll is defined in library
#ifdef __linux__
#include <endian.h>
#if __BYTE_ORDER == __BIG_ENDIAN       // 4312
inline uint64_t htonll(uint64_t n)
{ return n; }
#elif __BYTE_ORDER == __LITTLE_ENDIAN  // 1234
inline uint64_t htonll(uint64_t n)
{
return ((((uint64_t) htonl(n & 0xFFFFFFFFLLU)) << 32) | (htonl((n & 0xFFFFFFFF00000000LLU) >> 32)));
}
#else  // __BYTE_ORDER == __PDP_ENDIAN    3412
inline uint64_t htonll(uint64_t n);
// don't know 34127856 or 78563412, hope never be required to support this byte order.
#endif
#else //!__linux__
//Assume we're on little-endian
inline uint64_t htonll(uint64_t n)
{
return ((((uint64_t) htonl(n & 0xFFFFFFFFULL)) << 32) | (htonl((n & 0xFFFFFFFF00000000ULL) >> 32)));
}
#endif //__linux__

// this method evalutes the uint64 that stores a char[] to expected value
inline uint64_t uint64ToStr(uint64_t n)
{ return htonll(n); }


#if defined(_MSC_VER) && defined(xxxDATACONVERT_DLLEXPORT)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

namespace dataconvert
{

enum CalpontDateTimeFormat
{
    CALPONTDATE_ENUM     = 1, // date format is: "YYYY-MM-DD"
    CALPONTDATETIME_ENUM = 2  // date format is: "YYYY-MM-DD HH:MI:SS"
};

/** @brief a structure to hold a date
 */
struct Date
{
    unsigned spare  : 6;
    unsigned day    : 6;
    unsigned month  : 4;
    unsigned year   : 16;
    // NULL column value = 0xFFFFFFFE
    Date( )   { year = 0xFFFF; month = 0xF; day = 0x3F; spare = 0x3E;}
    // Construct a Date from a 64 bit integer Calpont date.
    Date(uint64_t val) { year = (val >> 16); month = (val >> 12) & 0xF; day = (val >> 6) & 077; spare = 0; }
};

/** @brief a structure to hold a datetime
 */
struct DateTime
{
    unsigned msecond : 20;
    unsigned second  : 6;
    unsigned minute  : 6;
    unsigned hour    : 6;
    unsigned day     : 6;
    unsigned month   : 4;
    unsigned year    : 16;
    // NULL column value = 0xFFFFFFFFFFFFFFFE
    DateTime( ) { year = 0xFFFF; month = 0xF; day = 0x3F;
        hour = 0x3F; minute = 0x3F; second = 0x3F; msecond = 0xFFFFE; }
    // Construct a DateTime from a 64 bit integer Calpont datetime.
    DateTime(uint64_t val) {  year = val >> 48; month = (val >> 44) & 0xF; day = (val >> 38) & 077;
    hour = (val >> 32) & 077; minute = (val >> 26) & 077; second = (val >> 20) & 077; msecond = val & 0xFFFFF; }
};

/** @brief a structure to hold a time
 *  range: -838:59:59 ~ 838:59:59
 */
struct Time
{
	signed msecond : 24;
	signed second  : 8;
	signed minute  : 8;
	signed hour    : 12;
	signed day     : 12;
	
	// NULL column value = 0xFFFFFFFFFFFFFFFE
	Time() : msecond (0xFFFFFE),
	         second (0xFF),
	         minute (0xFF),
	         hour (0xFFF),
	         day (0xFFF){}

	// Construct a Time from a 64 bit integer InfiniDB time.
	Time(int64_t val)
	{
		day = (val >> 52) & 0xfff;
		hour = (val >> 40) & 0xfff;
		minute = (val >> 32) & 0xff;
		second = (val >> 24) & 0xff;
		msecond = val & 0xffffff; 
	}
};

/** @brief DataConvert is a component for converting string data to Calpont format
  */
class DataConvert
{
public:

    /**
     * @brief convert a columns data, represnted as a string, to it's native
     * format
     *
     * @param type the columns data type
     * @param data the columns string representation of it's data
     */
    EXPORT static boost::any convertColumnData( execplan::CalpontSystemCatalog::ColType colType,
                                  				const std::string& dataOrig, bool& pushWarning,
												bool nulFlag = false, bool noRoundup = false );

   /**
     * @brief convert a columns data from native format to a string
     *
     * @param type the columns database type
     * @param data the columns string representation of it's data
     */
    EXPORT static std::string dateToString( int  datevalue );  
    static inline void dateToString( int datevalue, char* buf, unsigned int buflen );

   /**
     * @brief convert a columns data from native format to a string
     *
     * @param type the columns database type
     * @param data the columns string representation of it's data
     */
    EXPORT static std::string datetimeToString( long long  datetimevalue );      
    static inline void datetimeToString( long long datetimevalue, char* buf, unsigned int buflen );

   /**
     * @brief convert a columns data from native format to a string
     *
     * @param type the columns database type
     * @param data the columns string representation of it's data
     */
    EXPORT static std::string dateToString1( int  datevalue );  
    static inline void dateToString1( int datevalue, char* buf, unsigned int buflen );

   /**
     * @brief convert a columns data from native format to a string
     *
     * @param type the columns database type
     * @param data the columns string representation of it's data
     */
    EXPORT static std::string datetimeToString1( long long  datetimevalue );      
    static inline void datetimeToString1( long long datetimevalue, char* buf, unsigned int buflen );

    /**
     * @brief convert a date column data, represnted as a string, to it's native
     * format. This function is for bulkload to use.
     *
     * @param type the columns data type
     * @param dataOrig the columns string representation of it's data
     * @param dateFormat the format the date value in
     * @param status 0 - success, -1 - fail
     * @param dataOrgLen length specification of dataOrg
     */
    EXPORT static u_int32_t convertColumnDate( const char* dataOrg,
                                  CalpontDateTimeFormat dateFormat,
                                  int& status, unsigned int dataOrgLen );
                                                                 
    /**
     * @brief convert a datetime column data, represented as a string,
     * to it's native format. This function is for bulkload to use.
     *
     * @param type the columns data type
     * @param dataOrig the columns string representation of it's data
     * @param datetimeFormat the format the date value in
     * @param status 0 - success, -1 - fail
     * @param dataOrgLen length specification of dataOrg
     */
    EXPORT static u_int64_t convertColumnDatetime( const char* dataOrg,
                                  CalpontDateTimeFormat datetimeFormat,
                                  int& status, unsigned int dataOrgLen );  

    EXPORT static bool isNullData(execplan::ColumnResult* cr, int rownum, execplan::CalpontSystemCatalog::ColType colType);
    static inline void decimalToString( int64_t value, uint8_t scale, char* buf, unsigned int buflen );                          
    static inline std::string constructRegexp(const std::string& str);
    static inline bool isEscapedChar(char c) { return ('%' == c || '_' == c); }
    
    // convert string to date
    EXPORT static int64_t stringToDate(std::string data);
    // convert string to datetime
    EXPORT static int64_t stringToDatetime(std::string data, bool* isDate = NULL);
    // convert integer to date
    EXPORT static int64_t intToDate(int64_t data);
    // convert integer to datetime
    EXPORT static int64_t intToDatetime(int64_t data, bool* isDate = NULL);
    
    // convert string to date. alias to stringToDate
    EXPORT static int64_t dateToInt(std::string date);
    // convert string to datetime. alias to datetimeToInt
    EXPORT static int64_t datetimeToInt(std::string datetime);
    EXPORT static int64_t stringToTime (std::string data);
    
    // bug4388, union type conversion
    EXPORT static execplan::CalpontSystemCatalog::ColType convertUnionColType(std::vector<execplan::CalpontSystemCatalog::ColType>&);

};

inline void DataConvert::dateToString( int datevalue, char* buf, unsigned int buflen)
{
	snprintf( buf, buflen, "%04d-%02d-%02d",
				(unsigned)((datevalue >> 16) & 0xffff),
				(unsigned)((datevalue >> 12) & 0xf),
				(unsigned)((datevalue >> 6) & 0x3f)
			);
}

inline void DataConvert::datetimeToString( long long datetimevalue, char* buf, unsigned int buflen )
{
	snprintf( buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d", 
					(unsigned)((datetimevalue >> 48) & 0xffff), 
					(unsigned)((datetimevalue >> 44) & 0xf),
					(unsigned)((datetimevalue >> 38) & 0x3f),
					(unsigned)((datetimevalue >> 32) & 0x3f),
					(unsigned)((datetimevalue >> 26) & 0x3f),
					(unsigned)((datetimevalue >> 20) & 0x3f)
				);
}

inline void DataConvert::dateToString1( int datevalue, char* buf, unsigned int buflen)
{
	snprintf( buf, buflen, "%04d%02d%02d",
				(unsigned)((datevalue >> 16) & 0xffff),
				(unsigned)((datevalue >> 12) & 0xf),
				(unsigned)((datevalue >> 6) & 0x3f)
			);
}

inline void DataConvert::datetimeToString1( long long datetimevalue, char* buf, unsigned int buflen )
{
	snprintf( buf, buflen, "%04d%02d%02d%02d%02d%02d", 
					(unsigned)((datetimevalue >> 48) & 0xffff), 
					(unsigned)((datetimevalue >> 44) & 0xf),
					(unsigned)((datetimevalue >> 38) & 0x3f),
					(unsigned)((datetimevalue >> 32) & 0x3f),
					(unsigned)((datetimevalue >> 26) & 0x3f),
					(unsigned)((datetimevalue >> 20) & 0x3f)
				);
}

inline void DataConvert::decimalToString( int64_t int_val, uint8_t scale, char* buf, unsigned int buflen )
{
	// Need to convert a string with a binary unsigned number in it to a 64-bit signed int
	
	// MySQL seems to round off values unless we use the string store method. Groan.
	// Taken from ha_calpont_impl.cpp
	
	//biggest Calpont supports is DECIMAL(18,x), or 18 total digits+dp+sign for column
	// Need 19 digits maxium to hold a sum result of 18 digits decimal column.
#ifndef __LP64__
	snprintf(buf, buflen, "%lld", int_val);
#else
	snprintf(buf, buflen, "%ld", int_val);
#endif
	//we want to move the last dt_scale chars right by one spot to insert the dp
	//we want to move the trailing null as well, so it's really dt_scale+1 chars
	size_t l1 = strlen(buf);
	char* ptr = &buf[0];
	if (int_val < 0)
	{
		ptr++;
		assert(l1 >= 2);
		l1--;
	}
	//need to make sure we have enough leading zeros for this to work...
	//at this point scale is always > 0
	if ((unsigned)scale > l1)
	{
		const char* zeros = "0000000000000000000"; //19 0's
		size_t diff=0;
		if (int_val != 0)
			diff = scale - l1; //this will always be > 0
		else
			diff = scale;
		memmove((ptr + diff), ptr, l1 + 1); //also move null
		memcpy(ptr, zeros, diff);
		if (int_val != 0)
			l1 = 0;
		else
			l1 = 1;
	}
	else
		l1 -= scale;
	memmove((ptr + l1 + 1), (ptr + l1), scale + 1); //also move null
	if ( scale != 0 )
		*(ptr + l1) = '.';
}


//FIXME: copy/pasted from dictionary.cpp: refactor
inline std::string DataConvert::constructRegexp(const std::string& str)
{
	//In the worst case, every char is quadrupled, plus some leading/trailing cruft...
	char* cBuf = (char*)alloca(((4 * str.length()) + 3) * sizeof(char));
	char c;
	uint i, cBufIdx = 0;
	// translate to regexp symbols
	cBuf[cBufIdx++] = '^';  // implicit leading anchor
	for (i = 0; i < str.length(); i++) {
		c = (char) str.c_str()[i];
		switch (c) {

			// chars to substitute
			case '%':
				cBuf[cBufIdx++] = '.';
				cBuf[cBufIdx++] = '*';
				break;
			case '_':
				cBuf[cBufIdx++] = '.';
				break;

			// escape the chars that are special in regexp's but not in SQL
			// default special characters in perl: .[{}()\*+?|^$
			case '.':
			case '*':
			case '^':
			case '$':
 			case '?':
 			case '+':
 			case '|':
 			case '[':
 			case '{':
 			case '}':
 			case '(':
 			case ')':
				cBuf[cBufIdx++] = '\\';
				cBuf[cBufIdx++] = c;
				break;
			case '\\':  //this is the sql escape char
				if ( i + 1 < str.length())
				{
					if (isEscapedChar(str.c_str()[i+1]))
					{
						cBuf[cBufIdx++] = str.c_str()[++i];
						break;
					}
					else if ('\\' == str.c_str()[i+1])
					{
						cBuf[cBufIdx++] = c;
						cBuf[cBufIdx++] = str.c_str()[++i];
						break;
					}
					
				}  //single slash
				cBuf[cBufIdx++] = '\\';
				cBuf[cBufIdx++] = c;
				break;
			default:
				cBuf[cBufIdx++] = c;
		}
	}
	cBuf[cBufIdx++] = '$';  // implicit trailing anchor
	cBuf[cBufIdx++] = '\0';

#ifdef VERBOSE
  	cerr << "regexified string is " << cBuf << endl;
#endif
	return cBuf;
}

} // namespace dataconvert

#undef EXPORT

#endif //DATACONVERT_H

