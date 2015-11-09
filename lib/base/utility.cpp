/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2015 Icinga Development Team (http://www.icinga.org)    *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "base/utility.hpp"
#include "base/convert.hpp"
#include "base/application.hpp"
#include "base/logger.hpp"
#include "base/exception.hpp"
#include "base/socket.hpp"
#include "base/utility.hpp"
#include "base/json.hpp"
#include "base/objectlock.hpp"
#include <mmatch.h>
#include <boost/lexical_cast.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <ios>
#include <fstream>
#include <iostream>

#ifdef __FreeBSD__
#	include <pthread_np.h>
#endif /* __FreeBSD__ */

#ifdef HAVE_CXXABI_H
#	include <cxxabi.h>
#endif /* HAVE_CXXABI_H */

#ifndef _WIN32
#       include <sys/types.h>
#       include <pwd.h>
#       include <grp.h>
#endif /* _WIN32 */


using namespace icinga;

boost::thread_specific_ptr<String> Utility::m_ThreadName;
boost::thread_specific_ptr<unsigned int> Utility::m_RandSeed;

/**
 * Demangles a symbol name.
 *
 * @param sym The symbol name.
 * @returns A human-readable version of the symbol name.
 */
String Utility::DemangleSymbolName(const String& sym)
{
	String result = sym;

#ifdef HAVE_CXXABI_H
	int status;
	char *realname = abi::__cxa_demangle(sym.CStr(), 0, 0, &status);

	if (realname != NULL) {
		result = String(realname);
		free(realname);
	}
#elif defined(_MSC_VER) /* HAVE_CXXABI_H */
	CHAR output[256];

	if (UnDecorateSymbolName(sym.CStr(), output, sizeof(output), UNDNAME_COMPLETE) > 0)
		result = output;
#else /* _MSC_VER */
	/* We're pretty much out of options here. */
#endif /* _MSC_VER */

	return result;
}

/**
 * Returns a human-readable type name of a type_info object.
 *
 * @param ti A type_info object.
 * @returns The type name of the object.
 */
String Utility::GetTypeName(const std::type_info& ti)
{
	return DemangleSymbolName(ti.name());
}

String Utility::GetSymbolName(const void *addr)
{
#ifdef HAVE_DLADDR
	Dl_info dli;

	if (dladdr(const_cast<void *>(addr), &dli) > 0)
		return dli.dli_sname;
#endif /* HAVE_DLADDR */

#ifdef _WIN32
	char buffer[sizeof(SYMBOL_INFO)+MAX_SYM_NAME * sizeof(TCHAR)];
	PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
	pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	pSymbol->MaxNameLen = MAX_SYM_NAME;

	DWORD64 dwAddress = (DWORD64)addr;
	DWORD64 dwDisplacement;

	IMAGEHLP_LINE64 line;
	line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

	if (SymFromAddr(GetCurrentProcess(), dwAddress, &dwDisplacement, pSymbol)) {
		char output[256];
		if (UnDecorateSymbolName(pSymbol->Name, output, sizeof(output), UNDNAME_COMPLETE))
			return String(output) + "+" + Convert::ToString(dwDisplacement);
		else
			return String(pSymbol->Name) + "+" + Convert::ToString(dwDisplacement);
	}
#endif /* _WIN32 */

	return "(unknown function)";
}

/**
 * Performs wildcard pattern matching.
 *
 * @param pattern The wildcard pattern.
 * @param text The String that should be checked.
 * @returns true if the wildcard pattern matches, false otherwise.
 */
bool Utility::Match(const String& pattern, const String& text)
{
	return (match(pattern.CStr(), text.CStr()) == 0);
}

static bool ParseIp(const String& ip, char addr[16], int *proto)
{
	if (inet_pton(AF_INET, ip.CStr(), addr + 12) == 1) {
		/* IPv4-mapped IPv6 address (::ffff:<ipv4-bits>) */
		memset(addr, 0, 10);
		memset(addr + 10, 0xff, 2);
		*proto = AF_INET;

		return true;
	}

	if (inet_pton(AF_INET6, ip.CStr(), addr) == 1) {
		*proto = AF_INET6;

		return true;
	}

	return false;
}

static void ParseIpMask(const String& ip, char mask[16], int *bits)
{
	String::SizeType slashp = ip.FindFirstOf("/");
	String uip;

	if (slashp == String::NPos) {
		uip = ip;
		*bits = 0;
	} else {
		uip = ip.SubStr(0, slashp);
		*bits = Convert::ToLong(ip.SubStr(slashp + 1));
	}

	int proto;

	if (!ParseIp(uip, mask, &proto))
		BOOST_THROW_EXCEPTION(std::invalid_argument("Invalid IP address specified."));

	if (proto == AF_INET) {
		if (*bits > 32 || *bits < 0)
			BOOST_THROW_EXCEPTION(std::invalid_argument("Mask must be between 0 and 32 for IPv4 CIDR masks."));

		*bits += 96;
	}

	if (slashp == String::NPos)
		*bits = 128;

	if (*bits > 128 || *bits < 0)
		BOOST_THROW_EXCEPTION(std::invalid_argument("Mask must be between 0 and 128 for IPv6 CIDR masks."));

	for (int i = 0; i < 16; i++) {
		int lbits = std::max(0, *bits - i * 8);

		if (lbits >= 8)
			continue;

		if (mask[i] & (0xff >> lbits))
			BOOST_THROW_EXCEPTION(std::invalid_argument("Masked-off bits must all be zero."));
	}
}

static bool IpMaskCheck(char addr[16], char mask[16], int bits)
{
	for (int i = 0; i < 16; i++) {
		if (bits < 8)
			return !((addr[i] ^ mask[i]) >> (8 - bits));

		if (mask[i] != addr[i])
			return false;

		bits -= 8;

		if (bits == 0)
			return true;
	}

	return true;
}

bool Utility::CidrMatch(const String& pattern, const String& ip)
{
	char mask[16];
	int bits;

	ParseIpMask(pattern, mask, &bits);

	char addr[16];
	int proto;

	if (!ParseIp(ip, addr, &proto))
		return false;

	return IpMaskCheck(addr, mask, bits);
}

/**
 * Returns the directory component of a path. See dirname(3) for details.
 *
 * @param path The full path.
 * @returns The directory.
 */
String Utility::DirName(const String& path)
{
	char *dir;

#ifdef _WIN32
	String dupPath = path;

	/* PathRemoveFileSpec doesn't properly handle forward slashes. */
	BOOST_FOREACH(char& ch, dupPath) {
		if (ch == '/')
			ch = '\\';
	}

	dir = strdup(dupPath.CStr());
#else /* _WIN32 */
	dir = strdup(path.CStr());
#endif /* _WIN32 */

	if (dir == NULL)
		BOOST_THROW_EXCEPTION(std::bad_alloc());

	String result;

#ifndef _WIN32
	result = dirname(dir);
#else /* _WIN32 */
	if (dir[0] != 0 && !PathRemoveFileSpec(dir)) {
		free(dir);

		BOOST_THROW_EXCEPTION(win32_error()
		    << boost::errinfo_api_function("PathRemoveFileSpec")
		    << errinfo_win32_error(GetLastError()));
	}

	result = dir;

	if (result.IsEmpty())
		result = ".";
#endif /* _WIN32 */

	free(dir);

	return result;
}

/**
 * Returns the file component of a path. See basename(3) for details.
 *
 * @param path The full path.
 * @returns The filename.
 */
String Utility::BaseName(const String& path)
{
	char *dir = strdup(path.CStr());
	String result;

	if (dir == NULL)
		BOOST_THROW_EXCEPTION(std::bad_alloc());

#ifndef _WIN32
	result = basename(dir);
#else /* _WIN32 */
	result = PathFindFileName(dir);
#endif /* _WIN32 */

	free(dir);

	return result;
}

/**
 * Null deleter. Used as a parameter for the shared_ptr constructor.
 *
 * @param - The object that should be deleted.
 */
void Utility::NullDeleter(void *)
{
	/* Nothing to do here. */
}

/**
 * Returns the current UNIX timestamp including fractions of seconds.
 *
 * @returns The current time.
 */
double Utility::GetTime(void)
{
#ifdef _WIN32
	FILETIME cft;
	GetSystemTimeAsFileTime(&cft);

	ULARGE_INTEGER ucft;
	ucft.HighPart = cft.dwHighDateTime;
	ucft.LowPart = cft.dwLowDateTime;

	SYSTEMTIME est = { 1970, 1, 4, 1, 0, 0, 0, 0};
	FILETIME eft;
	SystemTimeToFileTime(&est, &eft);

	ULARGE_INTEGER ueft;
	ueft.HighPart = eft.dwHighDateTime;
	ueft.LowPart = eft.dwLowDateTime;

	return ((ucft.QuadPart - ueft.QuadPart) / 10000) / 1000.0;
#else /* _WIN32 */
	struct timeval tv;

	int rc = gettimeofday(&tv, NULL);
	VERIFY(rc >= 0);

	return tv.tv_sec + tv.tv_usec / 1000000.0;
#endif /* _WIN32 */
}

/**
 * Returns the ID of the current process.
 *
 * @returns The PID.
 */
pid_t Utility::GetPid(void)
{
#ifndef _WIN32
	return getpid();
#else /* _WIN32 */
	return GetCurrentProcessId();
#endif /* _WIN32 */
}

/**
 * Sleeps for the specified amount of time.
 *
 * @param timeout The timeout in seconds.
 */
void Utility::Sleep(double timeout)
{
#ifndef _WIN32
	usleep(timeout * 1000 * 1000);
#else /* _WIN32 */
	::Sleep(timeout * 1000);
#endif /* _WIN32 */
}

/**
 * Generates a new unique ID.
 *
 * @returns The new unique ID.
 */
String Utility::NewUniqueID(void)
{
	static boost::mutex mutex;
	static int next_id = 0;

	/* I'd much rather use UUIDs but RHEL is way too cool to have
	 * a semi-recent version of boost. Yay. */

	String id;

	char buf[128];
	if (gethostname(buf, sizeof(buf)) == 0)
		id = String(buf) + "-";

	id += Convert::ToString((long)Utility::GetTime()) + "-";

	{
		boost::mutex::scoped_lock lock(mutex);
		id += Convert::ToString(next_id);
		next_id++;
	}

	return id;
}

#ifdef _WIN32
static bool GlobHelper(const String& pathSpec, int type, std::vector<String>& files, std::vector<String>& dirs)
{
	HANDLE handle;
	WIN32_FIND_DATA wfd;

	handle = FindFirstFile(pathSpec.CStr(), &wfd);

	if (handle == INVALID_HANDLE_VALUE) {
		DWORD errorCode = GetLastError();

		if (errorCode == ERROR_FILE_NOT_FOUND)
			return false;

		BOOST_THROW_EXCEPTION(win32_error()
			<< boost::errinfo_api_function("FindFirstFile")
			<< errinfo_win32_error(errorCode)
			<< boost::errinfo_file_name(pathSpec));
	}

	do {
		if (strcmp(wfd.cFileName, ".") == 0 || strcmp(wfd.cFileName, "..") == 0)
			continue;

		String path = Utility::DirName(pathSpec) + "/" + wfd.cFileName;

		if ((wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (type & GlobDirectory))
			dirs.push_back(path);
		else if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (type & GlobFile))
			files.push_back(path);
	} while (FindNextFile(handle, &wfd));

	if (!FindClose(handle)) {
		BOOST_THROW_EXCEPTION(win32_error()
			<< boost::errinfo_api_function("FindClose")
			<< errinfo_win32_error(GetLastError()));
	}

	return true;
}
#endif /* _WIN32 */

/**
 * Calls the specified callback for each file matching the path specification.
 *
 * @param pathSpec The path specification.
 * @param callback The callback which is invoked for each matching file.
 * @param type The file type (a combination of GlobFile and GlobDirectory)
 */
bool Utility::Glob(const String& pathSpec, const boost::function<void (const String&)>& callback, int type)
{
	std::vector<String> files, dirs;

#ifdef _WIN32
	std::vector<String> tokens;
	boost::algorithm::split(tokens, pathSpec, boost::is_any_of("\\/"));

	String part1;

	for (std::vector<String>::size_type i = 0; i < tokens.size() - 1; i++) {
		const String& token = tokens[i];

		if (!part1.IsEmpty())
			part1 += "/";

		part1 += token;

		if (token.FindFirstOf("?*") != String::NPos) {
			String part2;

			for (std::vector<String>::size_type k = i + 1; k < tokens.size(); k++) {
				if (!part2.IsEmpty())
					part2 += "/";

				part2 += tokens[k];
			}

			std::vector<String> files2, dirs2;

			if (!GlobHelper(part1, GlobDirectory, files2, dirs2))
				return false;

			BOOST_FOREACH(const String& dir, dirs2) {
				if (!Utility::Glob(dir + "/" + part2, callback, type))
					return false;
			}

			return true;
		}
	}

	if (!GlobHelper(part1 + "/" + tokens[tokens.size() - 1], type, files, dirs))
		return false;
#else /* _WIN32 */
	glob_t gr;

	int rc = glob(pathSpec.CStr(), GLOB_ERR | GLOB_NOSORT, NULL, &gr);

	if (rc < 0) {
		if (rc == GLOB_NOMATCH)
			return false;

		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("glob")
		    << boost::errinfo_errno(errno)
		    << boost::errinfo_file_name(pathSpec));
	}

	if (gr.gl_pathc == 0) {
		globfree(&gr);
		return false;
	}

	size_t left;
	char **gp;
	for (gp = gr.gl_pathv, left = gr.gl_pathc; left > 0; gp++, left--) {
		struct stat statbuf;

		if (stat(*gp, &statbuf) < 0)
			continue;

		if (!S_ISDIR(statbuf.st_mode) && !S_ISREG(statbuf.st_mode))
			continue;

		if (S_ISDIR(statbuf.st_mode) && (type & GlobDirectory))
			dirs.push_back(*gp);
		else if (!S_ISDIR(statbuf.st_mode) && (type & GlobFile))
			files.push_back(*gp);
	}

	globfree(&gr);
#endif /* _WIN32 */

	std::sort(files.begin(), files.end());
	BOOST_FOREACH(const String& cpath, files) {
		callback(cpath);
	}

	std::sort(dirs.begin(), dirs.end());
	BOOST_FOREACH(const String& cpath, dirs) {
		callback(cpath);
	}

	return true;
}

