#pragma once

#include "AudioDevice.hpp"

class OutputAudioDevice : public AudioDevice {
public:
    void populate() override;
    sizedptr get_buffer();
    void submit_buffer(AudioDriver *driver);
};