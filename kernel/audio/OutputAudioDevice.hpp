#pragma once

#include "AudioDevice.hpp"

class OutputAudioDevice : public AudioDevice {
public:
    void populate() override;
    sizedptr request_buffer() override;
    void submit_buffer(AudioDriver *driver) override;
};