/**
 * Calls the specified callback for each file in the specified directory
 * or any of its child directories if the file name matches the specified
 * pattern.
 *
 * @param path The path.
 * @param pattern The pattern.
 * @param callback The callback which is invoked for each matching file.
 * @param type The file type (a combination of GlobFile and GlobDirectory)
 */
bool Utility::GlobRecursive(const String& path, const String& pattern, const boost::function<void (const String&)>& callback, int type)
{
	std::vector<String> files, dirs, alldirs;

#ifdef _WIN32
	HANDLE handle;
	WIN32_FIND_DATA wfd;

	String pathSpec = path + "/*";

	handle = FindFirstFile(pathSpec.CStr(), &wfd);

	if (handle == INVALID_HANDLE_VALUE) {
		DWORD errorCode = GetLastError();

		if (errorCode == ERROR_FILE_NOT_FOUND)
			return false;

		BOOST_THROW_EXCEPTION(win32_error()
		    << boost::errinfo_api_function("FindFirstFile")
			<< errinfo_win32_error(errorCode)
		    << boost::errinfo_file_name(pathSpec));
	}

	do {
		if (strcmp(wfd.cFileName, ".") == 0 || strcmp(wfd.cFileName, "..") == 0)
			continue;

		String cpath = path + "/" + wfd.cFileName;

		if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			alldirs.push_back(cpath);

		if (!Utility::Match(pattern, wfd.cFileName))
			continue;

		if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (type & GlobFile))
			files.push_back(cpath);

		if ((wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (type & GlobDirectory))
			dirs.push_back(cpath);
	} while (FindNextFile(handle, &wfd));

	if (!FindClose(handle)) {
		BOOST_THROW_EXCEPTION(win32_error()
		    << boost::errinfo_api_function("FindClose")
		    << errinfo_win32_error(GetLastError()));
	}
#else /* _WIN32 */
	DIR *dirp;

	dirp = opendir(path.CStr());

	if (dirp == NULL)
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("opendir")
		    << boost::errinfo_errno(errno)
		    << boost::errinfo_file_name(path));

	while (dirp) {
		dirent *pent;

		errno = 0;
		pent = readdir(dirp);
		if (!pent && errno != 0) {
			closedir(dirp);

			BOOST_THROW_EXCEPTION(posix_error()
			    << boost::errinfo_api_function("readdir")
			    << boost::errinfo_errno(errno)
			    << boost::errinfo_file_name(path));
		}

		if (!pent)
			break;

		if (strcmp(pent->d_name, ".") == 0 || strcmp(pent->d_name, "..") == 0)
			continue;

		String cpath = path + "/" + pent->d_name;

		struct stat statbuf;

		if (lstat(cpath.CStr(), &statbuf) < 0)
			continue;

		if (S_ISDIR(statbuf.st_mode))
			alldirs.push_back(cpath);

		if (!Utility::Match(pattern, pent->d_name))
			continue;

		if (S_ISDIR(statbuf.st_mode) && (type & GlobDirectory))
			dirs.push_back(cpath);

		if (!S_ISDIR(statbuf.st_mode) && (type & GlobFile))
			files.push_back(cpath);
	}

	closedir(dirp);

