/*
 * Copyright (C) 2016, 2017 Metrological Group B.V.
 * Copyright (C) 2016, 2017 Igalia S.L
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "AppendPipeline.h"

#if ENABLE(VIDEO) && USE(GSTREAMER) && ENABLE(MEDIA_SOURCE)

#include "AudioTrackPrivateGStreamer.h"
#include "GStreamerCommon.h"
#include "GStreamerEMEUtilities.h"
#include "GStreamerMediaDescription.h"
#include "GStreamerMediaSample.h"
#include "InbandTextTrackPrivateGStreamer.h"
#include "MediaDescription.h"
#include "SourceBufferPrivateGStreamer.h"
#include "VideoTrackPrivateGStreamer.h"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <gst/video/video.h>
#include <wtf/Condition.h>
#include <wtf/SetForScope.h>
#include <wtf/glib/GLibUtilities.h>
#include <wtf/glib/RunLoopSourcePriority.h>

GST_DEBUG_CATEGORY_EXTERN(webkit_mse_debug);
#define GST_CAT_DEFAULT webkit_mse_debug

namespace WebCore {

static const char* dumpAppendState(AppendPipeline::AppendState appendState)
{
    switch (appendState) {
    case AppendPipeline::AppendState::Invalid:
        return "Invalid";
    case AppendPipeline::AppendState::NotStarted:
        return "NotStarted";
    case AppendPipeline::AppendState::Ongoing:
        return "Ongoing";
    case AppendPipeline::AppendState::DataStarve:
        return "DataStarve";
    case AppendPipeline::AppendState::Sampling:
        return "Sampling";
    case AppendPipeline::AppendState::LastSample:
        return "LastSample";
    case AppendPipeline::AppendState::Aborting:
        return "Aborting";
    default:
        return "(unknown)";
    }
}

static void appendPipelineAppsrcNeedData(GstAppSrc*, guint, AppendPipeline*);
static void appendPipelineDemuxerPadAdded(GstElement*, GstPad*, AppendPipeline*);
static void appendPipelineDemuxerPadRemoved(GstElement*, GstPad*, AppendPipeline*);
static void appendPipelineAppsinkCapsChanged(GObject*, GParamSpec*, AppendPipeline*);
static GstPadProbeReturn appendPipelineAppsrcDataLeaving(GstPad*, GstPadProbeInfo*, AppendPipeline*);
#if !LOG_DISABLED
static GstPadProbeReturn appendPipelinePadProbeDebugInformation(GstPad*, GstPadProbeInfo*, struct PadProbeInformation*);
#endif

#if ENABLE(ENCRYPTED_MEDIA)
static GstPadProbeReturn appendPipelineAppsinkPadProtectionProbe(GstPad*, GstPadProbeInfo*, struct PadProbeInformation*);
#endif

static GstPadProbeReturn appendPipelineDemuxerBlackHolePadProbe(GstPad*, GstPadProbeInfo*, gpointer);
static GstFlowReturn appendPipelineAppsinkNewSample(GstElement*, AppendPipeline*);
static void appendPipelineAppsinkEOS(GstElement*, AppendPipeline*);
static void appendPipelineDemuxerNoMorePads(GstElement*, AppendPipeline* appendPipeline);

static void appendPipelineNeedContextMessageCallback(GstBus*, GstMessage* message, AppendPipeline* appendPipeline)
{
    GST_TRACE("received callback");
    appendPipeline->handleNeedContextSyncMessage(message);
}

static void appendPipelineApplicationMessageCallback(GstBus*, GstMessage* message, AppendPipeline* appendPipeline)
{
    appendPipeline->handleApplicationMessage(message);
}

static void appendPipelineStateChangeMessageCallback(GstBus*, GstMessage* message, AppendPipeline* appendPipeline)
{
    appendPipeline->handleStateChangeMessage(message);
}

// Auxiliary class to compute the sample duration when GStreamer provides an invalid one.
class BufferMetadataCompleter {
public:
    BufferMetadataCompleter()
        : m_sampleDuration(MediaTime::invalidTime())
        , m_lastPts(MediaTime::invalidTime())
        , m_lastDts(MediaTime::invalidTime())
        , m_ptsOffset(MediaTime::invalidTime())
    {
    }

    void completeMissingMetadata(GstBuffer* buffer)
    {
        if (buffer) {
	    GST_TRACE("Before: PTS=%" GST_TIME_FORMAT "  DTS=%" GST_TIME_FORMAT "  DUR=%" GST_TIME_FORMAT "\n", 
			    GST_TIME_ARGS(GST_BUFFER_PTS(buffer)),
			    GST_TIME_ARGS(GST_BUFFER_DTS(buffer)),
			    GST_TIME_ARGS(GST_BUFFER_DURATION(buffer)));

            if (!GST_BUFFER_DURATION_IS_VALID(buffer)) {
                if (m_sampleDuration.isValid()) {
                    // Some containers like webm and audio/x-opus don't supply a duration. Let's use the one supplied by the caps.
                    GST_BUFFER_DURATION(buffer) = toGstClockTime(m_sampleDuration);
                } else if (m_lastPts.isValid()) {
                    m_sampleDuration = MediaTime(GST_BUFFER_PTS(buffer), GST_SECOND) - m_lastPts;
                    GST_BUFFER_DURATION(buffer) = toGstClockTime(m_sampleDuration);
                } else if (GST_BUFFER_PTS_IS_VALID(buffer)) {
                    // Some containers like webm don't supply a duration so it has to be assumed. Let's assume it for 24 fps. It's safer
                    // than e.g. for 60 fps becuase with the duration assumed for 60 fps the algorithm of samples processing may cause them to be dropped/erased.
                    // On the other hand this does not do any harm for the next frames even if the duration is in fact shorter due to higher fps.
                    GST_BUFFER_DURATION(buffer) = toGstClockTime(MediaTime(1, 24));
                }
            }

            if (!GST_BUFFER_DTS_IS_VALID(buffer) && GST_BUFFER_PTS_IS_VALID(buffer)) {
                // If we had a last DTS and the PTS hasn't varied too much (continuous samples), DTS++.
                if (m_lastDts.isValid() && abs(m_lastPts + MediaTime(GST_BUFFER_DURATION(buffer), GST_SECOND) - MediaTime(GST_BUFFER_PTS(buffer), GST_SECOND)) < MediaTime(1, 2))
                    GST_BUFFER_DTS(buffer) = toGstClockTime(m_lastDts + MediaTime(GST_BUFFER_DURATION(buffer), GST_SECOND));
                else
                    GST_BUFFER_DTS(buffer) = toGstClockTime(MediaTime(GST_BUFFER_PTS(buffer), GST_SECOND) + MediaTime(GST_BUFFER_DURATION(buffer), GST_SECOND));
		GST_TRACE("DTS FILLED: %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(GST_BUFFER_DTS(buffer)));
            }

            // If the first sample (DTS=0) doesn't start with PTS=0, compute a negative offset.
            if (!GST_BUFFER_DTS(buffer) && GST_BUFFER_PTS(buffer) && !m_ptsOffset.isValid()) {
                m_ptsOffset = MediaTime(GST_BUFFER_DTS(buffer), GST_SECOND) - MediaTime(GST_BUFFER_PTS(buffer), GST_SECOND);
                GST_TRACE("Setting an offset of %s\n", m_ptsOffset.toString().utf8().data());
            }

            // Apply the offset to zero-align the first sample and also correct the next ones.
            if (m_ptsOffset.isValid())
                GST_BUFFER_PTS(buffer) += toGstClockTime(m_ptsOffset);

            m_lastPts = MediaTime(GST_BUFFER_PTS(buffer), GST_SECOND);
            m_lastDts = MediaTime(GST_BUFFER_DTS(buffer), GST_SECOND);

	    GST_TRACE("After: PTS=%" GST_TIME_FORMAT "  DTS=%" GST_TIME_FORMAT "  DUR=%" GST_TIME_FORMAT "\n", 
			    GST_TIME_ARGS(GST_BUFFER_PTS(buffer)),
			    GST_TIME_ARGS(GST_BUFFER_DTS(buffer)),
			    GST_TIME_ARGS(GST_BUFFER_DURATION(buffer)));
        }
    }

private:
    MediaTime m_sampleDuration;
    MediaTime m_lastPts;
    MediaTime m_lastDts;
    MediaTime m_ptsOffset;
};

AppendPipeline::AppendPipeline(Ref<MediaSourceClientGStreamerMSE> mediaSourceClient, Ref<SourceBufferPrivateGStreamer> sourceBufferPrivate, MediaPlayerPrivateGStreamerMSE& playerPrivate)
    : m_mediaSourceClient(mediaSourceClient.get())
    , m_sourceBufferPrivate(sourceBufferPrivate.get())
    , m_playerPrivate(&playerPrivate)
    , m_id(0)
    , m_busAlreadyNotifiedOfAvailablesamples(false)
    , m_appsrcAtLeastABufferLeft(false)
    , m_appsrcNeedDataReceived(false)
    , m_firstTrackDetected(false)
    , m_appsrcDataLeavingProbeId(0)
    , m_appendState(AppendState::NotStarted)
    , m_abortPending(false)
    , m_streamType(Unknown)
{
    ASSERT(WTF::isMainThread());

    GST_TRACE("Creating AppendPipeline (%p)", this);

    // FIXME: give a name to the pipeline, maybe related with the track it's managing.
    // The track name is still unknown at this time, though.
    m_pipeline = gst_pipeline_new(nullptr);

    m_bus = adoptGRef(gst_pipeline_get_bus(GST_PIPELINE(m_pipeline.get())));
    gst_bus_add_signal_watch_full(m_bus.get(), RunLoopSourcePriority::RunLoopDispatcher);
    gst_bus_enable_sync_message_emission(m_bus.get());

    g_signal_connect(m_bus.get(), "sync-message::need-context", G_CALLBACK(appendPipelineNeedContextMessageCallback), this);
    g_signal_connect(m_bus.get(), "message::application", G_CALLBACK(appendPipelineApplicationMessageCallback), this);
    g_signal_connect(m_bus.get(), "message::state-changed", G_CALLBACK(appendPipelineStateChangeMessageCallback), this);

    // We assign the created instances here instead of adoptRef() because gst_bin_add_many()
    // below will already take the initial reference and we need an additional one for us.
    m_appsrc = gst_element_factory_make("appsrc", nullptr);

    const String& type = m_sourceBufferPrivate->type().containerType();
    if (type.endsWith("mp4"))
        m_demux = gst_element_factory_make("qtdemux", nullptr);
    else if (type.endsWith("webm"))
        m_demux = gst_element_factory_make("matroskademux", nullptr);
    else
        ASSERT_NOT_REACHED();

    m_appsink = gst_element_factory_make("appsink", nullptr);

    gst_app_sink_set_emit_signals(GST_APP_SINK(m_appsink.get()), TRUE);
    gst_base_sink_set_sync(GST_BASE_SINK(m_appsink.get()), FALSE);

    GRefPtr<GstPad> appsinkPad = adoptGRef(gst_element_get_static_pad(m_appsink.get(), "sink"));
    g_signal_connect(appsinkPad.get(), "notify::caps", G_CALLBACK(appendPipelineAppsinkCapsChanged), this);

    setAppsrcDataLeavingProbe();

#if !LOG_DISABLED
    GRefPtr<GstPad> demuxerPad = adoptGRef(gst_element_get_static_pad(m_demux.get(), "sink"));
    m_demuxerDataEnteringPadProbeInformation.appendPipeline = this;
    m_demuxerDataEnteringPadProbeInformation.description = "demuxer data entering";
    m_demuxerDataEnteringPadProbeInformation.probeId = gst_pad_add_probe(demuxerPad.get(), GST_PAD_PROBE_TYPE_BUFFER, reinterpret_cast<GstPadProbeCallback>(appendPipelinePadProbeDebugInformation), &m_demuxerDataEnteringPadProbeInformation, nullptr);
    m_appsinkDataEnteringPadProbeInformation.appendPipeline = this;
    m_appsinkDataEnteringPadProbeInformation.description = "appsink data entering";
    m_appsinkDataEnteringPadProbeInformation.probeId = gst_pad_add_probe(appsinkPad.get(), GST_PAD_PROBE_TYPE_BUFFER, reinterpret_cast<GstPadProbeCallback>(appendPipelinePadProbeDebugInformation), &m_appsinkDataEnteringPadProbeInformation, nullptr);
#endif

#if ENABLE(ENCRYPTED_MEDIA)
    m_appsinkPadProtectionProbeInformation.appendPipeline = this;
    m_appsinkPadProtectionProbeInformation.description = "appsink protection probe";
    m_appsinkPadProtectionProbeInformation.probeId = gst_pad_add_probe(appsinkPad.get(), static_cast<GstPadProbeType>(static_cast<int>(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) | static_cast<int>(GST_PAD_PROBE_TYPE_BUFFER)), reinterpret_cast<GstPadProbeCallback>(appendPipelineAppsinkPadProtectionProbe), &m_appsinkPadProtectionProbeInformation, nullptr);

    m_cachedProtectionEvents = G_VALUE_INIT;
    g_value_init(&m_cachedProtectionEvents, GST_TYPE_LIST);
#endif

    // These signals won't be connected outside of the lifetime of "this".
    g_signal_connect(m_appsrc.get(), "need-data", G_CALLBACK(appendPipelineAppsrcNeedData), this);
    g_signal_connect(m_demux.get(), "pad-added", G_CALLBACK(appendPipelineDemuxerPadAdded), this);
    g_signal_connect(m_demux.get(), "pad-removed", G_CALLBACK(appendPipelineDemuxerPadRemoved), this);
    g_signal_connect(m_demux.get(), "no-more-pads", G_CALLBACK(appendPipelineDemuxerNoMorePads), this);
    g_signal_connect(m_appsink.get(), "new-sample", G_CALLBACK(appendPipelineAppsinkNewSample), this);
    g_signal_connect(m_appsink.get(), "eos", G_CALLBACK(appendPipelineAppsinkEOS), this);

    // Add_many will take ownership of a reference. That's why we used an assignment before.
    gst_bin_add_many(GST_BIN(m_pipeline.get()), m_appsrc.get(), m_demux.get(), nullptr);
    gst_element_link(m_appsrc.get(), m_demux.get());

    gst_element_set_state(m_pipeline.get(), GST_STATE_READY);
};

AppendPipeline::~AppendPipeline()
{
    ASSERT(WTF::isMainThread());

    GST_TRACE("Destroying AppendPipeline (%p)", this);

    setAppendState(AppendState::Invalid);

    {
        LockHolder locker(m_padAddRemoveLock);
        m_playerPrivate = nullptr;
        m_padAddRemoveCondition.notifyOne();
    }

    if (m_appsink) {
        GstSample *sample;
        while ((sample = gst_app_sink_try_pull_sample(GST_APP_SINK(m_appsink.get()), 0)))
            gst_sample_unref(sample);
    }

    // FIXME: Maybe notify appendComplete here?

    if (m_pipeline) {
        ASSERT(m_bus);
        g_signal_handlers_disconnect_by_func(m_bus.get(), reinterpret_cast<gpointer>(appendPipelineNeedContextMessageCallback), this);
        gst_bus_disable_sync_message_emission(m_bus.get());
        gst_bus_remove_signal_watch(m_bus.get());
        gst_element_set_state(m_pipeline.get(), GST_STATE_NULL);
        m_pipeline = nullptr;
    }

    if (m_appsrc) {
        removeAppsrcDataLeavingProbe();
        g_signal_handlers_disconnect_by_data(m_appsrc.get(), this);
        m_appsrc = nullptr;
    }

    if (m_demux) {
#if !LOG_DISABLED
        GRefPtr<GstPad> demuxerPad = adoptGRef(gst_element_get_static_pad(m_demux.get(), "sink"));
        gst_pad_remove_probe(demuxerPad.get(), m_demuxerDataEnteringPadProbeInformation.probeId);
#endif

        g_signal_handlers_disconnect_by_data(m_demux.get(), this);
        m_demux = nullptr;
    }

    if (m_appsink) {
        GRefPtr<GstPad> appsinkPad = adoptGRef(gst_element_get_static_pad(m_appsink.get(), "sink"));
        g_signal_handlers_disconnect_by_data(appsinkPad.get(), this);
        g_signal_handlers_disconnect_by_data(m_appsink.get(), this);

#if !LOG_DISABLED
        gst_pad_remove_probe(appsinkPad.get(), m_appsinkDataEnteringPadProbeInformation.probeId);
#endif

#if ENABLE(ENCRYPTED_MEDIA)
        gst_pad_remove_probe(appsinkPad.get(), m_appsinkPadProtectionProbeInformation.probeId);
#endif
        m_appsink = nullptr;
    }

    m_appsinkCaps = nullptr;
    m_demuxerSrcPadCaps = nullptr;

#if ENABLE(ENCRYPTED_MEDIA)
    g_value_unset(&m_cachedProtectionEvents);
#endif
};

void AppendPipeline::clearPlayerPrivate()
{
    ASSERT(WTF::isMainThread());
    GST_DEBUG("cleaning private player");

    // Make sure that AppendPipeline won't process more data from now on and
    // instruct handleNewSample to abort itself from now on as well.
    setAppendState(AppendState::Invalid);

    {
        LockHolder locker(m_padAddRemoveLock);
        m_playerPrivate = nullptr;
        m_padAddRemoveCondition.notifyOne();
    }

    if (m_appsink) {
        GstSample *sample;
        while ((sample = gst_app_sink_try_pull_sample(GST_APP_SINK(m_appsink.get()), 0)))
            gst_sample_unref(sample);
    }

    // And now that no handleNewSample operations will remain stalled waiting
    // for the main thread, stop the pipeline.
    if (m_pipeline)
        gst_element_set_state(m_pipeline.get(), GST_STATE_NULL);
}

void AppendPipeline::handleNeedContextSyncMessage(GstMessage* message)
{
    const gchar* contextType = nullptr;
    gst_message_parse_context_type(message, &contextType);
    GST_TRACE("context type: %s", contextType);

    LockHolder locker(m_appendStateTransitionLock);
    if (m_appendState == AppendState::Invalid)
        return;

    // MediaPlayerPrivateGStreamerBase will take care of setting up encryption if needed.
    if (m_playerPrivate)
        m_playerPrivate->handleSyncMessage(message);
}

void AppendPipeline::handleApplicationMessage(GstMessage* message)
{
    ASSERT(WTF::isMainThread());

    RefPtr<AppendPipeline> protectedThis(this);

    const GstStructure* structure = gst_message_get_structure(message);

    if (gst_structure_has_name(structure, "appsrc-need-data")) {
        handleAppsrcNeedDataReceived();
        return;
    }

    if (gst_structure_has_name(structure, "appsrc-buffer-left")) {
        handleAppsrcAtLeastABufferLeft();
        return;
    }

    if (gst_structure_has_name(structure, "demuxer-connect-to-appsink")) {
        GRefPtr<GstPad> demuxerSrcPad;
        gst_structure_get(structure, "demuxer-src-pad", G_TYPE_OBJECT, &demuxerSrcPad.outPtr(), nullptr);
        ASSERT(demuxerSrcPad);
        connectDemuxerSrcPadToAppsink(demuxerSrcPad.get());
        return;
    }

#if ENABLE(ENCRYPTED_MEDIA)
    if (gst_structure_has_name(structure, "demuxer-done-sending-protection-events")) {
        demuxerIsDoneSendingProtectionEvents(structure);
        return;
    }
#endif

    if (gst_structure_has_name(structure, "transition-main-thread")) {
        GST_TRACE("Received transition-main-thread in main thread");
        AppendState nextState;
        gst_structure_get(structure, "transition", G_TYPE_INT, &nextState, nullptr);
        transitionTo(nextState, false);
        return;
    }

    if (gst_structure_has_name(structure, "appsink-caps-changed")) {
        appsinkCapsChanged();
        return;
    }

    if (gst_structure_has_name(structure, "appsink-new-sample")) {
        m_busAlreadyNotifiedOfAvailablesamples = false;
        consumeAppSinkAvailableSamples();
        return;
    }

    if (gst_structure_has_name(structure, "appsink-eos")) {
        appsinkEOS();
        return;
    }

    if (gst_structure_has_name(structure, "demuxer-no-more-pads")) {
        demuxerNoMorePads();
        return;
    }

    ASSERT_NOT_REACHED();
}

void AppendPipeline::demuxerNoMorePads()
{
    GST_TRACE("calling didReceiveInitializationSegment");
    didReceiveInitializationSegment();
    GST_TRACE("set pipeline to playing");
    gst_element_set_state(m_pipeline.get(), GST_STATE_PLAYING);
}

void AppendPipeline::handleStateChangeMessage(GstMessage* message)
{
    ASSERT(WTF::isMainThread());

    if (GST_MESSAGE_SRC(message) == reinterpret_cast<GstObject*>(m_pipeline.get())) {
        GstState currentState, newState;
        gst_message_parse_state_changed(message, &currentState, &newState, nullptr);
        CString sourceBufferType = String(m_sourceBufferPrivate->type().raw())
            .replace("/", "_").replace(" ", "_")
            .replace("\"", "").replace("\'", "").utf8();
        CString dotFileName = String::format("webkit-append-%s-%s_%s",
            sourceBufferType.data(),
            gst_element_state_get_name(currentState),
            gst_element_state_get_name(newState)).utf8();
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline.get()), GST_DEBUG_GRAPH_SHOW_ALL, dotFileName.data());
    }
}

void AppendPipeline::handleAppsrcNeedDataReceived()
{
    if (!m_appsrcAtLeastABufferLeft) {
        GST_TRACE("discarding until at least a buffer leaves appsrc");
        return;
    }

    ASSERT(m_appendState == AppendState::Ongoing || m_appendState == AppendState::Sampling);
    ASSERT(!m_appsrcNeedDataReceived);

    GST_TRACE("received need-data from appsrc");

    m_appsrcNeedDataReceived = true;
    checkEndOfAppend();
}

void AppendPipeline::handleAppsrcAtLeastABufferLeft()
{
    m_appsrcAtLeastABufferLeft = true;
    GST_TRACE("received buffer-left from appsrc");
#if LOG_DISABLED
    removeAppsrcDataLeavingProbe();
#endif
}

gint AppendPipeline::id()
{
    ASSERT(WTF::isMainThread());

    if (m_id)
        return m_id;

    static gint s_totalAudio = 0;
    static gint s_totalVideo = 0;
    static gint s_totalText = 0;

    switch (m_streamType) {
    case Audio:
        m_id = ++s_totalAudio;
        break;
    case Video:
        m_id = ++s_totalVideo;
        break;
    case Text:
        m_id = ++s_totalText;
        break;
    case Unknown:
    case Invalid:
        GST_ERROR("Trying to get id for a pipeline of Unknown/Invalid type");
        ASSERT_NOT_REACHED();
        break;
    }

    GST_DEBUG("streamType=%d, id=%d", static_cast<int>(m_streamType), m_id);

    return m_id;
}

void AppendPipeline::setAppendState(AppendState newAppendState)
{
    ASSERT(WTF::isMainThread());
    // Valid transitions:
    // NotStarted-->Ongoing-->DataStarve-->NotStarted
    //           |         |            `->Aborting-->NotStarted
    //           |         `->Sampling-···->Sampling-->LastSample-->NotStarted
    //           |                                               `->Aborting-->NotStarted
    //           `->Aborting-->NotStarted
    AppendState oldAppendState = m_appendState;
    AppendState nextAppendState = AppendState::Invalid;

    if (oldAppendState != newAppendState)
        GST_TRACE("%s --> %s", dumpAppendState(oldAppendState), dumpAppendState(newAppendState));

    bool ok = false;
    bool mustCheckEndOfAppend = false;

    switch (oldAppendState) {
    case AppendState::NotStarted:
        switch (newAppendState) {
        case AppendState::Ongoing:
            ok = true;
            gst_element_set_state(m_pipeline.get(), GST_STATE_PLAYING);
            break;
        case AppendState::NotStarted:
            ok = true;
            if (m_pendingBuffer) {
                GST_TRACE("pushing pending buffer %p", m_pendingBuffer.get());
                gst_app_src_push_buffer(GST_APP_SRC(appsrc()), m_pendingBuffer.leakRef());
                nextAppendState = AppendState::Ongoing;
            }
            break;
        case AppendState::Aborting:
            ok = true;
            nextAppendState = AppendState::NotStarted;
            break;
        case AppendState::Invalid:
            ok = true;
            break;
        default:
            break;
        }
        break;
    case AppendState::Ongoing:
        switch (newAppendState) {
        case AppendState::Sampling:
        case AppendState::Invalid:
            ok = true;
            break;
        case AppendState::DataStarve:
            ok = true;
            GST_DEBUG("received all pending samples");
            m_sourceBufferPrivate->didReceiveAllPendingSamples();
            if (m_abortPending)
                nextAppendState = AppendState::Aborting;
            else
                nextAppendState = AppendState::NotStarted;
            break;
        default:
            break;
        }
        break;
    case AppendState::DataStarve:
        switch (newAppendState) {
        case AppendState::NotStarted:
        case AppendState::Invalid:
            ok = true;
            break;
        case AppendState::Aborting:
            ok = true;
            nextAppendState = AppendState::NotStarted;
            break;
        default:
            break;
        }
        break;
    case AppendState::Sampling:
        switch (newAppendState) {
        case AppendState::Sampling:
        case AppendState::Invalid:
            ok = true;
            break;
        case AppendState::LastSample:
            ok = true;
            GST_DEBUG("received all pending samples");
            m_sourceBufferPrivate->didReceiveAllPendingSamples();
            if (m_abortPending)
                nextAppendState = AppendState::Aborting;
            else
                nextAppendState = AppendState::NotStarted;
            break;
        default:
            break;
        }
        break;
    case AppendState::LastSample:
        switch (newAppendState) {
        case AppendState::NotStarted:
        case AppendState::Invalid:
            ok = true;
            break;
        case AppendState::Aborting:
            ok = true;
            nextAppendState = AppendState::NotStarted;
            break;
        default:
            break;
        }
        break;
    case AppendState::Aborting:
        switch (newAppendState) {
        case AppendState::NotStarted:
            ok = true;
            resetPipeline();
            m_abortPending = false;
            nextAppendState = AppendState::NotStarted;
            break;
        case AppendState::Invalid:
            ok = true;
            break;
        default:
            break;
        }
        break;
    case AppendState::Invalid:
        ok = true;
        break;
    }

    if (ok)
        m_appendState = newAppendState;
    else
        GST_ERROR("Invalid append state transition %s --> %s", dumpAppendState(oldAppendState), dumpAppendState(newAppendState));

    ASSERT(ok);

    if (mustCheckEndOfAppend)
        checkEndOfAppend();

    if (nextAppendState != AppendState::Invalid)
        setAppendState(nextAppendState);
}

void AppendPipeline::parseDemuxerSrcPadCaps(GstCaps* demuxerSrcPadCaps)
{
    ASSERT(WTF::isMainThread());

    m_demuxerSrcPadCaps = adoptGRef(demuxerSrcPadCaps);
    m_streamType = WebCore::MediaSourceStreamTypeGStreamer::Unknown;

    const char* originalMediaType = capsMediaType(m_demuxerSrcPadCaps.get());
    if (!MediaPlayerPrivateGStreamerMSE::supportsCodec(originalMediaType)) {
            m_presentationSize = WebCore::FloatSize();
            m_streamType = WebCore::MediaSourceStreamTypeGStreamer::Invalid;
    } else if (doCapsHaveType(m_demuxerSrcPadCaps.get(), GST_VIDEO_CAPS_TYPE_PREFIX)) {
        std::optional<FloatSize> size = getVideoResolutionFromCaps(m_demuxerSrcPadCaps.get());
        if (size.has_value())
            m_presentationSize = size.value();
        else
            m_presentationSize = WebCore::FloatSize();

        m_streamType = WebCore::MediaSourceStreamTypeGStreamer::Video;
    } else {
        m_presentationSize = WebCore::FloatSize();
        if (doCapsHaveType(m_demuxerSrcPadCaps.get(), GST_AUDIO_CAPS_TYPE_PREFIX))
            m_streamType = WebCore::MediaSourceStreamTypeGStreamer::Audio;
        else if (doCapsHaveType(m_demuxerSrcPadCaps.get(), GST_TEXT_CAPS_TYPE_PREFIX))
            m_streamType = WebCore::MediaSourceStreamTypeGStreamer::Text;
    }
}

void AppendPipeline::appsinkCapsChanged()
{
    ASSERT(WTF::isMainThread());

    if (!m_appsink)
        return;

    GRefPtr<GstPad> pad = adoptGRef(gst_element_get_static_pad(m_appsink.get(), "sink"));
    GRefPtr<GstCaps> caps = adoptGRef(gst_pad_get_current_caps(pad.get()));

    if (!caps)
        return;

    if (m_appsinkCaps != caps) {
        m_appsinkCaps = WTFMove(caps);
        if (m_playerPrivate)
            m_playerPrivate->trackDetected(this, m_track, !m_firstTrackDetected);
        gst_element_set_state(m_pipeline.get(), GST_STATE_PLAYING);
    }
}

void AppendPipeline::checkEndOfAppend()
{
    ASSERT(WTF::isMainThread());

    if (!m_appsrcNeedDataReceived || (m_appendState != AppendState::Ongoing && m_appendState != AppendState::Sampling))
        return;

    GST_TRACE("end of append data mark was received");

    switch (m_appendState) {
    case AppendState::Ongoing:
        GST_TRACE("DataStarve");
        m_appsrcNeedDataReceived = false;
        setAppendState(AppendState::DataStarve);
        break;
    case AppendState::Sampling:
        GST_TRACE("LastSample");
        m_appsrcNeedDataReceived = false;
        setAppendState(AppendState::LastSample);
        break;
    default:
        ASSERT_NOT_REACHED();
        break;
    }
}

void AppendPipeline::appsinkNewSample(GstSample* sample)
{
    ASSERT(WTF::isMainThread());

    // Ignore samples if we're not expecting them. Refuse processing if we're in Invalid state.
    if (m_appendState != AppendState::Ongoing && m_appendState != AppendState::Sampling) {
        GST_WARNING("Unexpected sample, appendState=%s", dumpAppendState(m_appendState));
        // FIXME: Return ERROR and find a more robust way to detect that all the
        // data has been processed, so we don't need to resort to these hacks.
        // All in all, return OK, even if it's not the proper thing to do. We don't want to break the demuxer.
        gst_sample_unref(sample);
        return;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        GST_WARNING("Received sample without buffer from appsink. Why?");
        gst_sample_unref(sample);
        return;
    }

    // This increases sample refcount, as GStreamerMediaSample manages its own reference.
    // We must still unref our own current ref before exiting this method.
    RefPtr<GStreamerMediaSample> mediaSample = WebCore::GStreamerMediaSample::create(sample, m_presentationSize, trackId());

    GST_TRACE("append: trackId=%s PTS=%s DTS=%s DUR=%s presentationSize=%.0fx%.0f %s%s",
        mediaSample->trackID().string().utf8().data(),
        mediaSample->presentationTime().toString().utf8().data(),
        mediaSample->decodeTime().toString().utf8().data(),
        mediaSample->duration().toString().utf8().data(),
        mediaSample->presentationSize().width(), mediaSample->presentationSize().height(),
        mediaSample->flags() == MediaSample::SampleFlags::IsSync ? "[SYNC]" : "",
        mediaSample->flags() == MediaSample::SampleFlags::IsNonDisplaying ? "[NON-DISPLAYING]" : "");

    // If we're beyond the duration, ignore this sample and the remaining ones.
    MediaTime duration = m_mediaSourceClient->duration();
    if (duration.isValid() && !duration.isIndefinite() && mediaSample->presentationTime() > duration) {
        GST_DEBUG("Detected sample (%s) beyond the duration (%s), declaring LastSample", mediaSample->presentationTime().toString().utf8().data(), duration.toString().utf8().data());
        setAppendState(AppendState::LastSample);
        gst_sample_unref(sample);
        return;
    }

    // Add a gap sample if a gap is detected before the first sample.
    if (mediaSample->decodeTime() == MediaTime::zeroTime()
        && mediaSample->presentationTime() > MediaTime::zeroTime()
        && mediaSample->presentationTime() <= MediaTime(1, 10)) {
        GST_DEBUG("Adding gap offset");
        mediaSample->applyPtsOffset(MediaTime::zeroTime());
    }

    m_sourceBufferPrivate->didReceiveSample(*mediaSample);
    setAppendState(AppendState::Sampling);
    gst_sample_unref(sample);
}

void AppendPipeline::appsinkEOS()
{
    ASSERT(WTF::isMainThread());

    switch (m_appendState) {
    case AppendState::Aborting:
        // Ignored. Operation completion will be managed by the Aborting->NotStarted transition.
        return;
    case AppendState::Ongoing:
        // Finish Ongoing and Sampling states.
        setAppendState(AppendState::DataStarve);
        break;
    case AppendState::Sampling:
        setAppendState(AppendState::LastSample);
        break;
    default:
        GST_DEBUG("Unexpected EOS");
        break;
    }
}

void AppendPipeline::didReceiveInitializationSegment()
{
    ASSERT(WTF::isMainThread());

    WebCore::SourceBufferPrivateClient::InitializationSegment initializationSegment;

    GST_DEBUG("Notifying SourceBuffer for track %s", (m_track) ? m_track->id().string().utf8().data() : nullptr);
    initializationSegment.duration = m_mediaSourceClient->duration();

    switch (m_streamType) {
    case Audio: {
        WebCore::SourceBufferPrivateClient::InitializationSegment::AudioTrackInformation info;
        info.track = static_cast<AudioTrackPrivateGStreamer*>(m_track.get());
        info.description = WebCore::GStreamerMediaDescription::create(m_demuxerSrcPadCaps.get());
        initializationSegment.audioTracks.append(info);
        break;
    }
    case Video: {
        WebCore::SourceBufferPrivateClient::InitializationSegment::VideoTrackInformation info;
        info.track = static_cast<VideoTrackPrivateGStreamer*>(m_track.get());
        info.description = WebCore::GStreamerMediaDescription::create(m_demuxerSrcPadCaps.get());
        initializationSegment.videoTracks.append(info);
        break;
    }
    default:
        GST_ERROR("Unsupported stream type or codec");
        break;
    }

    m_sourceBufferPrivate->didReceiveInitializationSegment(initializationSegment);
}

AtomicString AppendPipeline::trackId()
{
    ASSERT(WTF::isMainThread());

    if (!m_track)
        return AtomicString();

    return m_track->id();
}

void AppendPipeline::consumeAppSinkAvailableSamples()
{
    ASSERT(WTF::isMainThread());

    GstSample *sample;
    int batchedSampleCount = 0;
    while ((sample = gst_app_sink_try_pull_sample(GST_APP_SINK(m_appsink.get()), 0))) {
        appsinkNewSample(sample);
        batchedSampleCount++;
    }

    GST_DEBUG("batchedSampleCount = %d", batchedSampleCount);

    if (batchedSampleCount > 0) {
        checkEndOfAppend();
    }
}

void AppendPipeline::resetPipeline()
{
    ASSERT(WTF::isMainThread());
    GST_DEBUG("resetting pipeline");
    m_appsrcAtLeastABufferLeft = false;
    setAppsrcDataLeavingProbe();

    gst_element_set_state(m_pipeline.get(), GST_STATE_READY);
    gst_element_get_state(m_pipeline.get(), nullptr, nullptr, 0);

#if (!(LOG_DISABLED || defined(GST_DISABLE_GST_DEBUG)))
    {
        static unsigned i = 0;
        // This is here for debugging purposes. It does not make sense to have it as class member.
        WTF::String  dotFileName = String::format("reset-pipeline-%d", ++i);
        gst_debug_bin_to_dot_file(GST_BIN(m_pipeline.get()), GST_DEBUG_GRAPH_SHOW_ALL, dotFileName.utf8().data());
    }
#endif

}

void AppendPipeline::setAppsrcDataLeavingProbe()
{
    if (m_appsrcDataLeavingProbeId)
        return;

    GST_TRACE("setting appsrc data leaving probe");

    GRefPtr<GstPad> appsrcPad = adoptGRef(gst_element_get_static_pad(m_appsrc.get(), "src"));
    m_appsrcDataLeavingProbeId = gst_pad_add_probe(appsrcPad.get(), GST_PAD_PROBE_TYPE_BUFFER, reinterpret_cast<GstPadProbeCallback>(appendPipelineAppsrcDataLeaving), this, nullptr);
}

void AppendPipeline::removeAppsrcDataLeavingProbe()
{
    if (!m_appsrcDataLeavingProbeId)
        return;

    GST_TRACE("removing appsrc data leaving probe");

    GRefPtr<GstPad> appsrcPad = adoptGRef(gst_element_get_static_pad(m_appsrc.get(), "src"));
    gst_pad_remove_probe(appsrcPad.get(), m_appsrcDataLeavingProbeId);
    m_appsrcDataLeavingProbeId = 0;
}

void AppendPipeline::abort()
{
    ASSERT(WTF::isMainThread());
    GST_DEBUG("aborting");

    m_pendingBuffer = nullptr;

    // Abort already ongoing.
    if (m_abortPending)
        return;

    m_abortPending = true;
    if (m_appendState == AppendState::NotStarted)
        setAppendState(AppendState::Aborting);
    // Else, the automatic state transitions will take care when the ongoing append finishes.
}

GstFlowReturn AppendPipeline::pushNewBuffer(GstBuffer* buffer)
{
    GstFlowReturn result;

    if (m_abortPending) {
        m_pendingBuffer = adoptGRef(buffer);
        result = GST_FLOW_OK;
    } else {
        setAppendState(AppendPipeline::AppendState::Ongoing);
        GST_TRACE("pushing new buffer %p", buffer);
        result = gst_app_src_push_buffer(GST_APP_SRC(appsrc()), buffer);
    }

    return result;
}

void AppendPipeline::reportAppsrcAtLeastABufferLeft()
{
    GST_TRACE("buffer left appsrc, reposting to bus");
    GstStructure* structure = gst_structure_new_empty("appsrc-buffer-left");
    GstMessage* message = gst_message_new_application(GST_OBJECT(m_appsrc.get()), structure);
    gst_bus_post(m_bus.get(), message);
}

void AppendPipeline::reportAppsrcNeedDataReceived()
{
    GST_TRACE("received need-data signal at appsrc, reposting to bus");
    GstStructure* structure = gst_structure_new_empty("appsrc-need-data");
    GstMessage* message = gst_message_new_application(GST_OBJECT(m_appsrc.get()), structure);
    gst_bus_post(m_bus.get(), message);
}

GstFlowReturn AppendPipeline::handleNewAppsinkSample(GstElement* appsink)
{
    ASSERT(!WTF::isMainThread());

    if (!m_playerPrivate || m_appendState == AppendState::Invalid) {
        GstSample *sample;
        while ((sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 0)))
            gst_sample_unref(sample);
        GST_WARNING("AppendPipeline has been disabled, ignoring this sample");
        return GST_FLOW_ERROR;
    }

    if (!m_busAlreadyNotifiedOfAvailablesamples) {
        m_busAlreadyNotifiedOfAvailablesamples = true;
        GstStructure* structure = gst_structure_new_empty("appsink-new-sample");
        GstMessage* message = gst_message_new_application(GST_OBJECT(appsink), structure);
        gst_bus_post(m_bus.get(), message);
        GST_TRACE("appsink-new-sample message posted to bus");
    }

    return GST_FLOW_OK;
}

static GRefPtr<GstElement>
createOptionalParserForFormat(GstPad* demuxerSrcPad)
{
    GRefPtr<GstCaps> padCaps = adoptGRef(gst_pad_get_current_caps(demuxerSrcPad));
    GstStructure* structure = gst_caps_get_structure(padCaps.get(), 0);
    const char* mediaType = gst_structure_get_name(structure);

    GUniquePtr<char> demuxerPadName(gst_pad_get_name(demuxerSrcPad));
    GUniquePtr<char> parserName(g_strdup_printf("%s_parser", demuxerPadName.get()));

    if (!g_strcmp0(mediaType, "audio/x-opus")) {
        GstElement* opusparse = gst_element_factory_make("opusparse", parserName.get());
        RELEASE_ASSERT(opusparse);
        return GRefPtr<GstElement>(opusparse);
    }
    if (!g_strcmp0(mediaType, "audio/x-vorbis")) {
        GstElement* vorbisparse = gst_element_factory_make("vorbisparse", parserName.get());
        RELEASE_ASSERT(vorbisparse);
        return GRefPtr<GstElement>(vorbisparse);
    }

    return nullptr;
}

void AppendPipeline::connectDemuxerSrcPadToAppsinkFromAnyThread(GstPad* demuxerSrcPad)
{
    if (!m_appsink)
        return;

    GST_DEBUG("connecting to appsink");

    if (m_demux->numsrcpads > 1) {
        GST_WARNING("Only one stream per SourceBuffer is allowed! Ignoring stream %d by adding a black hole probe.", m_demux->numsrcpads);
        gulong probeId = gst_pad_add_probe(demuxerSrcPad, GST_PAD_PROBE_TYPE_BUFFER, reinterpret_cast<GstPadProbeCallback>(appendPipelineDemuxerBlackHolePadProbe), nullptr, nullptr);
        g_object_set_data(G_OBJECT(demuxerSrcPad), "blackHoleProbeId", GULONG_TO_POINTER(probeId));
        return;
    }

    GRefPtr<GstPad> appsinkSinkPad = adoptGRef(gst_element_get_static_pad(m_appsink.get(), "sink"));

    // Only one stream per demuxer is supported.
    ASSERT(!gst_pad_is_linked(appsinkSinkPad.get()));

    gint64 timeLength = 0;
    if (gst_element_query_duration(m_demux.get(), GST_FORMAT_TIME, &timeLength)
        && static_cast<guint64>(timeLength) != GST_CLOCK_TIME_NONE)
        m_initialDuration = MediaTime(GST_TIME_AS_USECONDS(timeLength), G_USEC_PER_SEC);
    else
        m_initialDuration = MediaTime::positiveInfiniteTime();

    if (WTF::isMainThread())
        connectDemuxerSrcPadToAppsink(demuxerSrcPad);
    else {
        // Call connectDemuxerSrcPadToAppsink() in the main thread and wait.
        LockHolder locker(m_padAddRemoveLock);
        if (!m_playerPrivate)
            return;

        GstStructure* structure = gst_structure_new("demuxer-connect-to-appsink", "demuxer-src-pad", G_TYPE_OBJECT, demuxerSrcPad, nullptr);
        GstMessage* message = gst_message_new_application(GST_OBJECT(m_demux.get()), structure);
        gst_bus_post(m_bus.get(), message);
        GST_TRACE("demuxer-connect-to-appsink message posted to bus, waiting...");

        m_padAddRemoveCondition.wait(m_padAddRemoveLock);

        if (!m_playerPrivate)
            return;
    }

    // Must be done in the thread we were called from (usually streaming thread).
    bool isData = (m_streamType == WebCore::MediaSourceStreamTypeGStreamer::Audio)
        || (m_streamType == WebCore::MediaSourceStreamTypeGStreamer::Video)
        || (m_streamType == WebCore::MediaSourceStreamTypeGStreamer::Text);

    GST_TRACE("isData = %s", boolForPrinting(isData));

    if (isData) {
        // FIXME: Only add appsink one time. This method can be called several times.
        GRefPtr<GstObject> parent = adoptGRef(gst_element_get_parent(m_appsink.get()));
        if (!parent)
            gst_bin_add(GST_BIN(m_pipeline.get()), m_appsink.get());

        // Current head of the pipeline being built.
        GRefPtr<GstPad> currentSrcPad = demuxerSrcPad;

        // Some audio files unhelpfully omit the duration of frames in the container. We need to parse
        // the contained audio streams in order to know the duration of the frames.
        // This is known to be an issue with YouTube WebM files containing Opus audio as of YTTV2018.
        m_parser = createOptionalParserForFormat(currentSrcPad.get());
        if (m_parser) {
            GST_INFO("created parser %s", GST_ELEMENT_NAME(m_parser.get()));
            gst_bin_add(GST_BIN(m_pipeline.get()), m_parser.get());
            gst_element_sync_state_with_parent(m_parser.get());

            GRefPtr<GstPad> parserSinkPad = adoptGRef(gst_element_get_static_pad(m_parser.get(), "sink"));
            GRefPtr<GstPad> parserSrcPad = adoptGRef(gst_element_get_static_pad(m_parser.get(), "src"));

            gst_pad_link(currentSrcPad.get(), parserSinkPad.get());
            currentSrcPad = parserSrcPad;
        } else
            GST_INFO("no parser");

        gst_pad_add_probe(currentSrcPad.get(), GST_PAD_PROBE_TYPE_BUFFER,
            [] (GstPad*, GstPadProbeInfo* info, void* userData) -> GstPadProbeReturn {
                ASSERT(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER);
                BufferMetadataCompleter* completer = static_cast<BufferMetadataCompleter*>(userData);
                GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
                completer->completeMissingMetadata(buffer);
                return GST_PAD_PROBE_OK;
            },
            new BufferMetadataCompleter(),
            [] (gpointer userData) {
                BufferMetadataCompleter* completer = static_cast<BufferMetadataCompleter*>(userData);
                delete completer;
            });

        gst_pad_link(currentSrcPad.get(), appsinkSinkPad.get());

        gst_element_sync_state_with_parent(m_appsink.get());

        gst_element_set_state(m_pipeline.get(), GST_STATE_PAUSED);
        gst_element_sync_state_with_parent(m_appsink.get());

        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline.get()), GST_DEBUG_GRAPH_SHOW_ALL, "webkit-after-link");
    }
}

void AppendPipeline::transitionTo(AppendState nextState, bool isAlreadyLocked)
{
    ASSERT(WTF::isMainThread());

    LockHolder locker(isAlreadyLocked ? nullptr : &m_appendStateTransitionLock);

    if (m_appendState == AppendState::Invalid || m_appendState == nextState || !m_playerPrivate) {
        m_appendStateTransitionCondition.notifyOne();
        return;
    }

    setAppendState(nextState);

    m_appendStateTransitionCondition.notifyOne();
}

void AppendPipeline::connectDemuxerSrcPadToAppsink(GstPad* demuxerSrcPad)
{
    ASSERT(WTF::isMainThread());
    GST_DEBUG("Connecting to appsink");

    LockHolder locker(m_padAddRemoveLock);
    GRefPtr<GstPad> sinkSinkPad = adoptGRef(gst_element_get_static_pad(m_appsink.get(), "sink"));

    // Only one stream per demuxer is supported.
    ASSERT(!gst_pad_is_linked(sinkSinkPad.get()));

    GRefPtr<GstCaps> caps = adoptGRef(gst_pad_get_current_caps(GST_PAD(demuxerSrcPad)));

    if (!caps || m_appendState == AppendState::Invalid || !m_playerPrivate) {
        m_padAddRemoveCondition.notifyOne();
        return;
    }

#ifndef GST_DISABLE_GST_DEBUG
    {
        GUniquePtr<gchar> strcaps(gst_caps_to_string(caps.get()));
        GST_DEBUG("%s", strcaps.get());
    }
#endif

    if (m_mediaSourceClient->duration().isInvalid() && m_initialDuration > MediaTime::zeroTime())
        m_mediaSourceClient->durationChanged(m_initialDuration);

    parseDemuxerSrcPadCaps(gst_caps_ref(caps.get()));

    switch (m_streamType) {
    case WebCore::MediaSourceStreamTypeGStreamer::Audio:
        if (m_playerPrivate)
            m_track = WebCore::AudioTrackPrivateGStreamer::create(m_playerPrivate->pipeline(), id(), sinkSinkPad.get());
        break;
    case WebCore::MediaSourceStreamTypeGStreamer::Video:
        if (m_playerPrivate)
            m_track = WebCore::VideoTrackPrivateGStreamer::create(m_playerPrivate->pipeline(), id(), sinkSinkPad.get());
        break;
    case WebCore::MediaSourceStreamTypeGStreamer::Text:
        m_track = WebCore::InbandTextTrackPrivateGStreamer::create(id(), sinkSinkPad.get());
        break;
    case WebCore::MediaSourceStreamTypeGStreamer::Invalid:
        {
            GUniquePtr<gchar> strcaps(gst_caps_to_string(caps.get()));
            GST_DEBUG("Unsupported track codec: %s", strcaps.get());
        }
        // This is going to cause an error which will detach the SourceBuffer and tear down this
        // AppendPipeline, so we need the padAddRemove lock released before continuing.
        m_track = nullptr;
        m_padAddRemoveCondition.notifyOne();
        locker.unlockEarly();
        didReceiveInitializationSegment();
        return;
    default:
        // No useful data, but notify anyway to complete the append operation.
        GST_DEBUG("Received all pending samples (no data)");
        m_sourceBufferPrivate->didReceiveAllPendingSamples();
        break;
    }

    m_appsinkCaps = WTFMove(caps);
    if (m_playerPrivate) {
        m_playerPrivate->trackDetected(this, m_track, !m_firstTrackDetected);
        m_firstTrackDetected = true;
    }

    m_padAddRemoveCondition.notifyOne();
}

void AppendPipeline::disconnectDemuxerSrcPadFromAppsinkFromAnyThread(GstPad*)
{
    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline.get()), GST_DEBUG_GRAPH_SHOW_ALL, "pad-removed-before");

    GST_DEBUG("Disconnecting appsink");

    if (m_parser) {
        gst_element_set_state(m_parser.get(), GST_STATE_NULL);
        gst_bin_remove(GST_BIN(m_pipeline.get()), m_parser.get());
        m_parser = nullptr;
    }

    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline.get()), GST_DEBUG_GRAPH_SHOW_ALL, "pad-removed-after");
}

void AppendPipeline::appendPipelineDemuxerNoMorePadsFromAnyThread()
{
    GST_TRACE("appendPipelineDemuxerNoMorePadsFromAnyThread");
    GstStructure* structure = gst_structure_new_empty("demuxer-no-more-pads");
    GstMessage* message = gst_message_new_application(GST_OBJECT(m_appsrc.get()), structure);
    gst_bus_post(m_bus.get(), message);
    GST_TRACE("appendPipelineDemuxerNoMorePadsFromAnyThread - posted to bus");
}

static void appendPipelineAppsinkCapsChanged(GObject* appsinkPad, GParamSpec*, AppendPipeline* appendPipeline)
{
    GstStructure* structure = gst_structure_new_empty("appsink-caps-changed");
    GstMessage* message = gst_message_new_application(GST_OBJECT(appsinkPad), structure);
    gst_bus_post(appendPipeline->bus(), message);
    GST_TRACE("appsink-caps-changed message posted to bus");
}

static GstPadProbeReturn appendPipelineAppsrcDataLeaving(GstPad*, GstPadProbeInfo* info, AppendPipeline* appendPipeline)
{
    ASSERT(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER);

    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    gsize bufferSize = gst_buffer_get_size(buffer);

    GST_TRACE("buffer of size %" G_GSIZE_FORMAT " going thru", bufferSize);

    appendPipeline->reportAppsrcAtLeastABufferLeft();

    return GST_PAD_PROBE_OK;
}

#if !LOG_DISABLED
static GstPadProbeReturn appendPipelinePadProbeDebugInformation(GstPad*, GstPadProbeInfo* info, struct PadProbeInformation* padProbeInformation)
{
    ASSERT(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER);
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GST_TRACE("%s: buffer of size %" G_GSIZE_FORMAT " going thru", padProbeInformation->description, gst_buffer_get_size(buffer));
    return GST_PAD_PROBE_OK;
}
#endif

#if ENABLE(ENCRYPTED_MEDIA)
void AppendPipeline::cacheProtectionEvent(GstEvent* event)
{
    if (!m_isProcessingProtectionEvents) {
        GST_TRACE("first event, resetting list");
        m_isProcessingProtectionEvents = true;
        g_value_reset(&m_cachedProtectionEvents);
    }

    GST_DEBUG("caching protection event %u on append pipeline appsink pad", GST_EVENT_SEQNUM(event));
    GValue* eventValue = g_new0(GValue, 1);
    g_value_init(eventValue, GST_TYPE_EVENT);
    g_value_take_boxed(eventValue, event);
    gst_value_list_append_and_take_value(&m_cachedProtectionEvents, eventValue);
}

void AppendPipeline::handleProtectedBufferProbeInformation(GstPadProbeInfo* info)
{
    ASSERT(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER);

    bool wasProcessingProtectionEvents = m_isProcessingProtectionEvents;
    m_isProcessingProtectionEvents = false;

    unsigned listSize = gst_value_list_get_size(&m_cachedProtectionEvents);
    if (!listSize)
        return;

    if (wasProcessingProtectionEvents) {
        GST_TRACE("sending %u protection events to the main thread", listSize);
        GstStructure* structure = gst_structure_new_empty("demuxer-done-sending-protection-events");
        gst_structure_set_value(structure, "stream-encryption-events", &m_cachedProtectionEvents);
        GstMessage* message = gst_message_new_application(GST_OBJECT(m_demux.get()), structure);
        gst_bus_post(m_bus.get(), message);
    }

    GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
    GstProtectionMeta* protectionMeta = reinterpret_cast<GstProtectionMeta*>(gst_buffer_get_protection_meta(buffer));
    if (!protectionMeta)
        return;

    GST_DEBUG("adding %u protection events to buffer %p", listSize, buffer);
    gst_structure_set_value(protectionMeta->info, "stream-encryption-events", &m_cachedProtectionEvents);
}

static GstPadProbeReturn appendPipelineAppsinkPadProtectionProbe(GstPad*, GstPadProbeInfo* info, struct PadProbeInformation *padProbeInformation)
{
    WebCore::AppendPipeline* appendPipeline = padProbeInformation->appendPipeline;
    ASSERT(appendPipeline);

    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        GstEvent* event = gst_pad_probe_info_get_event(info);

        switch (GST_EVENT_TYPE(event)) {
        case GST_EVENT_PROTECTION:
            appendPipeline->cacheProtectionEvent(event);
            return GST_PAD_PROBE_HANDLED;
        default:
            break;
        }
    } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)
        appendPipeline->handleProtectedBufferProbeInformation(info);
    else {
        ASSERT_NOT_REACHED();
    }

    return GST_PAD_PROBE_OK;
}

void AppendPipeline::demuxerIsDoneSendingProtectionEvents(const GstStructure* structure)
{
    const GValue* streamEncryptionEventsList = gst_structure_get_value(structure, "stream-encryption-events");
    ASSERT(streamEncryptionEventsList && GST_VALUE_HOLDS_LIST(streamEncryptionEventsList));
    unsigned streamEncryptionEventsListSize = gst_value_list_get_size(streamEncryptionEventsList);
    GST_DEBUG("forwarding %u protection events to the player", streamEncryptionEventsListSize);
    if (!streamEncryptionEventsListSize)
        return;
    Vector<GstEvent*> protectionEvents;
    protectionEvents.reserveInitialCapacity(streamEncryptionEventsListSize);
    for (unsigned i = 0; i < streamEncryptionEventsListSize; ++i)
        protectionEvents.uncheckedAppend(static_cast<GstEvent*>(g_value_get_boxed(gst_value_list_get_value(streamEncryptionEventsList, i))));
    m_playerPrivate->handleProtectionEvents(protectionEvents);
}
#endif

static GstPadProbeReturn appendPipelineDemuxerBlackHolePadProbe(GstPad*, GstPadProbeInfo* info, gpointer)
{
    ASSERT(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER);
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GST_TRACE("buffer of size %" G_GSIZE_FORMAT " ignored", gst_buffer_get_size(buffer));
    return GST_PAD_PROBE_DROP;
}

static void appendPipelineAppsrcNeedData(GstAppSrc*, guint, AppendPipeline* appendPipeline)
{
    appendPipeline->reportAppsrcNeedDataReceived();
}

static void appendPipelineDemuxerPadAdded(GstElement*, GstPad* demuxerSrcPad, AppendPipeline* appendPipeline)
{
    appendPipeline->connectDemuxerSrcPadToAppsinkFromAnyThread(demuxerSrcPad);
}

static void appendPipelineDemuxerPadRemoved(GstElement*, GstPad* demuxerSrcPad, AppendPipeline* appendPipeline)
{
    appendPipeline->disconnectDemuxerSrcPadFromAppsinkFromAnyThread(demuxerSrcPad);
}

static void appendPipelineDemuxerNoMorePads(GstElement*, AppendPipeline* appendPipeline)
{
    appendPipeline->appendPipelineDemuxerNoMorePadsFromAnyThread();
}

static GstFlowReturn appendPipelineAppsinkNewSample(GstElement* appsink, AppendPipeline* appendPipeline)
{
    return appendPipeline->handleNewAppsinkSample(appsink);
}

static void appendPipelineAppsinkEOS(GstElement*, AppendPipeline* appendPipeline)
{
    if (WTF::isMainThread())
        appendPipeline->appsinkEOS();
    else {
        GstStructure* structure = gst_structure_new_empty("appsink-eos");
        GstMessage* message = gst_message_new_application(GST_OBJECT(appendPipeline->appsink()), structure);
        gst_bus_post(appendPipeline->bus(), message);
        GST_TRACE("appsink-eos message posted to bus");
    }

    GST_DEBUG("%s main thread", (WTF::isMainThread()) ? "Is" : "Not");
}



} // namespace WebCore.

#endif // USE(GSTREAMER)
