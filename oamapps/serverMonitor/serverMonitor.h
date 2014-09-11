/***************************************************************************
 * $Id: serverMonitor.h 34 2006-09-29 21:13:54Z dhill $
 *
 *   Author: David Hill
 ***************************************************************************/
/**
 * @file
 */
#ifndef SERVER_MONITOR_H
#define SERVER_MONITOR_H

#include <iostream>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <pthread.h>
#include <list>
#include <sys/statvfs.h> 
#include <stdio.h> 
#include <sys/sysinfo.h>

#include "liboamcpp.h"
#include "messagelog.h"
#include "messageobj.h"
#include "loggingid.h"
#include "snmpmanager.h"
#include "socketclosed.h"

#define CPU_DEBUG 0		// 0 for supported
#define DISK_DEBUG 0		// 0 for supported

#define CPU_HEARTBEAT_ID 1
#define MEMORY_HEARTBEAT_ID 2
#define HW_HEARTBEAT_ID 3

#define MONITOR_PERIOD 60	// 60 seconds

typedef struct 
{
	std::string processName;
	double usedPercent;
} processCPU;

typedef   std::list<processCPU> ProcessCPUList;

typedef struct 
{
	std::string processName;
	long long usedBlocks;
	double usedPercent;
} processMemory;

typedef   std::list<processMemory> ProcessMemoryList;

typedef struct 
{
	std::string deviceName;
	long long totalBlocks;
	long long usedBlocks;
	double usedPercent;
} SMSystemDisk;

typedef   std::list<SMSystemDisk> SystemDiskList;


/**
* @brief Local Process-Monitor Monitor Thread
*/
void procmonMonitor();

/**
* @brief CPU Monitor Thread
*/
void cpuMonitor();

/**
* @brief Disk Monitor Thread
*/
void diskMonitor();

/**
* @brief Disk Monitor Thread
*/
void memoryMonitor();

/**
* @brief Hardware Monitor
*/
void hardwareMonitor(int IPMI_SUPPORT);

/**
* @brief Message Processor Thread
*/
void msgProcessor();

/**
* @brief Disk Monitor Thread
*/
void diskTest();

/**
* @brief DB Health Monitor Thread
*/
void dbhealthMonitor();

namespace servermonitor{

// Log ID
#define SERVER_MONITOR_LOG_ID 9


class ServerMonitor
{
public:
    /**
     * @brief Constructor
     */
    ServerMonitor();

    /**
     * @brief Default Destructor
     */
    ~ServerMonitor();
	
	/**
	* @brief send alarm
	*/
	void sendAlarm(std::string alarmItem, oam::ALARMS alarmID, int action, float sensorValue);
	
	/**
	* @brief check alarm
	*/
	void checkAlarm(std::string alarmItem, oam::ALARMS alarmID = oam::NO_ALARM);
	
	/**
	* @brief clear alarm
	*/
	void clearAlarm(std::string alarmItem, oam::ALARMS alarmID);
	
	/**
	* @brief send msg to shutdown server
	*/
	void sendMsgShutdownServer();
	
	/**
	* @brief strip off whitespaces from a string
	*/
	std::string StripWhitespace(std::string value);
	
	/**
	* @brief log cpu usage to active log file
	*/
	void logCPUactive (unsigned int); 
	
	/**
	* @brief log cpu peak and average to stat file
	*/
	void logCPUstat (int usageCount);
	
	/**
	* @brief send alarm
	*/
	void sendResourceAlarm(std::string alarmItem, oam::ALARMS alarmID, int action, int usage);
	
	/**
	* @brief check CPU alarm
	*/
	void checkCPUAlarm(std::string alarmItem, oam::ALARMS alarmID = oam::NO_ALARM);

	/**
	* @brief check Disk alarm
	*/
	void checkDiskAlarm(std::string alarmItem, oam::ALARMS alarmID = oam::NO_ALARM);

	/**
	* @brief check Memory alarm
	*/
	void checkMemoryAlarm(std::string alarmItem, oam::ALARMS alarmID = oam::NO_ALARM);

	/**
	* @brief check Swap alarm
	*/
	void checkSwapAlarm(std::string alarmItem, oam::ALARMS alarmID = oam::NO_ALARM);

	/**
	* @brief check Swap action
	*/
	void checkSwapAction();

	/**
	* @brief output Proc Memory
	*/
	void outputProcMemory(bool);

	/**
	* @brief get CPU Data
	*/
	void getCPUdata();

	/**
	* @brief db health check 
	*/
	int healthCheck(bool action = true);

}; // end of class

} // end of namespace

#endif
