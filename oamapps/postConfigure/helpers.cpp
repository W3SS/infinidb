#include <string>
#include <vector>
#include <unistd.h>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <stdio.h>

#include <readline/readline.h>

#include "configcpp.h"
using namespace config;

using namespace std;

#include "liboamcpp.h"
using namespace oam;

#include "helpers.h"

string mysqlpw = " ";
string pwprompt = " ";


namespace installer
{

bool waitForActive() 
{
	Oam oam;

	system("/usr/local/Calpont/bin/calpontConsole getsystemstatus > /tmp/status.log");

	for ( int i = 0 ; i < 40 ; i ++ )
	{
		if (oam.checkLogStatus("/tmp/status.log", "System        ACTIVE") )
			return true;
		if ( oam.checkLogStatus("/tmp/status.log", "System        FAILED") )
			return false;
		cout << ".";
		cout.flush();
		sleep (10);
		system("/usr/local/Calpont/bin/calpontConsole getsystemstatus > /tmp/status.log");
	}
	return false;
}

void dbrmDirCheck() 
{

	ifstream oldFile ("/usr/local/Calpont/etc/Calpont.xml.rpmsave");
	if (!oldFile) return;

	string SystemSection = "SystemConfig";
	Config* sysConfig = Config::makeConfig();
	Config* sysConfigPrev = Config::makeConfig("/usr/local/Calpont/etc/Calpont.xml.rpmsave");

	char* pcommand = 0;

	string dbrmroot = "";
	string dbrmrootPrev = "";

	try {
		dbrmroot = sysConfig->getConfig(SystemSection, "DBRMRoot");
		dbrmrootPrev = sysConfigPrev->getConfig(SystemSection, "DBRMRoot");
	}
	catch(...)
	{}

	if ( dbrmrootPrev.empty() )
		return;

	if ( dbrmroot == dbrmrootPrev )
		return;

	string dbrmrootDir = "";
	string dbrmrootPrevDir = "";

	string::size_type pos = dbrmroot.find("/BRM_saves",0);
	if (pos != string::npos)
		//get directory path
		dbrmrootDir = dbrmroot.substr(0,pos);
	else 
	{
		return;
	}

	pos = dbrmrootPrev.find("/BRM_saves",0);
	if (pos != string::npos)
		//get directory path
		dbrmrootPrevDir = dbrmrootPrev.substr(0,pos);
	else 
	{
		return;
	}

	// return if prev directory doesn't exist
	ifstream File (dbrmrootPrevDir.c_str());
	if (!File)
		return;

	string dbrmrootCurrent = dbrmroot + "_current";
	string dbrmrootCurrentPrev = dbrmrootPrev + "_current";

	// return if prev current file doesn't exist
	ifstream File1 (dbrmrootCurrentPrev.c_str());
	if (!File1)
		return;

	// check if current file does't exist
	// if not, copy prev files to current directory
	ifstream File2 (dbrmrootCurrent.c_str());
	if (!File2) {
		cout << endl << "===== DBRM Data File Directory Check  =====" << endl << endl;
		string cmd = "/bin/cp -rpf " + dbrmrootPrevDir + "/* " + dbrmrootDir + "/.";
		system(cmd.c_str());

		//update the current file hardcoded path
		ifstream oldFile (dbrmrootCurrent.c_str());
		if (oldFile) {
			char line[200];
			oldFile.getline(line, 200);
			string dbrmFile = line;

			string::size_type pos = dbrmFile.find("/BRM_saves",0);
			if (pos != string::npos)
				dbrmFile = dbrmrootDir + dbrmFile.substr(pos,80);

			unlink (dbrmrootCurrent.c_str());
			ofstream newFile (dbrmrootCurrent.c_str());
		
			string cmd = "echo " + dbrmFile + " > " + dbrmrootCurrent;
			system(cmd.c_str());
		
			newFile.close();
		}

		cmd = "mv -f " + dbrmrootPrevDir + " " + dbrmrootPrevDir + ".old";
		system(cmd.c_str());
		cout << endl << "DBRM data files were copied from dbrm directory" << endl;
		cout << dbrmrootPrevDir << " to current directory of " << dbrmrootDir << "." << endl;
		cout << "The old dbrm directory was renamed to " << dbrmrootPrevDir << ".old ." << endl;
	}
	else
	{
		string start = "y";
		cout << endl << "===== DBRM Data File Directory Check  =====" << endl << endl;
		cout << endl << "DBRM data files were found in " << dbrmrootPrevDir << endl;
		cout << "and in the new location " << dbrmrootDir << "." << endl << endl;
		cout << "Make sure that the correct set of files are in the new location." << endl;
		cout << "Then rename the directory " << dbrmrootPrevDir << " to " << dbrmrootPrevDir << ".old" << endl;
		cout << "If the files were copied from " << dbrmrootPrevDir << " to " << dbrmrootDir << endl;
		cout << "you will need to edit the file BRM_saves_current to contain the current path of" << endl;
		cout << dbrmrootDir << endl << endl;
		cout << "Please reference the Calpont InfiniDB Installation Guide on Upgrade Installs for" << endl;
		cout << "addition information, if needed." << endl << endl;

		while(true)
		{
			string answer = "n";
			pcommand = readline("Enter 'y' when you are ready to continue > ");
			if (pcommand)
			{
				if (strlen(pcommand) > 0) answer = pcommand;
				free(pcommand);
				pcommand = 0;
			}
			if ( answer == "y" )
				break;
			else
				cout << "Invalid Entry, please enter 'y' for yes" << endl;
		}
	}

	system("chmod 1777 -R /usr/local/Calpont/data1/systemFiles/dbrm > /dev/null 2>&1");

	return;
}

void mysqlSetup() 
{
	Oam oam;
	int rtnCode = system("/usr/local/Calpont/bin/post-mysqld-install");
	if (rtnCode != 0)
		cout << "Error running post-mysqld-install" << endl;

	//check for password set
	//start in the same way that mysqld will be started normally.
	try {
		oam.actionMysqlCalpont(MYSQL_START);
	}
	catch(...)
	{}
	sleep(2);
	
	string prompt = " *** Enter MySQL password > ";
	for (;;)
	{
		// check if mysql is supported and get info
		string calpontMysql = "/usr/local/Calpont/mysql/bin/mysql --defaults-file=/usr/local/Calpont/mysql/my.cnf -u root ";
		string cmd = calpontMysql + pwprompt + " -e 'status' > /tmp/idbmysql.log 2>&1";
		system(cmd.c_str());

		if (oam.checkLogStatus("/tmp/idbmysql.log", "ERROR 1045") ) {
			mysqlpw = getpass(prompt.c_str());
			pwprompt = "--password=" + mysqlpw;
			prompt = " *** Password incorrect, please re-enter MySQL password > ";
		}
		else
		{
			if (!oam.checkLogStatus("/tmp/idbmysql.log", "InfiniDB") ) {
				cout << endl << "ERROR: MySQL runtime error, exit..." << endl << endl;
				system("cat /tmp/idbmysql.log");
				exit (1);
			}
			else
			{
				try {
					oam.actionMysqlCalpont(MYSQL_STOP);
				}
				catch(...)
				{}
				unlink("/tmp/idbmysql.log");
				break;
			}
		}
	}
	
	string cmd = "/usr/local/Calpont/bin/post-mysql-install " + pwprompt;
	rtnCode = system(cmd.c_str());
	if (rtnCode != 0)
		cout << "Error running post-mysql-install" << endl;

	return;
}

/******************************************************************************************
* @brief	sendUpgradeRequest
*
* purpose:	send Upgrade Request Msg to all ACTIVE UMs
*
*
******************************************************************************************/
int sendUpgradeRequest(int IserverTypeInstall)
{
	Oam oam;

	// wait until DMLProc is ACTIVE
	while(true)
	{
		try{
			ProcessStatus procstat;
			oam.getProcessStatus("DMLProc", "pm1", procstat);
			if ( procstat.ProcessOpState == oam::ACTIVE)
				break;
		}
		catch (exception& ex)
		{}
	}

	SystemModuleTypeConfig systemmoduletypeconfig;

	try{
		oam.getSystemConfig(systemmoduletypeconfig);
	}
	catch (exception& ex)
	{}

	ByteStream msg;
	ByteStream::byte requestID = RUNUPGRADE;

	msg << requestID;
	msg << mysqlpw;

	int returnStatus = oam::API_SUCCESS;

	for( unsigned int i = 0; i < systemmoduletypeconfig.moduletypeconfig.size(); i++)
	{
		int moduleCount = systemmoduletypeconfig.moduletypeconfig[i].ModuleCount;
		if( moduleCount == 0)
			continue;

		string moduleType = systemmoduletypeconfig.moduletypeconfig[i].ModuleType;
		if ( moduleType == "um" ||
			( moduleType == "pm" && IserverTypeInstall == oam::INSTALL_COMBINE_DM_UM_PM ) ) {

			DeviceNetworkList::iterator pt = systemmoduletypeconfig.moduletypeconfig[i].ModuleNetworkList.begin();
			for ( ; pt != systemmoduletypeconfig.moduletypeconfig[i].ModuleNetworkList.end(); pt++)
			{
				int opState;
				bool degraded;
				try {
					oam.getModuleStatus((*pt).DeviceName, opState, degraded);

					if (opState == oam::ACTIVE) {
						returnStatus = sendMsgProcMon( (*pt).DeviceName, msg, requestID, 30 );
		
						if ( returnStatus != API_SUCCESS)
							return returnStatus;
					}
				}
				catch (exception& ex)
				{}
			}
		}
	}
	return returnStatus;
}

/******************************************************************************************
* @brief	sendMsgProcMon
*
* purpose:	Sends a Msg to ProcMon
*
******************************************************************************************/
int sendMsgProcMon( std::string module, ByteStream msg, int requestID, int timeout )
{
	string msgPort = module + "_ProcessMonitor";
	int returnStatus = API_FAILURE;
	Oam oam;

	// do a ping test to determine a quick failure
	Config* sysConfig = Config::makeConfig();

	string IPAddr = sysConfig->getConfig(msgPort, "IPAddr");

	if ( IPAddr == oam::UnassignedIpAddr ) {
		return returnStatus;
	}

	string cmdLine = "ping ";
	string cmdOption = " -w 1 >> /dev/null";
	string cmd = cmdLine + IPAddr + cmdOption;
	if ( system(cmd.c_str()) != 0) {
		//ping failure
		return returnStatus;
	}

	try
	{
		MessageQueueClient mqRequest(msgPort);
		mqRequest.write(msg);

		if ( timeout > 0 ) {
			// wait for response
			ByteStream::byte returnACK;
			ByteStream::byte returnRequestID;
			ByteStream::byte requestStatus;
			ByteStream receivedMSG;
		
			struct timespec ts = { timeout, 0 };

			// get current time in seconds
			time_t startTimeSec;
			time (&startTimeSec);

			while(true)
			{
				try {
					receivedMSG = mqRequest.read(&ts);
				}
				catch (...) {
					return returnStatus;
				}
	
				if (receivedMSG.length() > 0) {
					receivedMSG >> returnACK;
					receivedMSG >> returnRequestID;
					receivedMSG >> requestStatus;
		
					if ( returnACK == oam::ACK &&  returnRequestID == requestID) {
						// ACK for this request
						returnStatus = requestStatus;
					break;	
					}	
				}
				else
				{	//api timeout occurred, check if retry should be done
					// get current time in seconds
					time_t endTimeSec;
					time (&endTimeSec);
					if ( timeout <= (endTimeSec - startTimeSec) ) {
						break;
					}
				}
			}
		}
		else
			returnStatus = oam::API_SUCCESS;

		mqRequest.shutdown();
	}
	catch (exception& ex)
	{}

	return returnStatus;
}


void checkFilesPerPartion(int DBRootCount, Config* sysConfig)
{
	// check 'files per parition' with number of dbroots
	// 'files per parition' need to be a multiple of dbroots
	// update if no database already exist
	// issue warning if database exist

	Oam oam;
	string SystemSection = "SystemConfig";

	string dbRoot = "/usr/local/Calpont/data1";

	try {
		dbRoot = sysConfig->getConfig(SystemSection, "DBRoot1");
	}
	catch(...)
	{}

	dbRoot = dbRoot + "/000.dir";

	float FilesPerColumnPartition = 4;
	try {
		string tmp = sysConfig->getConfig("ExtentMap", "FilesPerColumnPartition");
		FilesPerColumnPartition = atoi(tmp.c_str());
	}
	catch(...)
	{}

	if ( fmod(FilesPerColumnPartition , (float) DBRootCount) != 0 ) {
		ifstream oldFile (dbRoot.c_str());
		if (!oldFile) {
			//set FilesPerColumnPartition == DBRootCount
			sysConfig->setConfig("ExtentMap", "FilesPerColumnPartition", oam.itoa(DBRootCount));
	
			cout << endl << "***************************************************************************" << endl;
			cout <<         "NOTE: Mismatch between FilesPerColumnPartition (" + oam.itoa((int)FilesPerColumnPartition) + ") and number of DBRoots (" + oam.itoa(DBRootCount) + ")" << endl;
			cout <<         "      Setting FilesPerColumnPartition = number of DBRoots"  << endl;
			cout <<         "***************************************************************************" << endl;
		}
		else
		{
			cout << endl << "***************************************************************************" << endl;
			cout <<         "WARNING: Mismatch between FilesPerColumnPartition (" + oam.itoa((int)FilesPerColumnPartition) + ") and number of DBRoots (" + oam.itoa(DBRootCount) + ")" << endl;
			cout <<         "         Database already exist, going forward could corrupt the database"  << endl;
			cout <<         "         Please Contact Customer Support"  << endl;
			cout <<         "***************************************************************************" << endl;
			exit (1);
		}
	}


	return;
}

void cleanupFstab()
{
	string fileName = "/etc/fstab";

	ifstream oldFile (fileName.c_str());
	if (!oldFile) return;
	
	vector <string> lines;
	char line[200];
	string buf;
	while (oldFile.getline(line, 200))
	{
		buf = line;
		string::size_type pos = buf.find("usr/local/Calpont/data ",0);
		if (pos != string::npos) {
			//output to temp file
			lines.push_back(buf);
			continue;
		}
		pos = buf.find("#",0);
		if (pos != string::npos) {
			//output to temp file
			lines.push_back(buf);
			continue;
		}
		pos = buf.find("usr/local/Calpont/data",0);
		if (pos == string::npos)
			//output to temp file
			lines.push_back(buf);
	}
	
	oldFile.close();
	unlink (fileName.c_str());
   	ofstream newFile (fileName.c_str());	
	
	//create new file
	int fd = open(fileName.c_str(), O_RDWR|O_CREAT, 0666);
	
	copy(lines.begin(), lines.end(), ostream_iterator<string>(newFile, "\n"));
	newFile.close();
	
	close(fd);
	return;
}

}

