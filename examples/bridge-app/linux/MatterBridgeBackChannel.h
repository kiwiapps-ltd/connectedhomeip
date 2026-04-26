/*
 * MatterBridge: helper → Swift back-channel.
 *
 * The cluster delegates running inside chip-bridge-app need to ask Swift for
 * media (snapshots, WebRTC SDP, PTZ commands). The existing `--app-pipe`
 * named pipe is one-way (Swift → helper). This back-channel is a Unix-domain
 * stream socket the helper listens on; Swift connects and the two exchange
 * length-prefixed JSON frames. Symmetric to `Core/.../Helper/IPCFraming.swift`.
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

namespace MatterBridge {

class BackChannel
{
public:
    using EventHandler = std::function<void(const Json::Value &)>;

    BackChannel();
    ~BackChannel();

    /// Bind + listen on the given Unix-domain socket path. Spawns an
    /// accept-thread that handles a single client at a time. Idempotent.
    bool Start(const std::string & socketPath);

    /// Send a fire-and-forget event (no response expected). Thread-safe.
    /// Returns false if no client is currently connected.
    bool SendEvent(Json::Value && payload);

    /// Send a request and wait for the matching response. Used by cluster
    /// delegates that need a synchronous answer (e.g. CaptureSnapshot).
    /// Returns false on timeout or disconnect; out is populated with the
    /// response payload on success.
    bool Request(Json::Value && payload, Json::Value & out, int timeoutMs = 30000);

    void Stop();

    bool HasClient() const { return mHasClient.load(); }

private:
    void AcceptLoop();
    void ClientLoop(int fd);
    bool WriteFrame(int fd, const std::string & json);

    std::string mSocketPath;
    int mListenFd  = -1;
    int mClientFd  = -1;
    std::thread mAcceptThread;
    std::mutex mWriteMutex;
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mHasClient{false};

    // Pending requests waiting on responses, keyed by RequestId.
    struct Pending
    {
        std::mutex mu;
        std::condition_variable cv;
        Json::Value response;
        bool ready = false;
    };
    std::mutex mPendingMutex;
    std::map<uint64_t, std::shared_ptr<Pending>> mPending;
    std::atomic<uint64_t> mNextRequestId{1};
};

/// Process-wide singleton.
BackChannel & GetBackChannel();

} // namespace MatterBridge
