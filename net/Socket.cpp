/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <config.h>

#include <stdio.h>
#include <ctype.h>
#include <iomanip>
#include <zlib.h>

#include <Poco/DateTime.h>
#include <Poco/DateTimeFormat.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/Net/HTTPResponse.h>

#include <SigUtil.hpp>
#include "Socket.hpp"
#include "ServerSocket.hpp"
#include "WebSocketHandler.hpp"

int SocketPoll::DefaultPollTimeoutMs = 5000;
std::atomic<bool> SocketPoll::InhibitThreadChecks(false);
std::atomic<bool> Socket::InhibitThreadChecks(false);

int Socket::createSocket(Socket::Type type)
{
    int domain = type == Type::IPv4 ? AF_INET : AF_INET6;
    return socket(domain, SOCK_STREAM | SOCK_NONBLOCK, 0);
}

// help with initialization order
namespace {
    std::vector<int> &getWakeupsArray()
    {
        static std::vector<int> pollWakeups;
        return pollWakeups;
    }
    std::mutex &getPollWakeupsMutex()
    {
        static std::mutex pollWakeupsMutex;
        return pollWakeupsMutex;
    }
}

SocketPoll::SocketPoll(const std::string& threadName)
    : _name(threadName),
      _stop(false),
      _threadStarted(false),
      _threadFinished(false),
      _owner(std::this_thread::get_id())
{
    // Create the wakeup fd.
    if (::pipe2(_wakeup, O_CLOEXEC | O_NONBLOCK) == -1)
    {
        throw std::runtime_error("Failed to allocate pipe for SocketPoll [" + threadName + "] waking.");
    }

    std::lock_guard<std::mutex> lock(getPollWakeupsMutex());
    getWakeupsArray().push_back(_wakeup[1]);
}

SocketPoll::~SocketPoll()
{
    joinThread();

    {
        std::lock_guard<std::mutex> lock(getPollWakeupsMutex());
        auto it = std::find(getWakeupsArray().begin(),
                            getWakeupsArray().end(),
                            _wakeup[1]);

        if (it != getWakeupsArray().end())
            getWakeupsArray().erase(it);
    }

    ::close(_wakeup[0]);
    ::close(_wakeup[1]);
    _wakeup[0] = -1;
    _wakeup[1] = -1;
}

void SocketPoll::startThread()
{
    if (!_threadStarted)
    {
        _threadStarted = true;
        try
        {
            _thread = std::thread(&SocketPoll::pollingThreadEntry, this);
        }
        catch (const std::exception& exc)
        {
            LOG_ERR("Failed to start poll thread: " << exc.what());
            _threadStarted = false;
        }
    }
}

void SocketPoll::joinThread()
{
    addCallback([this](){ removeSockets(); });
    stop();
    if (_threadStarted && _thread.joinable())
    {
        if (_thread.get_id() == std::this_thread::get_id())
            LOG_ERR("DEADLOCK PREVENTED: joining own thread!");
        else
        {
            _thread.join();
            _threadStarted = false;
        }
    }
}

void SocketPoll::wakeupWorld()
{
    for (const auto& fd : getWakeupsArray())
        wakeup(fd);
}

void ServerSocket::dumpState(std::ostream& os)
{
    os << "\t" << getFD() << "\t<accept>\n";
}


void SocketDisposition::execute()
{
    // We should have hard ownership of this socket.
    assert(_socket->getThreadOwner() == std::this_thread::get_id());
    if (_socketMove)
    {
        // Drop pretentions of ownership before _socketMove.
        _socket->setThreadOwner(std::thread::id(0));
        _socketMove(_socket);
    }
    _socketMove = nullptr;
}

const int WebSocketHandler::InitialPingDelayMs = 25;
const int WebSocketHandler::PingFrequencyMs = 18 * 1000;

void WebSocketHandler::dumpState(std::ostream& os)
{
    os << (_shuttingDown ? "shutd " : "alive ")
       << std::setw(5) << _pingTimeUs/1000. << "ms ";
    if (_wsPayload.size() > 0)
        Util::dumpHex(os, "\t\tws queued payload:\n", "\t\t", _wsPayload);
    os << "\n";
}

void StreamSocket::dumpState(std::ostream& os)
{
    int timeoutMaxMs = SocketPoll::DefaultPollTimeoutMs;
    int events = getPollEvents(std::chrono::steady_clock::now(), timeoutMaxMs);
    os << "\t" << getFD() << "\t" << events << "\t"
       << _inBuffer.size() << "\t" << _outBuffer.size() << "\t"
       << " r: " << _bytesRecvd << "\t w: " << _bytesSent << "\t";
    _socketHandler->dumpState(os);
    if (_inBuffer.size() > 0)
        Util::dumpHex(os, "\t\tinBuffer:\n", "\t\t", _inBuffer);
    if (_outBuffer.size() > 0)
        Util::dumpHex(os, "\t\toutBuffer:\n", "\t\t", _inBuffer);
}

void StreamSocket::send(Poco::Net::HTTPResponse& response)
{
    response.set("User-Agent", HTTP_AGENT_STRING);
    response.set("Date", Poco::DateTimeFormatter::format(Poco::Timestamp(), Poco::DateTimeFormat::HTTP_FORMAT));

    std::ostringstream oss;
    response.write(oss);

    send(oss.str());
}

void SocketPoll::dumpState(std::ostream& os)
{
    // FIXME: NOT thread-safe! _pollSockets is modified from the polling thread!
    os << " Poll [" << _pollSockets.size() << "] - wakeup r: "
       << _wakeup[0] << " w: " << _wakeup[1] << "\n";
    if (_newCallbacks.size() > 0)
        os << "\tcallbacks: " << _newCallbacks.size() << "\n";
    os << "\tfd\tevents\trsize\twsize\n";
    for (auto &i : _pollSockets)
        i->dumpState(os);
}

