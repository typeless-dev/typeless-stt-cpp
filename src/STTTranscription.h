#ifndef STT_RT_DLL_STTTRANSCRIPTION_H
#define STT_RT_DLL_STTTRANSCRIPTION_H
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/common/asio_ssl.hpp>
#include <websocketpp/base64/base64.hpp>
#include <iostream>
#include <portaudio.h>
#include <thread>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstdlib>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

class STTTranscription
{
public:
    int startRecording(void (*updateText)(char *transcription), const std::string &url, const std::string &language, bool manualPunctuation, const std::string &domain);
    int stopRecording();
    // void wsThread(client &c);
    void clearConsole();

private:
    bool shouldRecord = true;
    std::thread ws_thread;
};

#endif