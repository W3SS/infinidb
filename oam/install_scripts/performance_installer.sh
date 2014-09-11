#!/usr/bin/expect
#
# $Id: performance_installer.sh 1128 2009-01-05 16:36:59Z rdempsey $
#
# Install RPM and custom OS files on system
# Argument 1 - Remote Module Name
# Argument 2 - Remote Server Host Name or IP address
# Argument 3 - Root Password of remote server
# Argument 4 - Package name being installed
# Argument 5 - Install Type, "initial" or "upgrade"
# Argument 6 - Debug flag 1 for on, 0 for off
set timeout 30
set USERNAME root
set MODULE [lindex $argv 0]
set SERVER [lindex $argv 1]
set PASSWORD [lindex $argv 2]
set CALPONTRPM [lindex $argv 3]
set CALPONTMYSQLRPM [lindex $argv 4]
set CALPONTMYSQLDRPM [lindex $argv 5]
set INSTALLTYPE [lindex $argv 6]
set PKGTYPE [lindex $argv 7]
set NODEPS [lindex $argv 8]
set DEBUG [lindex $argv 9]

set BASH "/bin/bash "
if { $DEBUG == "1" } {
	set BASH "/bin/bash -x "
}

log_user $DEBUG
spawn -noecho /bin/bash
#
if { $PKGTYPE == "rpm" } {
	set PKGERASE "rpm -e --nodeps --allmatches "
	set PKGINSTALL "rpm -ivh $NODEPS "
	set PKGUPGRADE "rpm -Uvh --noscripts "
} else {
	if { $PKGTYPE == "deb" } {
		set PKGERASE "dpkg -P "
		set PKGINSTALL "dpkg -i "
		set PKGUPGRADE "dpkg -i "
	} else {
		if { $PKGTYPE != "bin" } {
			send_user "Invalid Package Type of $PKGTYPE"
			exit -1
		}
	}
}

# check and see if remote server has ssh keys setup, set PASSWORD if so
send_user " "
send "ssh $USERNAME@$SERVER 'time'\n"
set timeout 10
expect {
	-re "Host key verification failed" { send_user "FAILED: Host key verification failed\n" ; exit -1 }
	-re "service not known" { send_user "FAILED: Invalid Host\n" ; exit -1 }
	-re "authenticity" { send "yes\n" 
						expect {
							-re "word: " { send "$PASSWORD\n" } abort
							-re "passphrase: " { send "$PASSWORD\n" } abort
						}
	}
	-re "real" { set PASSWORD "ssh" } abort
	-re "word: " { send "$PASSWORD\n" } abort
	-re "passphrase: " { send "$PASSWORD\n" } abort
	-re "Permission denied, please try again"   { send_user "ERROR: Invalid password\n" ; exit -1 }
}
set timeout 5
expect {
	-re "# "        {  } abort
	-re "real" {  } abort
}
send_user "\n"

if { $INSTALLTYPE == "initial" || $INSTALLTYPE == "uninstall" } {
	# 
	# unmount disk
	#
	send_user "Unmount disk                                    "
	send "ssh $USERNAME@$SERVER '$BASH /usr/local/Calpont/bin/syslogSetup.sh uninstall > /dev/null 2>&1;umount /usr/local/Calpont/data*'\n"
	if { $PASSWORD != "ssh" } {
		set timeout 10
		expect {
			-re "word: " { send "$PASSWORD\n" } abort
			-re "passphrase: " { send "$PASSWORD\n" } abort
		}
	}
	set timeout 20
	expect {
		-re "Permission denied, please try again"   { send_user "ERROR: Invalid password\n" ; exit -1 }
		-re "# "                  { send_user "DONE" } abort
	}
	send_user "\n"
	#
	if { $PKGTYPE == "bin" } { exit 0 }		

	# 
	# erase package
	#
	send_user "Erase calpont Package on Module                 "
	set timeout 10
	expect -re "# "
	send "ssh $USERNAME@$SERVER '$PKGERASE calpont'\n"
	if { $PASSWORD != "ssh" } {
		set timeout 10
		expect {
			-re "word: " { send "$PASSWORD\n" } abort
			-re "passphrase: " { send "$PASSWORD\n" } abort
		}
	}
	set timeout 30
	expect {
		-re "# "                  { send_user "DONE" } abort
		-re "uninstall completed" { send_user "DONE" } abort
		-re "removing" { send_user "DONE" } abort
		-re "error: Failed dependencies" { send_user "ERROR: Failed dependencies\n" ; exit -1 }
		-re "Permission denied, please try again"   { send_user "ERROR: Invalid password\n" ; exit -1 }
		-re "error: package"       { send_user "INFO: Package not installed" } abort
		-re "not installed"       { send_user "INFO: Package not installed" } abort
		-re "isn't installed"       { send_user "INFO: Package not installed" } abort
	}
	send_user "\n"
}
if { $INSTALLTYPE == "uninstall" } { exit 0 }