/// Returns true on success only.
bool ServerSocket::bind(Type type, int port)
{
    // Enable address reuse to avoid stalling after
    // recycling, when previous socket is TIME_WAIT.
    //TODO: Might be worth refactoring out.
    const int reuseAddress = 1;
    constexpr unsigned int len = sizeof(reuseAddress);
    ::setsockopt(getFD(), SOL_SOCKET, SO_REUSEADDR, &reuseAddress, len);

    int rc;

    if (_type == Socket::Type::IPv4)
    {
        struct sockaddr_in addrv4;
        std::memset(&addrv4, 0, sizeof(addrv4));
        addrv4.sin_family = AF_INET;
        addrv4.sin_port = htons(port);
        if (type == Type::Public)
            addrv4.sin_addr.s_addr = type == htonl(INADDR_ANY);
        else
            addrv4.sin_addr.s_addr = type == htonl(INADDR_LOOPBACK);

        rc = ::bind(getFD(), (const sockaddr *)&addrv4, sizeof(addrv4));
    }
    else
    {
        struct sockaddr_in6 addrv6;
        std::memset(&addrv6, 0, sizeof(addrv6));
        addrv6.sin6_family = AF_INET6;
        addrv6.sin6_port = htons(port);
        if (type == Type::Public)
            addrv6.sin6_addr = in6addr_any;
        else
            addrv6.sin6_addr = in6addr_loopback;

        int ipv6only = _type == Socket::Type::All ? 0 : 1;
        if (::setsockopt(getFD(), IPPROTO_IPV6, IPV6_V6ONLY, (char*)&ipv6only, sizeof(ipv6only)) == -1)
            LOG_SYS("Failed set ipv6 socket to %d" << ipv6only);

        rc = ::bind(getFD(), (const sockaddr *)&addrv6, sizeof(addrv6));
    }

    if (rc)
        LOG_SYS("Failed to bind to: " << (_type == Socket::Type::IPv4 ? "IPv4" : "IPv6") << " port: " << port);

    return rc == 0;
}


namespace HttpHelper
{
    void sendUncompressedFileContent(const std::shared_ptr<StreamSocket>& socket,
                                     const std::string& path,
                                     const int bufferSize)
    {
        std::ifstream file(path, std::ios::binary);
        std::unique_ptr<char[]> buf(new char[bufferSize]);
        do
        {
            file.read(&buf[0], bufferSize);
            const int size = file.gcount();
            if (size > 0)
                socket->send(&buf[0], size, true);
            else
                break;
        }
        while (file);
    }

    void sendDeflatedFileContent(const std::shared_ptr<StreamSocket>& socket,
                                 const std::string& path,
                                 const int fileSize)
    {
        // FIXME: Should compress once ahead of time
        // compression of bundle.js takes significant time:
        //   200's ms for level 9 (468k), 72ms for level 1(587k)
        //   down from 2Mb.
        if (fileSize > 0)
        {
            std::ifstream file(path, std::ios::binary);
            std::unique_ptr<char[]> buf(new char[fileSize]);
            file.read(&buf[0], fileSize);

            static const unsigned int Level = 1;
            const long unsigned int size = file.gcount();
            long unsigned int compSize = compressBound(size);
            std::unique_ptr<char[]> cbuf(new char[compSize]);
            compress2((Bytef *)&cbuf[0], &compSize, (Bytef *)&buf[0], size, Level);

            if (size > 0)
                socket->send(&cbuf[0], compSize, true);
        }
    }

    void sendFile(const std::shared_ptr<StreamSocket>& socket,
                  const std::string& path,
                  const std::string& mediaType,
                  Poco::Net::HTTPResponse& response,
                  const bool noCache,
                  const bool deflate,
                  const bool headerOnly)
    {
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
        {
            LOG_WRN("#" << socket->getFD() << ": Failed to stat [" << path << "]. File will not be sent.");
            throw Poco::FileNotFoundException("Failed to stat [" + path + "]. File will not be sent.");
        }

        if (!noCache)
        {
            // 60 * 60 * 24 * 128 (days) = 11059200
            response.set("Cache-Control", "max-age=11059200");
            response.set("ETag", "\"" LOOLWSD_VERSION_HASH "\"");
        }

        response.setContentType(mediaType);
        response.add("X-Content-Type-Options", "nosniff");

        int bufferSize = std::min(st.st_size, (off_t)Socket::MaximumSendBufferSize);
        if (st.st_size >= socket->getSendBufferSize())
        {
            socket->setSocketBufferSize(bufferSize);
            bufferSize = socket->getSendBufferSize();
        }

        // Disable deflate for now - until we can cache deflated data.
        // FIXME: IE/Edge doesn't work well with deflate, so check with
        // IE/Edge before enabling the deflate again
        if (!deflate || true)
        {
            response.setContentLength(st.st_size);
            LOG_TRC("#" << socket->getFD() << ": Sending " <<
                    (headerOnly ? "header for " : "") << " file [" << path << "].");
            socket->send(response);

            if (!headerOnly)
                sendUncompressedFileContent(socket, path, bufferSize);
        }
        else
        {
            response.set("Content-Encoding", "deflate");
            LOG_TRC("#" << socket->getFD() << ": Sending " <<
                    (headerOnly ? "header for " : "") << " file [" << path << "].");
            socket->send(response);

            if (!headerOnly)
                sendDeflatedFileContent(socket, path, st.st_size);
        }
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
