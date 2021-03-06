#include "AudioStreamingPart.h"

#include "AudioStreamingPartInternal.h"

#include "rtc_base/logging.h"
#include "rtc_base/third_party/base64/base64.h"

#include <string>
#include <bitset>
#include <set>
#include <map>

namespace tgcalls {

namespace {

uint32_t stringToUInt32(std::string const &string) {
    std::stringstream stringStream(string);
    uint32_t value = 0;
    stringStream >> value;
    return value;
}

template <typename Out>
void splitString(const std::string &s, char delim, Out result) {
    std::istringstream iss(s);
    std::string item;
    while (std::getline(iss, item, delim)) {
        *result++ = item;
    }
}

std::vector<std::string> splitString(const std::string &s, char delim) {
    std::vector<std::string> elems;
    splitString(s, delim, std::back_inserter(elems));
    return elems;
}

static absl::optional<uint32_t> readInt32(std::string const &data, int &offset) {
    if (offset + 4 > data.length()) {
        return absl::nullopt;
    }

    int32_t value = 0;
    memcpy(&value, data.data() + offset, 4);
    offset += 4;

    return value;
}

struct ChannelUpdate {
    int frameIndex = 0;
    int id = 0;
    uint32_t ssrc = 0;
};

static std::vector<ChannelUpdate> parseChannelUpdates(std::string const &data, int &offset) {
    std::vector<ChannelUpdate> result;

    auto channels = readInt32(data, offset);
    if (!channels) {
        return {};
    }

    auto count = readInt32(data, offset);
    if (!count) {
        return {};
    }

    for (int i = 0; i < count.value(); i++) {
        auto frameIndex = readInt32(data, offset);
        if (!frameIndex) {
            return {};
        }

        auto channelId = readInt32(data, offset);
        if (!channelId) {
            return {};
        }

        auto ssrc = readInt32(data, offset);
        if (!ssrc) {
            return {};
        }

        ChannelUpdate update;
        update.frameIndex = frameIndex.value();
        update.id = channelId.value();
        update.ssrc = ssrc.value();

        result.push_back(update);
    }

    return result;
}

class AVIOContextImpl {
public:
    AVIOContextImpl(std::vector<uint8_t> &&fileData) :
    _fileData(std::move(fileData)) {
        _buffer.resize(4 * 1024);
        _context = avio_alloc_context(_buffer.data(), (int)_buffer.size(), 0, this, &AVIOContextImpl::read, NULL, &AVIOContextImpl::seek);
    }

    ~AVIOContextImpl() {
        av_free(_context);
    }

    static int read(void *opaque, unsigned char *buffer, int bufferSize) {
        AVIOContextImpl *instance = static_cast<AVIOContextImpl *>(opaque);

        int bytesToRead = std::min(bufferSize, ((int)instance->_fileData.size()) - instance->_fileReadPosition);
        if (bytesToRead < 0) {
            bytesToRead = 0;
        }

        if (bytesToRead > 0) {
            memcpy(buffer, instance->_fileData.data() + instance->_fileReadPosition, bytesToRead);
            instance->_fileReadPosition += bytesToRead;

            return bytesToRead;
        } else {
            return AVERROR_EOF;
        }
    }

    static int64_t seek(void *opaque, int64_t offset, int whence) {
        AVIOContextImpl *instance = static_cast<AVIOContextImpl *>(opaque);

        if (whence == 0x10000) {
            return (int64_t)instance->_fileData.size();
        } else {
            int64_t seekOffset = std::min(offset, (int64_t)instance->_fileData.size());
            if (seekOffset < 0) {
                seekOffset = 0;
            }
            instance->_fileReadPosition = (int)seekOffset;
            return seekOffset;
        }
    }

    AVIOContext *getContext() {
        return _context;
    }

private:
    std::vector<uint8_t> _fileData;
    int _fileReadPosition = 0;

    std::vector<uint8_t> _buffer;
    AVIOContext *_context = nullptr;
};

}

struct ReadPcmResult {
    int numSamples = 0;
    int numChannels = 0;
};

class AudioStreamingPartState {
    struct ChannelMapping {
        uint32_t ssrc = 0;
        int channelIndex = 0;

        ChannelMapping(uint32_t ssrc_, int channelIndex_) :
            ssrc(ssrc_), channelIndex(channelIndex_) {
        }
    };

public:
    AudioStreamingPartState(std::vector<uint8_t> &&data, std::string const &container, bool isSingleChannel) :
    _isSingleChannel(isSingleChannel),
    _parsedPart(std::move(data), container) {
        if (_parsedPart.getChannelUpdates().size() == 0 && !isSingleChannel) {
            _didReadToEnd = true;
            return;
        }

        _remainingMilliseconds = _parsedPart.getDurationInMilliseconds();

        for (const auto &it : _parsedPart.getChannelUpdates()) {
            _allSsrcs.insert(it.ssrc);
        }
    }