#endif /* _WIN32 */

	std::sort(files.begin(), files.end());
	BOOST_FOREACH(const String& cpath, files) {
		callback(cpath);
	}

	std::sort(dirs.begin(), dirs.end());
	BOOST_FOREACH(const String& cpath, dirs) {
		callback(cpath);
	}

	std::sort(alldirs.begin(), alldirs.end());
	BOOST_FOREACH(const String& cpath, alldirs) {
		GlobRecursive(cpath, pattern, callback, type);
	}

	return true;
}


void Utility::MkDir(const String& path, int flags)
{
#ifndef _WIN32
	if (mkdir(path.CStr(), flags) < 0 && errno != EEXIST) {
#else /*_ WIN32 */
	if (mkdir(path.CStr()) < 0 && errno != EEXIST) {
#endif /* _WIN32 */
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("mkdir")
		    << boost::errinfo_errno(errno));
	}
}

void Utility::MkDirP(const String& path, int mode)
{
	size_t pos = 0;

	while (pos != String::NPos) {
		pos = path.Find("/", pos + 1);
		MkDir(path.SubStr(0, pos), mode);
	}
}

void Utility::RemoveDirRecursive(const String& path)
{
	std::vector<String> paths;
	Utility::GlobRecursive(path, "*", boost::bind(&Utility::CollectPaths, _1, boost::ref(paths)), GlobFile | GlobDirectory);

	/* This relies on the fact that GlobRecursive lists the parent directory
	   first before recursing into subdirectories. */
	std::reverse(paths.begin(), paths.end());

	BOOST_FOREACH(const String& path, paths) {
		if (remove(path.CStr()) < 0)
			BOOST_THROW_EXCEPTION(posix_error()
			    << boost::errinfo_api_function("remove")
			    << boost::errinfo_errno(errno)
			    << boost::errinfo_file_name(path));
	}

#ifndef _WIN32
	if (rmdir(path.CStr()) < 0)
#else /* _WIN32 */
	if (_rmdir(path.CStr()) < 0)
#endif /* _WIN32 */
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("rmdir")
		    << boost::errinfo_errno(errno)
		    << boost::errinfo_file_name(path));
}

