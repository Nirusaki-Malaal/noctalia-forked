#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    bool load(const std::string& path);
    void play();
    void stop();

    // Callback is invoked with RGBA data, width, height. Note: called from a background thread!
    void setFrameCallback(std::function<void(const uint8_t*, int, int)> cb);

private:
    static GstFlowReturn onNewSample(GstAppSink* sink, gpointer data);

    GstElement* m_pipeline = nullptr;
    GstElement* m_appsink = nullptr;
    std::function<void(const uint8_t*, int, int)> m_callback;
};