    ~AudioStreamingPartState() {
    }

    std::map<std::string, int32_t> getEndpointMapping() const {
        return _parsedPart.getEndpointMapping();
    }

    int getRemainingMilliseconds() const {
        return _remainingMilliseconds;
    }

    std::vector<AudioStreamingPart::StreamingPartChannel> get10msPerChannel(AudioStreamingPartPersistentDecoder &persistentDecoder) {
        if (_didReadToEnd) {
            return {};
        }

        for (const auto &update : _parsedPart.getChannelUpdates()) {
            if (update.frameIndex == _frameIndex) {
                updateCurrentMapping(update.ssrc, update.id);
            }
        }

        auto readResult = _parsedPart.readPcm(persistentDecoder, _pcm10ms);
        if (readResult.numSamples <= 0) {
            _didReadToEnd = true;
            return {};
        }

        std::vector<AudioStreamingPart::StreamingPartChannel> resultChannels;

        if (_isSingleChannel) {
            for (int i = 0; i < readResult.numChannels; i++) {
                AudioStreamingPart::StreamingPartChannel emptyPart;
                emptyPart.ssrc = i + 1;
                resultChannels.push_back(emptyPart);
            }

            for (int i = 0; i < readResult.numChannels; i++) {
                auto channel = resultChannels.begin() + i;
                int sourceChannelIndex = i;
                for (int j = 0; j < readResult.numSamples; j++) {
                    channel->pcmData.push_back(_pcm10ms[sourceChannelIndex + j * readResult.numChannels]);
                }
                channel->numSamples += readResult.numSamples;
            }
        } else {
            for (const auto ssrc : _allSsrcs) {
                AudioStreamingPart::StreamingPartChannel emptyPart;
                emptyPart.ssrc = ssrc;
                resultChannels.push_back(emptyPart);
            }

            for (auto &channel : resultChannels) {
                auto mappedChannelIndex = getCurrentMappedChannelIndex(channel.ssrc);

                if (mappedChannelIndex) {
                    int sourceChannelIndex = mappedChannelIndex.value();
                    for (int j = 0; j < readResult.numSamples; j++) {
                        channel.pcmData.push_back(_pcm10ms[sourceChannelIndex + j * readResult.numChannels]);
                    }
                    channel.numSamples += readResult.numSamples;
                } else {
                    for (int j = 0; j < readResult.numSamples; j++) {
                        channel.pcmData.push_back(0);
                    }
                    channel.numSamples += readResult.numSamples;
                }
            }
        }

        _remainingMilliseconds -= 10;
        if (_remainingMilliseconds < 0) {
            _remainingMilliseconds = 0;
        }
        _frameIndex++;

        return resultChannels;
    }

private:
    absl::optional<int> getCurrentMappedChannelIndex(uint32_t ssrc) {
        for (const auto &it : _currentChannelMapping) {
            if (it.ssrc == ssrc) {
                return it.channelIndex;
            }
        }
        return absl::nullopt;
    }

    void updateCurrentMapping(uint32_t ssrc, int channelIndex) {
        for (int i = (int)_currentChannelMapping.size() - 1; i >= 0; i--) {
            const auto &entry = _currentChannelMapping[i];
            if (entry.ssrc == ssrc && entry.channelIndex == channelIndex) {
                return;
            } else if (entry.ssrc == ssrc || entry.channelIndex == channelIndex) {
                _currentChannelMapping.erase(_currentChannelMapping.begin() + i);
            }
        }
        _currentChannelMapping.emplace_back(ssrc, channelIndex);
    }

private:
    bool _isSingleChannel = false;
    AudioStreamingPartInternal _parsedPart;
    std::set<uint32_t> _allSsrcs;

    std::vector<int16_t> _pcm10ms;
    std::vector<ChannelMapping> _currentChannelMapping;
    int _frameIndex = 0;
    int _remainingMilliseconds = 0;

    bool _didReadToEnd = false;
};

AudioStreamingPart::AudioStreamingPart(std::vector<uint8_t> &&data, std::string const &container, bool isSingleChannel) {
    if (!data.empty()) {
        _state = new AudioStreamingPartState(std::move(data), container, isSingleChannel);
    }
}

AudioStreamingPart::~AudioStreamingPart() {
    if (_state) {
        delete _state;
    }
}

std::map<std::string, int32_t> AudioStreamingPart::getEndpointMapping() const {
    return _state ? _state->getEndpointMapping() : std::map<std::string, int32_t>();
}

int AudioStreamingPart::getRemainingMilliseconds() const {
    return _state ? _state->getRemainingMilliseconds() : 0;
}

std::vector<AudioStreamingPart::StreamingPartChannel> AudioStreamingPart::get10msPerChannel(AudioStreamingPartPersistentDecoder &persistentDecoder) {
    return _state
        ? _state->get10msPerChannel(persistentDecoder)
        : std::vector<AudioStreamingPart::StreamingPartChannel>();
}

}
