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

/***********************************************************************
*   $Id: inetstreamsocket.cpp 2436 2011-03-07 17:48:38Z pleblanc $
*
*
***********************************************************************/
#include "config.h"

#include <cerrno>
#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#else
#if __FreeBSD__
#include <sys/types.h>
#endif
#include <sys/socket.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#endif
#include <sys/types.h>
#include <sys/time.h>
#include <cstring>

#include <stdexcept>
#include <string>
#include <sstream>
using namespace std;

#include <boost/scoped_array.hpp>
using boost::scoped_array;

#include "bytestream.h"
#include "iosocket.h"
#include "socketparms.h"
#include "inetstreamsocket.h"
#include "socketclosed.h"
#include "logger.h"
#include "loggingid.h"

// some static functions
namespace {
using messageqcpp::ByteStream;

// @bug 2441 - Retry after 512 read() error.
// ERESTARTSYS (512) is a kernal I/O errno that is similar to a EINTR, except
// that it is not supposed to "leak" out into the user space.   But we are
// sometimes seeing "unknown error 512" error msgs in response to calls to
// read(), so adding logic to retry after ERESTARTSYS the way we do for EINTR.
//const int KERR_ERESTARTSYS = 512;

#ifdef _MSC_VER
const int MaxSendPacketSize=64 * 1024;
#endif

bool pollfd(int fd) 
{
	struct pollfd pfd[1];
	pfd[0].fd = fd;
	pfd[0].events = POLLOUT;
		poll(pfd, 1, 0);
	return ((pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0);
}

} //namespace anon

namespace messageqcpp {

InetStreamSocket::InetStreamSocket(size_t blocksize) :
	fSocketParms(PF_INET, SOCK_STREAM, IPPROTO_TCP),
	fBlocksize(blocksize),
	fSyncProto(true),
	fMagicBuffer(0)
{
	memset(&fSa, 0, sizeof(fSa));
	fConnectionTimeout.tv_sec = 20;
	fConnectionTimeout.tv_nsec = 0;
}

InetStreamSocket::~InetStreamSocket()
{
}

void InetStreamSocket::open()
{
	int bufferSize;
	int ret;
	socklen_t bufferSizeSize;

	if (isOpen())
		throw logic_error("InetStreamSocket::open: socket is already open");

	int sd;
	sd = ::socket(fSocketParms.domain(), fSocketParms.type(), fSocketParms.protocol());
	int e = errno;

	if (sd < 0)
	{
#ifdef _MSC_VER
		int wsaError = WSAGetLastError();
		if (wsaError == WSANOTINITIALISED)
		{
			WSAData wsadata;
			const WORD minVersion = MAKEWORD(2, 2);
			if (WSAStartup(minVersion, &wsadata) == 0)
			{
				if (wsadata.wVersion == minVersion)
				{
					sd = ::socket(fSocketParms.domain(), fSocketParms.type(), fSocketParms.protocol());
					e = errno;
					if (sd >= 0) goto setopts;
				}
				//Didn't get the required min version, error out
			}
			//WSAStartup failed, continue to report error
		}
#endif
		string msg = "InetStreamSocket::open: socket() error: ";
 		scoped_array<char> buf(new char[80]);
#if STRERROR_R_CHAR_P
		const char* p;
		if ((p = strerror_r(e, buf.get(), 80)) != 0)
			msg += p;
#else
		int p;
		if ((p = strerror_r(e, buf.get(), 80)) == 0)
			msg += buf.get();
#endif
		throw runtime_error(msg);
	}

#ifdef _MSC_VER
setopts:
#endif

/*  XXXPAT:  If we have latency problems again, try these... 
	bufferSizeSize = 4;
	bufferSize = 512000;
	setsockopt(sd, SOL_SOCKET, SO_SNDBUF, &bufferSize, bufferSizeSize);
	bufferSize = 512000;
	setsockopt(sd, SOL_SOCKET, SO_RCVBUF, &bufferSize, bufferSizeSize);
	bufferSize = 1;
	setsockopt(sd, SOL_SOCKET, SO_RCVLOWAT, &bufferSize, bufferSizeSize);
	setsockopt(sd, SOL_SOCKET, SO_SNDLOWAT, &bufferSize, bufferSizeSize);
*/
	bufferSize = 1;
	bufferSizeSize = 4;
	ret = setsockopt(sd, IPPROTO_TCP, TCP_NODELAY, (const char*)&bufferSize, bufferSizeSize);
	if (ret < 0) {
		perror("setsockopt");
		exit(1);
	}
	bufferSize = 1;
	ret = setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (const char*)&bufferSize, bufferSizeSize);
	if (ret < 0) {
		perror("setsockopt");
		exit(1);
	}

