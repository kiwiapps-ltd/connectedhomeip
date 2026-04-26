#include "MatterBridgeBackChannel.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <lib/support/logging/CHIPLogging.h>

namespace MatterBridge {

namespace {
// 4-byte big-endian length prefix per frame.
bool WriteAll(int fd, const void * buf, size_t len)
{
    const auto * p = static_cast<const uint8_t *>(buf);
    while (len > 0)
    {
        ssize_t n = write(fd, p, len);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool ReadAll(int fd, void * buf, size_t len)
{
    auto * p = static_cast<uint8_t *>(buf);
    while (len > 0)
    {
        ssize_t n = read(fd, p, len);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false; // EOF
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}
} // namespace

BackChannel::BackChannel() = default;

BackChannel::~BackChannel()
{
    Stop();
}

bool BackChannel::Start(const std::string & socketPath)
{
    if (mRunning.exchange(true)) return true;

    mSocketPath = socketPath;
    unlink(socketPath.c_str());

    mListenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mListenFd < 0)
    {
        ChipLogError(NotSpecified, "BackChannel: socket() failed: %s", strerror(errno));
        mRunning = false;
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(addr.sun_path))
    {
        ChipLogError(NotSpecified, "BackChannel: socket path too long: %s", socketPath.c_str());
        close(mListenFd); mListenFd = -1; mRunning = false; return false;
    }
    memcpy(addr.sun_path, socketPath.c_str(), socketPath.size());

    if (bind(mListenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        ChipLogError(NotSpecified, "BackChannel: bind(%s) failed: %s", socketPath.c_str(), strerror(errno));
        close(mListenFd); mListenFd = -1; mRunning = false; return false;
    }
    chmod(socketPath.c_str(), 0600);

    if (listen(mListenFd, 1) != 0)
    {
        ChipLogError(NotSpecified, "BackChannel: listen failed: %s", strerror(errno));
        close(mListenFd); mListenFd = -1; mRunning = false; return false;
    }

    mAcceptThread = std::thread([this] { AcceptLoop(); });
    ChipLogProgress(NotSpecified, "BackChannel listening on %s", socketPath.c_str());
    return true;
}

void BackChannel::Stop()
{
    if (!mRunning.exchange(false)) return;
    if (mClientFd >= 0) { ::shutdown(mClientFd, SHUT_RDWR); close(mClientFd); mClientFd = -1; }
    if (mListenFd >= 0) { ::shutdown(mListenFd, SHUT_RDWR); close(mListenFd); mListenFd = -1; }
    if (mAcceptThread.joinable()) mAcceptThread.join();
    if (!mSocketPath.empty()) unlink(mSocketPath.c_str());
    mHasClient = false;
}

void BackChannel::AcceptLoop()
{
    while (mRunning.load())
    {
        int fd = ::accept(mListenFd, nullptr, nullptr);
        if (fd < 0)
        {
            if (!mRunning.load()) break;
            if (errno == EINTR) continue;
            ChipLogError(NotSpecified, "BackChannel: accept failed: %s", strerror(errno));
            continue;
        }
        ChipLogProgress(NotSpecified, "BackChannel: client connected");
        mClientFd  = fd;
        mHasClient = true;
        ClientLoop(fd);
        mHasClient = false;
        mClientFd  = -1;
        ChipLogProgress(NotSpecified, "BackChannel: client disconnected");
    }
}

void BackChannel::ClientLoop(int fd)
{
    while (mRunning.load())
    {
        uint8_t lenBuf[4];
        if (!ReadAll(fd, lenBuf, 4)) break;
        uint32_t len = (uint32_t(lenBuf[0]) << 24) | (uint32_t(lenBuf[1]) << 16) |
                       (uint32_t(lenBuf[2]) << 8)  | uint32_t(lenBuf[3]);
        if (len > (1u << 20))
        {
            ChipLogError(NotSpecified, "BackChannel: oversized frame %u — disconnecting", len);
            break;
        }
        std::vector<uint8_t> payload(len);
        if (len > 0 && !ReadAll(fd, payload.data(), len)) break;
        std::string s(reinterpret_cast<const char *>(payload.data()), len);

        Json::Value msg;
        Json::CharReaderBuilder b;
        std::unique_ptr<Json::CharReader> reader(b.newCharReader());
        std::string errs;
        if (!reader->parse(s.data(), s.data() + s.size(), &msg, &errs))
        {
            ChipLogError(NotSpecified, "BackChannel: JSON parse error: %s", errs.c_str());
            continue;
        }

        // If this is a response to a pending request, deliver it.
        if (msg.isMember("ResponseId") && msg["ResponseId"].isUInt64())
        {
            uint64_t id = msg["ResponseId"].asUInt64();
            std::shared_ptr<Pending> p;
            {
                std::lock_guard<std::mutex> lk(mPendingMutex);
                auto it = mPending.find(id);
                if (it != mPending.end()) { p = it->second; mPending.erase(it); }
            }
            if (p)
            {
                std::lock_guard<std::mutex> lk(p->mu);
                p->response = std::move(msg);
                p->ready    = true;
                p->cv.notify_all();
            }
            continue;
        }
        // Else: fire-and-forget event from Swift (for now, we just log).
        ChipLogProgress(NotSpecified, "BackChannel inbound event: %s",
                        msg.isMember("Name") ? msg["Name"].asString().c_str() : "(unnamed)");
    }
}

bool BackChannel::WriteFrame(int fd, const std::string & json)
{
    uint32_t len = static_cast<uint32_t>(json.size());
    uint8_t lenBuf[4] = {
        static_cast<uint8_t>((len >> 24) & 0xFF),
        static_cast<uint8_t>((len >> 16) & 0xFF),
        static_cast<uint8_t>((len >> 8) & 0xFF),
        static_cast<uint8_t>(len & 0xFF),
    };
    std::lock_guard<std::mutex> lk(mWriteMutex);
    if (!WriteAll(fd, lenBuf, 4)) return false;
    if (len > 0 && !WriteAll(fd, json.data(), len)) return false;
    return true;
}

bool BackChannel::SendEvent(Json::Value && payload)
{
    int fd = mClientFd;
    if (fd < 0) return false;
    Json::FastWriter writer;
    writer.omitEndingLineFeed();
    std::string s = writer.write(payload);
    return WriteFrame(fd, s);
}

bool BackChannel::Request(Json::Value && payload, Json::Value & out, int timeoutMs)
{
    int fd = mClientFd;
    if (fd < 0) return false;

    uint64_t id = mNextRequestId.fetch_add(1, std::memory_order_relaxed);
    payload["RequestId"] = static_cast<Json::UInt64>(id);

    auto p = std::make_shared<Pending>();
    {
        std::lock_guard<std::mutex> lk(mPendingMutex);
        mPending[id] = p;
    }

    Json::FastWriter writer;
    writer.omitEndingLineFeed();
    std::string s = writer.write(payload);
    if (!WriteFrame(fd, s))
    {
        std::lock_guard<std::mutex> lk(mPendingMutex);
        mPending.erase(id);
        return false;
    }

    std::unique_lock<std::mutex> lk(p->mu);
    if (!p->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return p->ready; }))
    {
        std::lock_guard<std::mutex> lk2(mPendingMutex);
        mPending.erase(id);
        return false;
    }
    out = std::move(p->response);
    return true;
}

BackChannel & GetBackChannel()
{
    static BackChannel sInstance;
    return sInstance;
}

} // namespace MatterBridge