void Utility::CollectPaths(const String& path, std::vector<String>& paths)
{
	paths.push_back(path);
}

void Utility::CopyFile(const String& source, const String& target)
{
	std::ifstream ifs(source.CStr(), std::ios::binary);
	std::ofstream ofs(target.CStr(), std::ios::binary | std::ios::trunc);

	ofs << ifs.rdbuf();
}

/*
 * Set file permissions
 */
bool Utility::SetFileOwnership(const String& file, const String& user, const String& group)
{
#ifndef _WIN32
	errno = 0;
	struct passwd *pw = getpwnam(user.CStr());

	if (!pw) {
		if (errno == 0) {
			Log(LogCritical, "cli")
			    << "Invalid user specified: " << user;
			return false;
		} else {
			Log(LogCritical, "cli")
			    << "getpwnam() failed with error code " << errno << ", \"" << Utility::FormatErrorNumber(errno) << "\"";
			return false;
		}
	}

	errno = 0;
	struct group *gr = getgrnam(group.CStr());

	if (!gr) {
		if (errno == 0) {
			Log(LogCritical, "cli")
			    << "Invalid group specified: " << group;
			return false;
		} else {
			Log(LogCritical, "cli")
			    << "getgrnam() failed with error code " << errno << ", \"" << Utility::FormatErrorNumber(errno) << "\"";
			return false;
		}
	}

	if (chown(file.CStr(), pw->pw_uid, gr->gr_gid) < 0) {
		Log(LogCritical, "cli")
		    << "chown() failed with error code " << errno << ", \"" << Utility::FormatErrorNumber(errno) << "\"";
		return false;
	}
#endif /* _WIN32 */

	return true;
}

