/*
 * Oreka -- A media capture and retrieval platform
 *
 * LiveStreamFilter Plugin
 * Author Kinshuk Bairagi
 *
 */
#pragma warning(disable: 4786) // disables truncated symbols in browse-info warning

#define _WINSOCKAPI_ // prevents the inclusion of winsock.h

#include "LiveStreamFilter.h"
#include "LiveStreamConfig.h"

#define G711_PACKET_INTERVAL 20

static log4cxx::LoggerPtr s_log = log4cxx::Logger::getLogger("plugin.livestream");

LiveStreamFilter::LiveStreamFilter() : bufferQueueA(LIVESTREAMCONFIG.m_queueFlushThresholdMillis/G711_PACKET_INTERVAL), bufferQueueB(LIVESTREAMCONFIG.m_queueFlushThresholdMillis/G711_PACKET_INTERVAL) {
    // LOG4CXX_DEBUG(s_log, "LiveStream New Instance Created");
    //For 1 second, there will be 1000ms / 20ms = 50 frames
    // auto bufferSize = LIVESTREAMCONFIG.m_queueFlushThresholdMillis/20;
    shouldStreamAllCalls = LIVESTREAMCONFIG.m_shouldStreamAllCalls;
}

LiveStreamFilter::~LiveStreamFilter() {
    if (silentChannelBuffer != NULL){
        free(silentChannelBuffer);
    }

    // LOG4CXX_DEBUG(s_log, "LiveStream Instance Destroying");
}

FilterRef LiveStreamFilter::Instanciate() {
    FilterRef Filter(new LiveStreamFilter());
    return Filter;
}

void LiveStreamFilter::AudioChunkIn(AudioChunkRef & inputAudioChunk) {
    m_outputAudioChunk = inputAudioChunk;

    if (inputAudioChunk.get() == NULL) {
        return;
    }

    if (inputAudioChunk->GetNumSamples() == 0) {
        return;
    }

    AudioChunkDetails inputDetails = * inputAudioChunk->GetDetails();
    char * newBuffer = (char * ) inputAudioChunk->m_pBuffer;

    if (isFirstPacket) {
        currentBufferChannel = inputDetails.m_channel;
        isFirstPacket = false;
        CStdString logMsg;
        logMsg.Format("LiveStream:: HeadChannel: %d", currentBufferChannel);
        LOG4CXX_DEBUG(s_log, logMsg);
    }

    if (silentChannelBuffer == NULL){
        silentChannelBuffer = (char *)malloc(inputDetails.m_numBytes);
        if (!silentChannelBuffer) {
            CStdString logMsg;
            logMsg.Format("LiveStreamFilter::AudioChunkIn [%s] SilentChannelBuffer Memory allocation failed.", m_orkRefId);
            LOG4CXX_ERROR(s_log, logMsg);
            return;
        }
        std::fill_n(silentChannelBuffer, inputDetails.m_numBytes, 255);
    }

    if (status) {
        char *bufferedChunk;
        char *newChunk;
        char *leftChunk;
        char *rightChunk;
        RingBuffer<AudioChunkRef> *currentBufferQueue;
        RingBuffer<AudioChunkRef> *standbyBufferQueue;
        if (useBufferA) {
            currentBufferQueue = &bufferQueueA;
            standbyBufferQueue = &bufferQueueB;
        } else {
            currentBufferQueue = &bufferQueueB;
            standbyBufferQueue = &bufferQueueA;
        }
        boost::optional<AudioChunkRef> queuedChunk;
        if (inputDetails.m_channel == currentBufferChannel) {
            if (queuedChunk = currentBufferQueue->put(inputAudioChunk)){
                newChunk = silentChannelBuffer;
                bufferedChunk = (char *)(*queuedChunk)->m_pBuffer;
            } else {
                return;
            }
        } else {
            if (queuedChunk = currentBufferQueue->get()){
                newChunk = newBuffer;
                bufferedChunk = (char *)(*queuedChunk)->m_pBuffer;
            } else {
                useBufferA = !useBufferA;
                currentBufferChannel = inputDetails.m_channel;

                if (queuedChunk = standbyBufferQueue->put(inputAudioChunk)) {
                    LOG4CXX_ERROR(s_log, "Standby buffer has chunk");
                    status = false;
                }
                return;
            }
        }
        if (currentBufferChannel == 1) {
            leftChunk = bufferedChunk;
            rightChunk = newChunk;
        } else {
            leftChunk = newChunk;
            rightChunk = bufferedChunk;
        }
        PushToRTMP(inputDetails, leftChunk, rightChunk);
    }

}