#gives time for the rpmsave to be done
sleep 5
# 
# send the package
#
set timeout 10
expect -re "# "
send_user "Copy New calpont Package to Module              "
send "ssh $USERNAME@$SERVER 'rm -f /root/calpont*.$PKGTYPE'\n"
if { $PASSWORD != "ssh" } {
	set timeout 10
	expect {
		-re "word: " { send "$PASSWORD\n" } abort
		-re "passphrase: " { send "$PASSWORD\n" } abort
	}
}
expect {
	-re "# " { } abort
}
set timeout 10
expect -re "# "
send "scp $CALPONTRPM  $USERNAME@$SERVER:$CALPONTRPM\n"
if { $PASSWORD != "ssh" } {
	set timeout 10
	expect {
		-re "word: " { send "$PASSWORD\n" } abort
		-re "passphrase: " { send "$PASSWORD\n" } abort
	}
}
set timeout 60
expect {
	-re "100%" 				{ send_user "DONE" } abort
	-re "directory"  				{ send_user "ERROR\n" ; 
				 			send_user "\n*** Installation ERROR\n" ; 
							exit -1 }
	-re "Permission denied, please try again"         { send_user "ERROR: Invalid password\n" ; exit -1 }
	-re "No such file or directory" { send_user "ERROR: Invalid package\n" ; exit -1 }
}
send_user "\n"
# 
# send the package
#
send_user "Copy New calpont-mysql Package to Module        "
send "scp $CALPONTMYSQLRPM  $USERNAME@$SERVER:$CALPONTMYSQLRPM\n"
if { $PASSWORD != "ssh" } {
	set timeout 10
	expect {
		-re "word: " { send "$PASSWORD\n" } abort
		-re "passphrase: " { send "$PASSWORD\n" } abort
	}
}
set timeout 30
expect {
	-re "100%" 				{ send_user "DONE" } abort
	-re "directory"  				{ send_user "ERROR\n" ; 
				 			send_user "\n*** Installation ERROR\n" ; 
							exit -1 }
	-re "Permission denied, please try again"         { send_user "ERROR: Invalid password\n" ; exit -1 }
	-re "No such file or directory" { send_user "ERROR: Invalid package\n" ; exit -1 }
}
send_user "\n"
# 
# send the package
#
send_user "Copy New calpont-mysqld Package to Module       "
send "scp $CALPONTMYSQLDRPM  $USERNAME@$SERVER:$CALPONTMYSQLDRPM\n"
if { $PASSWORD != "ssh" } {
	set timeout 10
	expect {
		-re "word: " { send "$PASSWORD\n" } abort
		-re "passphrase: " { send "$PASSWORD\n" } abort
	}
}
set timeout 30
expect {
	-re "100%" 				{ send_user "DONE" } abort
	-re "directory"  				{ send_user "ERROR\n" ; 
				 			send_user "\n*** Installation ERROR\n" ; 
							exit -1 }
	-re "Permission denied, please try again"         { send_user "ERROR: Invalid password\n" ; exit -1 }
	-re "No such file or directory" { send_user "ERROR: Invalid package\n" ; exit -1 }
}
send_user "\n"
#
if { $INSTALLTYPE == "initial"} {
	#
	# install package
	#
	send_user "Install calpont Package on Module               "
	send "ssh $USERNAME@$SERVER '$PKGINSTALL $CALPONTRPM'\n"
	if { $PASSWORD != "ssh" } {
		set timeout 10
		expect {
			-re "word: " { send "$PASSWORD\n" } abort
			-re "passphrase: " { send "$PASSWORD\n" } abort
		}
	}
	set timeout 60
	expect {
		-re "# " 		  		  { send_user "DONE" } abort
		-re "completed" 		  { send_user "DONE" } abort
		-re "Setting up" 		  { send_user "DONE" } abort
		-re "error: Failed dependencies" { send_user "ERROR: Failed dependencies\n" ; 
									send_user "\n*** Installation ERROR\n" ; 
										exit -1 }
		-re "Permission denied, please try again"   { send_user "ERROR: Invalid password\n" ; exit -1 }
	}
} else {
	#
	# upgrade package
	#
	send_user "Upgrade calpont Package on Module               "
	send "ssh $USERNAME@$SERVER '$PKGUPGRADE  $CALPONTRPM'\n"
	if { $PASSWORD != "ssh" } {
		set timeout 10
		expect {
			-re "word: " { send "$PASSWORD\n" } abort
			-re "passphrase: " { send "$PASSWORD\n" } abort
		}
	}
	set timeout 60
	expect {
		-re "# " 		  		  { send_user "DONE" } abort
		-re "completed" 		  { send_user "DONE" } abort
		-re "Setting up" 		  { send_user "DONE" } abort
		-re "already installed"   { send_user "INFO: Already Installed\n" ; exit -1 }
		-re "error: Failed dependencies" { send_user "ERROR: Failed dependencies\n" ; 
									send_user "\n*** Installation ERROR\n" ; 
										exit -1 }
		-re "Permission denied, please try again"   { send_user "ERROR: Invalid password\n" ; exit -1 }
	}
}
set timeout 10
expect -re "# "
send_user "\n"
if { $PKGTYPE == "deb" } {
	send_user "Run post-install script                         "
	send "ssh $USERNAME@$SERVER '$BASH /usr/local/Calpont/bin/post-install'\n"
	if { $PASSWORD != "ssh" } {
		set timeout 10
		expect {
			-re "word: " { send "$PASSWORD\n" } abort
			-re "passphrase: " { send "$PASSWORD\n" } abort
		}
	}
	set timeout 30
	expect {
		-re "# " 		  		  { send_user "DONE" } abort
		-re "Permission denied, please try again"   { send_user "ERROR: Invalid password\n" ; exit -1 }
	}
	send_user "\n"
}
#
if { $INSTALLTYPE == "initial"} {
	#
	# copy over calpont config file
	#
	send_user "Copy Calpont Config file to Module              "
	send "scp /usr/local/Calpont/etc/*  $USERNAME@$SERVER:/usr/local/Calpont/etc/.\n"
	if { $PASSWORD != "ssh" } {
		set timeout 10
		expect {
			-re "word: " { send "$PASSWORD\n" } abort
			-re "passphrase: " { send "$PASSWORD\n" } abort
		}
	}
	set timeout 30
	expect {
		-re "# " 		  		  { send_user "DONE" } abort
		-re "Permission denied, please try again"   { send_user "ERROR: Invalid password\n" ; exit -1 }
	}
	send_user "\n"
	#
	# copy over custom OS tmp files
	#
	send_user "Copy Custom OS files to Module                  "
	send "scp -r /usr/local/Calpont/local/etc  $USERNAME@$SERVER:/usr/local/Calpont/local/.\n"
	if { $PASSWORD != "ssh" } {
		set timeout 10
		expect {
			-re "word: " { send "$PASSWORD\n" } abort
			-re "passphrase: " { send "$PASSWORD\n" } abort
		}
	}
	set timeout 30
	expect {
		-re "# " 		  		  { send_user "DONE" } abort
		-re "Permission denied, please try again"   { send_user "ERROR: Invalid password\n" ; exit -1 }
	}
	send_user "\n"
	#
	# copy over calpont OS files
	#
	send_user "Copy Calpont OS files to Module                 "
	send "scp /usr/local/Calpont/local/etc/$MODULE/*  $USERNAME@$SERVER:/usr/local/Calpont/local/.\n"
	if { $PASSWORD != "ssh" } {
		set timeout 10
		expect {
			-re "word: " { send "$PASSWORD\n" } abort
			-re "passphrase: " { send "$PASSWORD\n" } abort
		}
	}
	set timeout 30
	expect {
		-re "# " 		  		  { send_user "DONE" } abort
		-re "Permission denied, please try again"   { send_user "ERROR: Invalid password\n" ; exit -1 }
	}
	#
	send_user "\n"
	#
	# Start module installer to setup Custom OS files
	#
	send_user "Run Module Installer                            "
	send "ssh $USERNAME@$SERVER '$BASH /usr/local/Calpont/bin/module_installer.sh'\n"
	if { $PASSWORD != "ssh" } {
		set timeout 10
		expect {
			-re "word: " { send "$PASSWORD\n" } abort
			-re "passphrase: " { send "$PASSWORD\n" } abort
		}
	}
	set timeout 30
	expect {
		-re "!!!Module" 				  			{ send_user "DONE" } abort
		-re "Permission denied, please try again"   { send_user "ERROR: Invalid password\n" ; exit -1 }
		-re "FAILED"   								{ send_user "ERROR: missing OS file\n" ; exit -1 }
	}
	send_user "\n"
}

#
# check InfiniDB syslog functionality
#
set timeout 10
expect -re "# "

send_user "Check InfiniDB system logging functionality     "
send " \n"
send date\n
send "ssh $USERNAME@$SERVER '$BASH /usr/local/Calpont/bin/syslogSetup.sh check'\n"
if { $PASSWORD != "ssh" } {
	set timeout 10
	expect {
		-re "word: " { send "$PASSWORD\n" } abort
		-re "passphrase: " { send "$PASSWORD\n" } abort
	}
}
set timeout 30
expect {
	-re "Logging working" { send_user "DONE" } abort
	timeout { send_user "DONE" } 
	-re "not working" { send_user "WARNING: InfiniDB system logging functionality not working" } abort
}
send_user "\n"

#
send_user "\nInstallation Successfully Completed on '$MODULE'\n"
exit 0