#ifndef _WIN32
void Utility::SetNonBlocking(int fd, bool nb)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0) {
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("fcntl")
		    << boost::errinfo_errno(errno));
	}

	if (nb)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;

	if (fcntl(fd, F_SETFL, flags) < 0) {
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("fcntl")
		    << boost::errinfo_errno(errno));
	}
}

void Utility::SetCloExec(int fd, bool cloexec)
{
	int flags = fcntl(fd, F_GETFD, 0);

	if (flags < 0) {
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("fcntl")
		    << boost::errinfo_errno(errno));
	}

	if (cloexec)
		flags |= FD_CLOEXEC;
	else
		flags &= ~FD_CLOEXEC;

	if (fcntl(fd, F_SETFD, flags) < 0) {
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("fcntl")
		    << boost::errinfo_errno(errno));
	}
}
#endif /* _WIN32 */

void Utility::SetNonBlockingSocket(SOCKET s, bool nb)
{
#ifndef _WIN32
	SetNonBlocking(s, nb);
#else /* _WIN32 */
	unsigned long lflag = nb;
	ioctlsocket(s, FIONBIO, &lflag);
#endif /* _WIN32 */
}

void Utility::QueueAsyncCallback(const boost::function<void (void)>& callback, SchedulerPolicy policy)
{
	Application::GetTP().Post(callback, policy);
}

