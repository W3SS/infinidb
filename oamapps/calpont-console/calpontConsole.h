/******************************************************************************************
 * $Id: calpontConsole.h 2641 2012-09-05 21:45:28Z dhill $
 *
 ******************************************************************************************/
/**
 * @file
 */
#ifndef CALPONTCONSOLE_H
#define CALPONTCONSOLE_H

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <string>
#include <limits.h>
#include <sstream>
#include <exception>
#include <stdexcept>
#include <vector>
#include <stdio.h>
#include <ctype.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

#include "liboamcpp.h"
#include "configcpp.h"
#include "snmpmanager.h"
#include "snmpglobal.h"
#include "calpontsystemcatalog.h"
#include "brmtypes.h"


const int CmdSize = 80;
const int ArgNum = 10;
const int DescNumMax = 10;
const int cmdNum = 68;

const std::string  DEFAULT_LOG_FILE = "/var/log/Calpont/uiCommands.log";
std::ofstream   logFile;

/**
 * write the command to the log file
 */
void    writeLog(std::string command);

/** @brief location of the Process Configuration file
 */
const std::string ConsoleCmdsFile= "ConsoleCmds.xml";

void getFlags(const std::string* arguments, oam::GRACEFUL_FLAG& gracefulTemp, oam::ACK_FLAG& ackTemp, oam::CC_SUSPEND_ANSWER& suspendAnswer);
int confirmPrompt(std::string warningCommand);
std::string dataPrompt(std::string promptCommand);
int processCommand(std::string*);
int ProcessSupportCommand(int CommandID, std::string arguments[]);
void printAlarmSummary();
void printCriticalAlarms();
void checkRepeat(std::string*, int);
void printSystemStatus();
void printProcessStatus(std::string port = "ProcStatusControl");
void printModuleCpuUsers(oam::TopProcessCpuUsers topprocesscpuusers);
void printModuleCpu(oam::ModuleCpu modulecpu);
void printModuleMemoryUsers(oam::TopProcessMemoryUsers topprocessmemoryusers);
void printModuleMemory(oam::ModuleMemory modulememory);
void printModuleDisk(oam::ModuleDisk moduledisk);
void printModuleResources(oam::TopProcessCpuUsers topprocesscpuusers, oam::ModuleCpu modulecpu, oam::TopProcessMemoryUsers topprocessmemoryusers, oam::ModuleMemory modulememory, oam::ModuleDisk moduledisk);
void printState(int state, std::string addInfo);
std::string getParentOAMModule();
bool checkForDisabledModules();
oam::CC_SUSPEND_ANSWER AskSuspendQuestion(int CmdID);



class to_lower
{
    public:
        char operator() (char c) const            // notice the return type
        {
            return tolower(c);
        }
};

/** @brief Hidden Support commands in lower-case
*/
const std::string supportCmds[] = {	"helpsupport",
									"stopprocess",
									"startprocess",
									"restartprocess",
									"killpid",
									"rebootsystem",
									"rebootnode",
									"stopdbrmprocess",
									"startdbrmprocess",
									"restartdbrmprocess",
									"setsystemstartupstate",
									"stopprimprocs",
									"startprimprocs",
									"restartprimprocs",
									"stopexemgrs",
									"startexemgrs",
									"restartexemgrs",
									"getprocessstatusstandby",
									"distributeconfigfile",
									"getpmdbrootconfig",
									"getdbrootpmconfig",
									"getsystemdbrootconfig",
									"checkdbfunctional",
									""
};


#endif