void LiveStreamFilter::PushToRTMP(AudioChunkDetails& channelDetails, char * firstChannelBuffer, char * secondChannelBuffer) {

    if (rtmp == NULL) {
        return;
    }

    CStdString logMsg;
    int size = channelDetails.m_numBytes * 2;
    //logMsg.Format("LiveStreamFilter AudioChunkIn Size: %d, Encoding: %s , RTP payload type: %s",size ,toString(outputDetails.m_encoding) , RtpPayloadTypeEnumToString(outputDetails.m_rtpPayloadType));
    //LOG4CXX_INFO(s_log, logMsg);

    // @param sound_format Format of SoundData. The following values are defined:
    // 0 = Linear PCM, platform endian
    // 1 = ADPCM
    // 2 = MP3
    // 3 = Linear PCM, little endian
    // 4 = Nellymoser 16 kHz mono
    // 5 = Nellymoser 8 kHz mono
    // 6 = Nellymoser
    // 7 = G.711 A-law logarithmic PCM
    // 8 = G.711 mu-law logarithmic PCM
    // 9 = reserved
    // 10 = AAC
    // 11 = Speex
    // 14 = MP3 8 kHz
    // 15 = Device-specific sound
    // Formats 7, 8, 14, and 15 are reserved.
    // AAC is supported in Flash Player 9,0,115,0 and higher.
    // Speex is supported in Flash Player 10 and higher.

    char sound_format = 9;
    if (channelDetails.m_rtpPayloadType == pt_PCMU)
        sound_format = 8;
    else if (channelDetails.m_rtpPayloadType == pt_PCMA)
        sound_format = 7;

    // @param sound_rate Sampling rate. The following values are defined:
    // 0 = 5.5 kHz
    // 1 = 11 kHz
    // 2 = 22 kHz
    // 3 = 44 kHz
    char sound_rate = 3;

    // @param sound_size Size of each audio sample. This parameter only pertains to
    // uncompressed formats. Compressed formats always decode
    // to 16 bits internally.
    // 0 = 8-bit samples
    // 1 = 16-bit samples
    char sound_size = 1;

    // @param sound_type Mono or stereo sound
    // 0 = Mono sound
    // 1 = Stereo sound
    //char sound_type = outputDetails.m_channel == 0 ? 0 : 1;
    char sound_type = 1;

    timestamp += G711_PACKET_INTERVAL;
    char *outputBuffer = (char *)malloc(size);
    if (!outputBuffer) {
        logMsg.Format("LiveStreamFilter::Send [%s] Memory allocation failed.", m_orkRefId);
        LOG4CXX_ERROR(s_log, logMsg);
        return;
    }

    for (int i = 0; i < channelDetails.m_numBytes; ++i)
    {
        outputBuffer[i * 2] = firstChannelBuffer[i];
        outputBuffer[i * 2 + 1] = secondChannelBuffer[i];
    }

    if (srs_audio_write_raw_frame(rtmp, sound_format, sound_rate, sound_size, sound_type, outputBuffer, size, timestamp) != 0)
    {
        //outputBuffer has been freed internally
        //rtmp server write failure, needs reopen, stream can't continue for this call.
        status = false;
        logMsg.Format("LiveStreamFilter::Send [%s] send audio raw data failed.", m_orkRefId);
        LOG4CXX_ERROR(s_log, logMsg);
        return;
    }

    logMsg.Format("LiveStreamFilter::Send [%s] packet: type=%s, time=%d, size=%d, codec=%d, rate=%d, sample=%d, channel=%d nativecallId=%s",
                  m_orkRefId, srs_human_flv_tag_type2string(SRS_RTMP_TYPE_AUDIO), timestamp, size, sound_format, sound_rate, sound_size, sound_type, m_callId);
    LOG4CXX_TRACE(s_log, logMsg);

    free(outputBuffer);
}