String Utility::NaturalJoin(const std::vector<String>& tokens)
{
	String result;

	for (std::vector<String>::size_type i = 0; i < tokens.size(); i++) {
		result += tokens[i];

		if (tokens.size() > i + 1) {
			if (i < tokens.size() - 2)
				result += ", ";
			else if (i == tokens.size() - 2)
				result += " and ";
		}
	}

	return result;
}

String Utility::Join(const Array::Ptr& tokens, char separator)
{
	String result;
	bool first = true;

	ObjectLock olock(tokens);
	BOOST_FOREACH(const Value& vtoken, tokens) {
		String token = Convert::ToString(vtoken);
		boost::algorithm::replace_all(token, "\\", "\\\\");

		char sep_before[2], sep_after[3];
		sep_before[0] = separator;
		sep_before[1] = '\0';
		sep_after[0] = '\\';
		sep_after[1] = separator;
		sep_after[2] = '\0';
		boost::algorithm::replace_all(token, sep_before, sep_after);

		if (first)
			first = false;
		else
			result += String(1, separator);

		result += token;
	}

	return result;
}

String Utility::FormatDuration(double duration)
{
	std::vector<String> tokens;
	String result;

	if (duration >= 86400) {
		int days = duration / 86400;
		tokens.push_back(Convert::ToString(days) + (days != 1 ? " days" : " day"));
		duration = static_cast<int>(duration) % 86400;
	}

	if (duration >= 3600) {
		int hours = duration / 3600;
		tokens.push_back(Convert::ToString(hours) + (hours != 1 ? " hours" : " hour"));
		duration = static_cast<int>(duration) % 3600;
	}

	if (duration >= 60) {
		int minutes = duration / 60;
		tokens.push_back(Convert::ToString(minutes) + (minutes != 1 ? " minutes" : " minute"));
		duration = static_cast<int>(duration) % 60;
	}

	if (duration >= 1) {
		int seconds = duration;
		tokens.push_back(Convert::ToString(seconds) + (seconds != 1 ? " seconds" : " second"));
	}

	if (tokens.size() == 0) {
		int milliseconds = std::floor(duration * 1000);
		if (milliseconds >= 1)
			tokens.push_back(Convert::ToString(milliseconds) + (milliseconds != 1 ? " milliseconds" : " millisecond"));
		else
			tokens.push_back("less than 1 millisecond");
	}

	return NaturalJoin(tokens);
}

String Utility::FormatDateTime(const char *format, double ts)
{
	char timestamp[128];
	time_t tempts = (time_t)ts; /* We don't handle sub-second timestamps here just yet. */
	tm tmthen;

#ifdef _MSC_VER
	tm *temp = localtime(&tempts);

	if (temp == NULL) {
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("localtime")
		    << boost::errinfo_errno(errno));
	}

	tmthen = *temp;
#else /* _MSC_VER */
	if (localtime_r(&tempts, &tmthen) == NULL) {
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("localtime_r")
		    << boost::errinfo_errno(errno));
	}
#endif /* _MSC_VER */

	strftime(timestamp, sizeof(timestamp), format, &tmthen);

	return timestamp;
}

String Utility::FormatErrorNumber(int code) {
	std::ostringstream msgbuf;

#ifdef _WIN32
	char *message;
	String result = "Unknown error.";

	DWORD rc = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM, NULL, code, 0, (char *)&message,
		0, NULL);

	if (rc != 0) {
		result = String(message);
		LocalFree(message);

		/* remove trailing new-line characters */
		boost::algorithm::trim_right(result);
	}

	msgbuf << code << ", \"" << result << "\"";
#else
	msgbuf << strerror(code);
#endif
	return msgbuf.str();
}

String Utility::EscapeShellCmd(const String& s)
{
	String result;
	size_t prev_quote = String::NPos;
	int index = -1;

	BOOST_FOREACH(char ch, s) {
		bool escape = false;

		index++;

#ifdef _WIN32
		if (ch == '%' || ch == '"' || ch == '\'')
			escape = true;
#else /* _WIN32 */
		if (ch == '"' || ch == '\'') {
			/* Find a matching closing quotation character. */
			if (prev_quote == String::NPos && (prev_quote = s.FindFirstOf(ch, index + 1)) != String::NPos)
				; /* Empty statement. */
			else if (prev_quote != String::NPos && s[prev_quote] == ch)
				prev_quote = String::NPos;
			else
				escape = true;
		}
#endif /* _WIN32 */

		if (ch == '#' || ch == '&' || ch == ';' || ch == '`' || ch == '|' ||
		    ch == '*' || ch == '?' || ch == '~' || ch == '<' || ch == '>' ||
		    ch == '^' || ch == '(' || ch == ')' || ch == '[' || ch == ']' ||
		    ch == '{' || ch == '}' || ch == '$' || ch == '\\' || ch == '\x0A' ||
		    ch == '\xFF')
			escape = true;

		if (escape)
#ifdef _WIN32
			result += '^';
#else /* _WIN32 */
			result += '\\';
#endif /* _WIN32 */

		result += ch;
	}

	return result;
}

