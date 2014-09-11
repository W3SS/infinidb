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
these changes are made, Calpont will make a new copy of the Calpont End User
License Agreement available at http://www.calpont.com and a new copy of the
Calpont Open Source License Agreement available at http:///www.infinidb.org.
You understand and agree that if you use the Program after the date on which
the license agreement authorizing your use has changed, Calpont will treat your
use as acceptance of the updated License.

*/
/******************************************************************************************
* $Id: genErrId.pl 2399 2011-02-10 14:23:16Z rdempsey $
*
******************************************************************************************/
/**
 * @file
 */
#ifndef LOGGING_ERRORIDS_H
#define LOGGING_ERRORIDS_H

namespace logging {

EOD

$backmatter = <<'EOD';

}//namespace logging

#endif //LOGGING_ERRORIDS_H

EOD

print $frontmatter;

while (<FH>)
{
	chomp;
	next if (/^$/);
	next if (/^#/);
	($errid, $errname, undef) = split;
	printf "const unsigned %s = %d;\n", $errname, $errid;
}

print $backmatter;

close FH;