	fSocketParms.sd(sd);
}

void InetStreamSocket::close()
{
	if (isOpen())
	{
		::shutdown(fSocketParms.sd(), SHUT_RDWR);
#ifdef _MSC_VER
		::closesocket(fSocketParms.sd());
#else
		::close(fSocketParms.sd());
#endif
		fSocketParms.sd(-1);
	}
}

// needs to be in sync with clone()
void InetStreamSocket::doCopy(const InetStreamSocket& rhs)
{
	fBlocksize = rhs.fBlocksize;
	fSocketParms = rhs.fSocketParms;
	fSa = rhs.fSa;
	fConnectionTimeout = rhs.fConnectionTimeout;
	fSyncProto = rhs.fSyncProto;
}

// needs to be in sync with doCopy()
Socket* InetStreamSocket::clone() const
{
	InetStreamSocket* iss = new InetStreamSocket(fBlocksize);
	iss->fSocketParms = fSocketParms;
	iss->fSa = fSa;
	iss->fConnectionTimeout = fConnectionTimeout;
	iss->fSyncProto = fSyncProto;
	return iss;
}

InetStreamSocket::InetStreamSocket(const InetStreamSocket& rhs)
{
	doCopy(rhs);
}

InetStreamSocket& InetStreamSocket::operator=(const InetStreamSocket& rhs)
{
	if (this != &rhs)
		doCopy(rhs);

	return *this;
}

/* The caller needs to know when/if the remote closes the connection or sends data.
 * Returns 0 on timeout, 1 if there is data to read, 2 if the connection was dropped.
 */
int InetStreamSocket::pollConnection(int connectionNum, long msecs)
{
	struct pollfd pfd[1];
	int err;

retry:
	memset(&pfd, 0, sizeof(struct pollfd));
	pfd[0].fd = connectionNum;
	pfd[0].events = POLLIN;
	err = poll(pfd, 1, msecs);
	if (err < 0) {
		int e = errno;
		if (e == EINTR || e == KERR_ERESTARTSYS)
			goto retry;
	}

	// Linux doesn't set POLLHUP, need add'l check for data or EOF
	if (pfd[0].revents & POLLIN) {
		char buf;
		err = ::recv(connectionNum, &buf, 1, MSG_PEEK);
		if (err == 0)
			return 2;
		else if (err == 1)	// there is in fact data to read
			return 1;
		else
			return 3;
	}
	if (err == 0)	// timeout
		return 0;
	return 3;       // catch-all error code
}

/* returns true when the next thing in the stream is the beginning of a new
ByteStream object. */
bool InetStreamSocket::readToMagic(long msecs, bool* isTimeOut) const
{
	int err;
	struct pollfd pfd[1];
	uint8_t *magicBuffer8;

	magicBuffer8 = reinterpret_cast<uint8_t*>(&fMagicBuffer);
	pfd[0].fd = fSocketParms.sd();
	pfd[0].events = POLLIN;

	while (fMagicBuffer != BYTESTREAM_MAGIC) {

		if (msecs >= 0) {
			pfd[0].revents = 0;

			err = poll(pfd, 1, msecs);
			if (err < 0) {
				int e = errno;
				if (e == EINTR) {
					continue;
				}
				if (e == KERR_ERESTARTSYS) {
					logIoError("InetStreamSocket::readToMagic(): I/O error1",e);
					continue;
				}

				ostringstream oss;
				oss << "InetStreamSocket::readToMagic(): I/O error1: " <<
					strerror(e);
				throw runtime_error(oss.str());
			}

			if (pfd[0].revents & (POLLHUP | POLLNVAL | POLLERR)) {
				ostringstream oss;
				oss << "InetStreamSocket::readToMagic(): I/O error1: rc-" <<
					err << "; poll signal interrupt ( ";
				if (pfd[0].revents & POLLHUP)
					oss << "POLLHUP ";
				if (pfd[0].revents & POLLNVAL)
					oss << "POLLNVAL ";
				if (pfd[0].revents & POLLERR)
					oss << "POLLERR ";
				oss << ")";
				throw runtime_error(oss.str());
			}

			if (err == 0) // timeout
			{
				if (isTimeOut)
					*isTimeOut = true;
				return false;
			}
		}

		fMagicBuffer = fMagicBuffer >> 8;
retry:
#ifdef _MSC_VER
		err = ::recv(fSocketParms.sd(), (char*)&magicBuffer8[3], 1, 0);
#else
		err = ::read(fSocketParms.sd(), &magicBuffer8[3], 1);
#endif
		if (err < 0) {
			int e = errno;
#ifdef _MSC_VER
			if (WSAGetLastError() == WSAECONNRESET)
			{
				//throw runtime_error("connection reset by peer");
				if (msecs < 0) return false;
				else throw SocketClosed("InetStreamSocket::readToMagic: Remote is closed");
			}
#endif
			if (e == EINTR) {
				goto retry;
			}
			if (e == KERR_ERESTARTSYS) {
				logIoError("InetStreamSocket::readToMagic(): I/O error2.0",e);
				goto retry;
			}

			ostringstream oss;
			oss << "InetStreamSocket::readToMagic(): I/O error2.1: " <<
				"err = " << err << " e = " << e <<
#ifdef _MSC_VER
				" WSA error = " << WSAGetLastError() <<
#endif
				": " << strerror(e);
			throw runtime_error(oss.str());
		}

		// EOF. If no timeout was specified, ByteStream() gets returned to the caller.
		// If one was, throw SocketClosed.
		if (err == 0)	// EOF. if a timeout was specified, ByteStream() 
		{
			if (msecs < 0)
				return false;
			else 
				throw SocketClosed("InetStreamSocket::readToMagic: Remote is closed");
		}
	}
	fMagicBuffer = 0;
	return true;
}

const SBS InetStreamSocket::read(const struct ::timespec* timeout, bool* isTimeOut) const
{
 	long msecs = -1;

	struct pollfd pfd[1];
	pfd[0].fd = fSocketParms.sd();
	pfd[0].events = POLLIN;

	if (timeout != 0)
		msecs = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;

	// we need to read the 4-byte message length first.
	uint32_t msglen;
	ByteStream::byte* msglenp = reinterpret_cast<ByteStream::byte*>(&msglen);
	size_t mlread = 0;
	
	if (readToMagic(msecs, isTimeOut) == false)	//indicates a timeout or EOF
		return SBS(new ByteStream(0));

	//FIXME: This seems like a lot of work to read 4 bytes...
	while (mlread < sizeof(msglen))
	{
		ssize_t t;
		if (timeout != NULL) {
			int err;

			pfd[0].revents = 0;
			err = poll(pfd, 1, msecs);
			if (err < 0 || pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				ostringstream oss;
				oss << "InetStreamSocket::read: I/O error1: " <<
				  strerror(errno);
				throw runtime_error(oss.str());
			}
			if (err == 0)  // timeout
			{
				if (isTimeOut)
					*isTimeOut = true;
				return SBS(new ByteStream(0));
			}
		}

#ifdef _MSC_VER
		t = ::recv(fSocketParms.sd(), (char*)(msglenp + mlread), sizeof(msglen) - mlread, 0);
#else
		t = ::read(fSocketParms.sd(), msglenp + mlread, sizeof(msglen) - mlread);
#endif
		if (t == 0)
		{
			if (timeout == NULL)
				return SBS(new ByteStream(0));  // don't return an incomplete message
			else
				throw SocketClosed("InetStreamSocket::read: Remote is closed");
		}
		if (t < 0) {
			int e = errno;
			if (e == EINTR) {
				continue;
			}
			if (e == KERR_ERESTARTSYS) {
				logIoError("InetStreamSocket::read: I/O error2",e);
				continue;
			}

			ostringstream oss;
			oss << "InetStreamSocket::read: I/O error2: " <<
			  strerror(e);
			throw runtime_error(oss.str());
		}
		mlread += t;
	}

	SBS res(new ByteStream(msglen));
 	ByteStream::byte* bufp = res->getInputPtr();

	size_t nread = 0;

	//Finally read the actual message...
	while (nread < msglen)
	{
		ssize_t t;

		if (timeout != NULL) {
			int err;

			pfd[0].revents = 0;
			err = poll(pfd, 1, msecs);
			if (err < 0 || pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				ostringstream oss;
				oss << "InetStreamSocket::read: I/O error3: " <<
					strerror(errno);
				throw runtime_error(oss.str());
			}
			if (err == 0)  // timeout
			{
				if (isTimeOut)
					*isTimeOut = true;
				return SBS(new ByteStream(0));
			}
		}

#ifdef _MSC_VER
		int readAmount = std::min((int)msglen - (int)nread, MaxSendPacketSize);
		t = ::recv(fSocketParms.sd(), (char*)(bufp + nread), readAmount, 0);
#else
 		t = ::read(fSocketParms.sd(), bufp + nread, msglen - nread);
#endif
		if (t == 0) 
		{
			if (timeout == NULL)
				return SBS(new ByteStream(0));	// don't return an incomplete message
			else
				throw SocketClosed("InetStreamSocket::read: Remote is closed");
		}
		if (t < 0) {
			ostringstream oss;
#ifdef _MSC_VER
			int e = WSAGetLastError();
			oss << "InetStreamSocket::read: I/O error4: WSA: " << e;
#else
			int e = errno;
			if (e == EINTR) {
				continue;
			}
			if (e == KERR_ERESTARTSYS) {
				logIoError("InetStreamSocket::read: I/O error4",e);
				continue;
			}

			oss << "InetStreamSocket::read: I/O error4: " <<
				strerror(e);
#endif
			throw runtime_error(oss.str());
		}
		nread += t;
	}
	res->advanceInputPtr(msglen);

	return res;
}

/*
* The protocol here is that we write the length of the ByteStream first, then the bytes. On the
* read side, we reverse it.
*/

void InetStreamSocket::write(const ByteStream& msg) const
{
	uint32_t msglen = msg.length();
	uint32_t magic = BYTESTREAM_MAGIC;
	uint32_t *realBuf;

	if (msglen == 0) return;

	/* buf.fCurOutPtr points to the data to send; ByteStream guarantees that there
	   are at least 8 bytes before that for the magic & length fields */
	realBuf = (uint32_t *)msg.buf();
	realBuf -= 2;
	realBuf[0] = magic;
	realBuf[1] = msglen;

	try {
		writen(fSocketParms.sd(), (const uint8_t*)realBuf, msglen + sizeof(msglen) + sizeof(magic));
	}
	catch (std::exception& ex) {
		string errorMsg(ex.what());
		errorMsg += " -- write from " + toString();
		throw runtime_error(errorMsg);
	}
}

void InetStreamSocket::write_raw(const ByteStream& msg) const
{
	uint32_t msglen = msg.length();

	if (msglen == 0) return;

	try {
		writen(fSocketParms.sd(), msg.buf(), msglen);
	}
	catch (std::exception& ex) {
		string errorMsg(ex.what());
		errorMsg += " -- write_raw from " + toString();
		throw runtime_error(errorMsg);
	}
}

void InetStreamSocket::bind(const struct sockaddr_in* serv_addr)
{
	fSa = *serv_addr;
	if (::bind(fSocketParms.sd(), (struct sockaddr*)serv_addr, sizeof(*serv_addr)) != 0)
	{
		int e = errno;
		string msg = "InetStreamSocket::bind: bind() error: ";
 		scoped_array<char> buf(new char[80]);
#if STRERROR_R_CHAR_P
		const char* p;
		if ((p = strerror_r(e, buf.get(), 80)) != 0)
			msg += p;
#else
		int p;
		if ((p = strerror_r(e, buf.get(), 80)) == 0)
			msg += buf.get();
#endif
		throw runtime_error(msg);
	}

}

void InetStreamSocket::listen(int backlog)
{
	if (::listen(socketParms().sd(), backlog) != 0)
	{
		int e = errno;
		string msg = "InetStreamSocket::listen: listen() error: ";
 		scoped_array<char> buf(new char[80]);
#if STRERROR_R_CHAR_P
		const char* p;
		if ((p = strerror_r(e, buf.get(), 80)) != 0)
			msg += p;
#else
		int p;
		if ((p = strerror_r(e, buf.get(), 80)) == 0)
			msg += buf.get();
#endif
		throw runtime_error(msg);
	}

}

const IOSocket InetStreamSocket::accept(const struct timespec* timeout)
{
	int clientfd;
	long msecs = 0;

	IOSocket ios(new InetStreamSocket(fBlocksize));

	struct pollfd pfd[1];
	pfd[0].fd = socketParms().sd();
	pfd[0].events = POLLIN;

	if (timeout != 0)
	{
		msecs = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
		if (poll(pfd, 1, msecs) != 1 || (pfd[0].revents & POLLIN) == 0 ||
			pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			return ios;
	}

	struct sockaddr_in sa;
	socklen_t sl = sizeof(sa);
	int e;
	do
	{
		clientfd = ::accept(socketParms().sd(), reinterpret_cast<sockaddr*>(&sa), &sl);
		e = errno;
	} while (clientfd < 0 && (e == EINTR ||
#ifdef ERESTART
		e == ERESTART ||
#endif
#ifdef ECONNABORTED
		e == ECONNABORTED ||
#endif
		false));
	if (clientfd < 0)
	{
		string msg = "InetStreamSocket::accept: accept() error: ";
 		scoped_array<char> buf(new char[80]);
#if STRERROR_R_CHAR_P
		const char* p;
		if ((p = strerror_r(e, buf.get(), 80)) != 0)
			msg += p;
#else
		int p;
		if ((p = strerror_r(e, buf.get(), 80)) == 0)
			msg += buf.get();
#endif
		throw runtime_error(msg);
	}

	if (fSyncProto)
	{
		/* send a byte to artificially synchronize with connect() on the remote */
		char b = 'A';
		int ret;

		ret = ::send(clientfd, &b, 1, 0);
		e = errno;
		if (ret < 0) {
			ostringstream  os;
			char blah[80];
#if STRERROR_R_CHAR_P
			const char* p;
			if ((p = strerror_r(e, blah, 80)) != 0)
				os << "InetStreamSocket::accept sync: " << p;
#else
			int p;
			if ((p = strerror_r(e, blah, 80)) == 0)
				os << "InetStreamSocket::accept sync: " << blah;
#endif
			::close(clientfd);
			throw runtime_error(os.str());
		}
		else if (ret == 0) {
			::close(clientfd);
			throw runtime_error("InetStreamSocket::accept sync: got unexpected error code");
		}
	}

	SocketParms sp;
	sp = ios.socketParms();
	sp.sd(clientfd);
	ios.socketParms(sp);
	ios.sa(sa);
	return ios;

}

void InetStreamSocket::connect(const struct sockaddr_in* serv_addr)
{
	fSa = *serv_addr;
	if (::connect(socketParms().sd(), (struct sockaddr*)serv_addr, sizeof(*serv_addr)))
	{
		int e = errno;
		string msg = "InetStreamSocket::connect: connect() error: ";
 		scoped_array<char> buf(new char[80]);
#if STRERROR_R_CHAR_P
		const char* p;
		if ((p = strerror_r(e, buf.get(), 80)) != 0)
			msg += p;
#else
		int p;
		if ((p = strerror_r(e, buf.get(), 80)) == 0)
			msg += buf.get();
#endif
		throw runtime_error(msg);
	}

	if (!fSyncProto)
		return;

	/* read a byte to artificially synchronize with accept() on the remote */
	int ret = -1;
	int e = EBADF;
	char buf = '\0';
	struct pollfd pfd;

	long msecs = fConnectionTimeout.tv_sec * 1000 + fConnectionTimeout.tv_nsec / 1000000;

	do {
		pfd.fd = socketParms().sd();
		pfd.revents = 0;
		pfd.events = POLLIN;
		ret = poll(&pfd, 1, msecs);
		e = errno;
	} while (ret == -1 && e == EINTR && !(pfd.revents & (POLLERR | POLLHUP | POLLNVAL)));

	// success
	if (ret == 1) {
		int rd;
#ifdef _MSC_VER
		rd = ::recv(socketParms().sd(), &buf, 1, 0);
#else
		rd = ::read(socketParms().sd(), &buf, 1);
#endif
		return;
	}

	/* handle the various errors */
	if (ret == 0)
		throw runtime_error("InetStreamSocket::connect: connection timed out");
	else if (ret == -1 && e != EINTR) {
		ostringstream  os;
		char blah[80];
#if STRERROR_R_CHAR_P
		const char* p;
		if ((p = strerror_r(e, blah, 80)) != 0)
			os << "InetStreamSocket::connect: " << p;
#else
		int p;
		if ((p = strerror_r(e, blah, 80)) == 0)
			os << "InetStreamSocket::connect: " << blah;
#endif
		throw runtime_error(os.str());
	}
	else
		throw runtime_error("InetStreamSocket::connect: unknown connection error");
}

const string InetStreamSocket::toString() const
{
	ostringstream oss;
	char buf[INET_ADDRSTRLEN];
	const SocketParms& sp = fSocketParms;
	oss << "InetStreamSocket: sd: " << sp.sd() <<
#ifndef _MSC_VER
	       " inet: " << inet_ntop(AF_INET, &fSa.sin_addr, buf, INET_ADDRSTRLEN) <<
#endif
	       " port: " << ntohs(fSa.sin_port);
	return oss.str();
}

//
//  Log a Warning msg pertaining to an I/O error; Currently used to log a
//  ERESTARTSYS (errno 512) condition, but could be used to log any other
//  I/O error that will retried.
//
void InetStreamSocket::logIoError(const char* errMsg, int errNum) const
{
	logging::Logger        logger(31);
	logging::Message::Args args;
	logging::LoggingID     li(31);
	args.add(errMsg);
	args.add(strerror(errNum));
	args.add(toString());

	logging::MsgMap msgMap;
	msgMap[logging::M0071] = logging::Message( logging::M0071 );
	logger.msgMap(msgMap);

	logger.logMessage(logging::LOG_TYPE_WARNING, logging::M0071, args, li);
}

ssize_t InetStreamSocket::writen(int fd, const ByteStream::byte* ptr, size_t nbytes) const
{
	size_t nleft;
	ssize_t nwritten;
	const char* bufp;
	
	nleft = nbytes;
	bufp = reinterpret_cast<const char*>(ptr);

	while (nleft > 0)
	{
		// the O_NONBLOCK flag is not set, this is a blocking I/O.
#ifdef _MSC_VER
		int writeAmount = std::min((int)nleft, MaxSendPacketSize);
		if ((nwritten = ::send(fd, bufp, writeAmount, 0)) < 0)
#else
  		if ((nwritten = ::write(fd, bufp, nleft)) < 0)
#endif
		{
			if (errno == EINTR)
				nwritten = 0;
			else {
				// save the error no first
				int e = errno;
				string errorMsg = "InetStreamSocket::write error: ";
		 		scoped_array<char> buf(new char[80]);
#if STRERROR_R_CHAR_P
				const char* p;
		 		if ((p = strerror_r(e, buf.get(), 80)) != 0)
					errorMsg += p;
#else
				int p;
		 		if ((p = strerror_r(e, buf.get(), 80)) == 0)
					errorMsg += buf.get();
#endif
				throw runtime_error(errorMsg);
			}
		}
		nleft -= nwritten;
		bufp += nwritten;
	}

	return nbytes;
}

} //namespace messageqcpp