String Utility::EscapeShellArg(const String& s)
{
	String result;

#ifdef _WIN32
	result = "\"";
#else /* _WIN32 */
	result = "'";
#endif /* _WIN32 */

	BOOST_FOREACH(char ch, s) {
#ifdef _WIN32
		if (ch == '"' || ch == '%') {
			result += ' ';
		}
#else /* _WIN32 */
		if (ch == '\'')
			result += "'\\'";
#endif
		result += ch;
	}

#ifdef _WIN32
	result += '"';
#else /* _WIN32 */
	result += '\'';
#endif /* _WIN32 */

	return result;
}

#ifdef _WIN32
String Utility::EscapeCreateProcessArg(const String& arg)
{
	if (arg.FindFirstOf(" \t\n\v\"") == String::NPos)
		return arg;

	String result = "\"";

	for (String::ConstIterator it = arg.Begin(); ; it++) {
		int numBackslashes = 0;

		while (it != arg.End() && *it == '\\') {
			it++;
			numBackslashes++;
		}

		if (it == arg.End()) {
			result.Append(numBackslashes * 2, '\\');
			break;
		} else if (*it == '"') {
			result.Append(numBackslashes * 2, '\\');
			result.Append(1, *it);
		} else {
			result.Append(numBackslashes, '\\');
			result.Append(1, *it);
		}
	}

	result += "\"";

	return result;
}
#endif /* _WIN32 */

#ifdef _WIN32
static void WindowsSetThreadName(const char *name)
{
	THREADNAME_INFO info;
	info.dwType = 0x1000;
	info.szName = name;
	info.dwThreadID = -1;
	info.dwFlags = 0;

	__try {
		RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR *)&info);
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		/* Nothing to do here. */
	}
}
#endif /* _WIN32 */

void Utility::SetThreadName(const String& name, bool os)
{
	m_ThreadName.reset(new String(name));

	if (!os)
		return;

#ifdef _WIN32
	WindowsSetThreadName(name.CStr());
#endif /* _WIN32 */

#ifdef HAVE_PTHREAD_SET_NAME_NP
	pthread_set_name_np(pthread_self(), name.CStr());
#endif /* HAVE_PTHREAD_SET_NAME_NP */

#ifdef HAVE_PTHREAD_SETNAME_NP
#	ifdef __APPLE__
	pthread_setname_np(name.CStr());
#	else /* __APPLE__ */
	String tname = name.SubStr(0, 15);
	pthread_setname_np(pthread_self(), tname.CStr());
#	endif /* __APPLE__ */
#endif /* HAVE_PTHREAD_SETNAME_NP */
}

String Utility::GetThreadName(void)
{
	String *name = m_ThreadName.get();

	if (!name) {
		std::ostringstream idbuf;
		idbuf << boost::this_thread::get_id();
		return idbuf.str();
	}

	return *name;
}

unsigned long Utility::SDBM(const String& str, size_t len)
{
	unsigned long hash = 0;
	size_t current = 0;

	BOOST_FOREACH(char c, str) {
		if (current >= len)
			break;

		hash = c + (hash << 6) + (hash << 16) - hash;

		current++;
	}

	return hash;
}

int Utility::CompareVersion(const String& v1, const String& v2)
{
	std::vector<String> tokensv1, tokensv2;
	boost::algorithm::split(tokensv1, v1, boost::is_any_of("."));
	boost::algorithm::split(tokensv2, v2, boost::is_any_of("."));

	for (std::vector<String>::size_type i = 0; i < tokensv2.size() - tokensv1.size(); i++)
		tokensv1.push_back("0");

	for (std::vector<String>::size_type i = 0; i < tokensv1.size() - tokensv2.size(); i++)
		tokensv2.push_back("0");

	for (std::vector<String>::size_type i = 0; i < tokensv1.size(); i++) {
		if (Convert::ToLong(tokensv2[i]) > Convert::ToLong(tokensv1[i]))
			return 1;
		else if (Convert::ToLong(tokensv2[i]) < Convert::ToLong(tokensv1[i]))
			return -1;
	}

	return 0;
}

String Utility::GetHostName(void)
{
	char name[255];

	if (gethostname(name, sizeof(name)) < 0)
		return "localhost";

	return name;
}