void LiveStreamFilter::AudioChunkOut(AudioChunkRef & chunk) {
    chunk = m_outputAudioChunk;
}

AudioEncodingEnum LiveStreamFilter::GetInputAudioEncoding() {
    return UnknownAudio;
}

AudioEncodingEnum LiveStreamFilter::GetOutputAudioEncoding() {
    return UnknownAudio;
}

CStdString LiveStreamFilter::GetName() {
    return "LiveStreamFilter";
}

bool LiveStreamFilter::SupportsInputRtpPayloadType(int rtpPayloadType) {
    //so that BatchProcessing doesn't pick this filter.
    return rtpPayloadType == pt_Unknown;
}

void LiveStreamFilter::CaptureEventIn(CaptureEventRef & event) {
    //Start RTP Stream Open
    CStdString logMsg;
    auto key = event->EventTypeToString(event->m_type);

    if (event->m_type == CaptureEvent::EventTypeEnum::EtStart) {
        m_orkRefId = event->m_value;
    }

    logMsg.Format("LiveStream:: CaptureEventIn[%s] Key: %s, Value: %s", m_orkRefId, key, event->m_value);
    LOG4CXX_DEBUG(s_log, logMsg);

    if (event->m_type == CaptureEvent::EventTypeEnum::EtCallId) {
        m_callId = event->m_value;
    }

    if (event->m_type == CaptureEvent::EventTypeEnum::EtOrkUid) {
        m_orkUid = event->m_value;
    }
    if (event->m_type == CaptureEvent::EventTypeEnum::EtDirection) {
        m_direction = event->m_value;
    }
    if (event->m_type == CaptureEvent::EventTypeEnum::EtLocalParty) {
        m_localParty = event->m_value;
    }
    if (event->m_type == CaptureEvent::EventTypeEnum::EtRemoteParty) {
        m_remoteParty = event->m_value;
    }
    if (event->m_type == CaptureEvent::EventTypeEnum::EtLocalIp) {
        m_localIp = event->m_value;
    }
    if (event->m_type == CaptureEvent::EventTypeEnum::EtRemoteIp) {
        m_remoteIp = event->m_value;
    }

    if (
        (event->m_type == CaptureEvent::EventTypeEnum::EtCallId && shouldStreamAllCalls) ||
        (event->m_type == CaptureEvent::EventTypeEnum::EtKeyValue && event->m_key == "LiveStream" && event->m_value == "start" && !shouldStreamAllCalls)
    ) {

        if (m_callId.empty()) {
            logMsg.Format("LiveStream:: Start[%s] Failed for Empty Call ID", m_orkRefId);
            LOG4CXX_ERROR(s_log, logMsg);
            return;
        }
        if (m_orkUid.empty()) {
            logMsg.Format("LiveStream:: Start[%s] Failed for Empty Ork UID", m_orkUid);
            LOG4CXX_ERROR(s_log, logMsg);
            return;
        }
        const uuid streamingUuid = random_generator()();
        auto liveStreamingId = boost::lexical_cast<std::string>(streamingUuid);
        std::string url = "rtmp://" ;
        url += LIVESTREAMCONFIG.m_rtmpServerEndpoint;
        url += "/";
        url += liveStreamingId;
        url += "?orkuid=";
        url += m_orkUid;
        url += "&nativecallid=";
        url += m_callId;
        url += "&localparty=";
        url += m_localParty;
        url += "&remoteparty=";
        url += m_remoteParty;
        url += "&remoteip=";
        url += m_remoteIp;
        url += "&localip=";
        url += m_localIp;
        url += "&direction=";
        url += m_direction;
        url += "&service=";
        url += LIVESTREAMCONFIG.m_serviceName;


        logMsg.Format("LiveStream:: Start[%s] Streaming URL %s", m_orkRefId, url.c_str());
        LOG4CXX_INFO(s_log, logMsg);
        //open rstp stream
        rtmp = srs_rtmp_create(url.c_str());
        if (srs_rtmp_handshake(rtmp) != 0) {
            logMsg.Format("LiveStream:: Start[%s] RTMP simple handshake failed.", m_orkRefId);
            LOG4CXX_ERROR(s_log, logMsg);
            return;
        }
        logMsg.Format("LiveStream:: Start[%s] RTMP simple handshake success", m_orkRefId);
        LOG4CXX_DEBUG(s_log, logMsg);

        if (srs_rtmp_connect_app(rtmp) != 0) {
            logMsg.Format("LiveStream:: Start[%s] RTMP connect vhost/app failed", m_orkRefId);
            LOG4CXX_ERROR(s_log, logMsg);
            return;
        }

        logMsg.Format("LiveStream:: Start[%s] RTMP connect vhost/app success", m_orkRefId);
        LOG4CXX_DEBUG(s_log, logMsg);

        if (srs_rtmp_publish_stream(rtmp) != 0) {
            logMsg.Format("LiveStream:: Start[%s] RTMP publish stream failed", m_orkRefId);
            LOG4CXX_ERROR(s_log, logMsg);
            return;
        }

        logMsg.Format("LiveStream:: Start[%s] RTMP publish stream success", m_orkRefId);
        LOG4CXX_DEBUG(s_log, logMsg);

        status = true;
        LiveStreamSessionsSingleton::instance()->AddToStreamCallList(m_callId);
    }

    if (event->m_type == CaptureEvent::EventTypeEnum::EtStop) {
        //close rtmp stream
        status = false;
        LiveStreamSessionsSingleton::instance()->RemoveFromStreamCallList(m_callId);
        if (rtmp != NULL) {
            logMsg.Format("LiveStream:: Stop[%s] RTMP stream destroying", m_orkRefId);
            LOG4CXX_INFO(s_log, logMsg);
            srs_rtmp_destroy(rtmp);
        }
    }

    if (event->m_type == CaptureEvent::EventTypeEnum::EtKeyValue && event->m_key == "LiveStream" && event->m_value == "stop") {
        LiveStreamSessionsSingleton::instance()->RemoveFromStreamCallList(m_callId);
        status = false;
    }
}

void LiveStreamFilter::CaptureEventOut(CaptureEventRef & event) {
    //LOG4CXX_INFO(s_log, "LiveStream CaptureEventOut " + toString(event.get()));
}

void LiveStreamFilter::SetSessionInfo(CStdString & trackingId) {
    LOG4CXX_INFO(s_log, "LiveStream SetSessionInfo " + trackingId);
}


// =================================================================

extern "C"
{
    DLL_EXPORT void __CDECL__ OrkInitialize()
    {
        LOG4CXX_INFO(s_log, "LiveStream  Filter starting");

        //LiveStreamConfig
        ConfigManager::Instance()->AddConfigureFunction(LiveStreamConfig::Configure);

        FilterRef filter(new LiveStreamFilter());
        FilterRegistry::instance()->RegisterFilter(filter);

        LOG4CXX_INFO(s_log, "LiveStream  filter initialized");

        LiveStreamServer *liveStreamServer = new LiveStreamServer(LIVESTREAMCONFIG.m_serverPort);
        liveStreamServer->Run();
    }

}
