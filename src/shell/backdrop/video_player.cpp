#include "video_player.h"

VideoPlayer::VideoPlayer() {}

VideoPlayer::~VideoPlayer() {
    stop();
}

bool VideoPlayer::load(const std::string& path) {
    stop();

    std::string uri = "file://" + path;
    // We want RGBA to load easily into OpenGL textures
    std::string pipeline_str = "uridecodebin uri=" + uri + " ! videoconvert ! video/x-raw,format=RGBA ! appsink name=sink sync=true";

    GError* err = nullptr;
    m_pipeline = gst_parse_launch(pipeline_str.c_str(), &err);
    if (err != nullptr) {
        g_error_free(err);
        return false;
    }

    m_appsink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
    if (!m_appsink) {
        stop();
        return false;
    }

    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(m_appsink), &callbacks, this, nullptr);

    return true;
}

void VideoPlayer::play() {
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    }
}

void VideoPlayer::stop() {
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        
        if (m_appsink) {
            gst_object_unref(m_appsink);
            m_appsink = nullptr;
        }
    }
}

void VideoPlayer::setFrameCallback(std::function<void(const uint8_t*, int, int)> cb) {
    m_callback = std::move(cb);
}

GstFlowReturn VideoPlayer::onNewSample(GstAppSink* sink, gpointer data) {
    auto* self = static_cast<VideoPlayer*>(data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    GstCaps* caps = gst_sample_get_caps(sample);
    if (!caps) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstStructure* s = gst_caps_get_structure(caps, 0);
    int w = 0, h = 0;
    if (!gst_structure_get_int(s, "width", &w) || !gst_structure_get_int(s, "height", &h)) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        if (self->m_callback) {
            self->m_callback(map.data, w, h);
        }
        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