/**
 * Returns the fully-qualified domain name for the host
 * we're running on.
 *
 * @returns The FQDN.
 */
String Utility::GetFQDN(void)
{
	String hostname = GetHostName();

	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_CANONNAME;

	addrinfo *result;
	int rc = getaddrinfo(hostname.CStr(), NULL, &hints, &result);

	if (rc != 0)
		result = NULL;

	if (result) {
		if (strcmp(result->ai_canonname, "localhost") != 0)
			hostname = result->ai_canonname;

		freeaddrinfo(result);
	}

	return hostname;
}

int Utility::Random(void)
{
#ifdef _WIN32
	return rand();
#else /* _WIN32 */
	unsigned int *seed = m_RandSeed.get();

	if (!seed) {
		seed = new unsigned int(Utility::GetTime());
		m_RandSeed.reset(seed);
	}

	return rand_r(seed);
#endif /* _WIN32 */
}

tm Utility::LocalTime(time_t ts)
{
#ifdef _MSC_VER
	tm *result = localtime(&ts);

	if (result == NULL) {
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("localtime")
		    << boost::errinfo_errno(errno));
	}

	return *result;
#else /* _MSC_VER */
	tm result;

	if (localtime_r(&ts, &result) == NULL) {
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("localtime_r")
		    << boost::errinfo_errno(errno));
	}

	return result;
#endif /* _MSC_VER */
}

bool Utility::PathExists(const String& path)
{
#ifndef _WIN32
	struct stat statbuf;
	return (lstat(path.CStr(), &statbuf) >= 0);
#else /* _WIN32 */
	struct _stat statbuf;
	return (_stat(path.CStr(), &statbuf) >= 0);
#endif /* _WIN32 */
}

Value Utility::LoadJsonFile(const String& path)
{
	std::ifstream fp;
	fp.open(path.CStr());

	String json((std::istreambuf_iterator<char>(fp)), std::istreambuf_iterator<char>());

	fp.close();

	if (fp.fail())
		BOOST_THROW_EXCEPTION(std::runtime_error("Could not read JSON file '" + path + "'."));

	return JsonDecode(json);
}

void Utility::SaveJsonFile(const String& path, const Value& value)
{
	String tempPath = path + ".tmp";

	std::ofstream fp(tempPath.CStr(), std::ofstream::out | std::ostream::trunc);
	fp.exceptions(std::ofstream::failbit | std::ofstream::badbit);
	fp << JsonEncode(value);
	fp.close();

#ifdef _WIN32
	_unlink(path.CStr());
#endif /* _WIN32 */

	if (rename(tempPath.CStr(), path.CStr()) < 0) {
		BOOST_THROW_EXCEPTION(posix_error()
		    << boost::errinfo_api_function("rename")
		    << boost::errinfo_errno(errno)
		    << boost::errinfo_file_name(tempPath));
	}
}

static void HexEncode(char ch, std::ostream& os)
{
	const char *hex_chars = "0123456789ABCDEF";

	os << hex_chars[ch >> 4 & 0x0f];
	os << hex_chars[ch & 0x0f];
}

static int HexDecode(char hc)
{
	if (hc >= '0' && hc <= '9')
		return hc - '0';
	else if (hc >= 'a' && hc <= 'f')
		return hc - 'a' + 10;
	else if (hc >= 'A' && hc <= 'F')
		return hc - 'A' + 10;
	else
		BOOST_THROW_EXCEPTION(std::invalid_argument("Invalid hex character."));
}

String Utility::EscapeString(const String& s, const String& chars, const bool illegal)
{
	std::ostringstream result;
	if (illegal) {
		BOOST_FOREACH(char ch, s) {
			if (chars.FindFirstOf(ch) != String::NPos || ch == '%') {
				result << '%';
				HexEncode(ch, result);
			} else
				result << ch;
		}
	} else {
		BOOST_FOREACH(char ch, s) {
			if (chars.FindFirstOf(ch) == String::NPos || ch == '%') {
				result << '%';
				HexEncode(ch, result);
			} else
				result << ch;
		}
	}

	return result.str();
}

String Utility::UnescapeString(const String& s)
{
	std::ostringstream result;

	for (String::SizeType i = 0; i < s.GetLength(); i++) {
		if (s[i] == '%') {
			if (i + 2 > s.GetLength() - 1)
				BOOST_THROW_EXCEPTION(std::invalid_argument("Invalid escape sequence."));

			char ch = HexDecode(s[i + 1]) * 16 + HexDecode(s[i + 2]);
			result << ch;

			i += 2;
		} else
			result << s[i];
	}

	return result.str();
}
