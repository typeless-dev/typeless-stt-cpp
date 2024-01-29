#include "STTTranscription.h"

// Constants
constexpr int SAMPLE_RATE = 16000;
constexpr int CHUNK_SECONDS = 1;
constexpr int NUM_CHANNELS = 1;
constexpr int FRAMES_PER_BUFFER = SAMPLE_RATE * CHUNK_SECONDS;

// Globals
typedef websocketpp::client<websocketpp::config::asio_tls_client> client;
std::queue<std::vector<char>> audioQueue;
std::mutex audioMutex;
std::condition_variable audioCond;
websocketpp::connection_hdl global_hdl;

void writeToBuffer(std::ostringstream &buffer, int data, int size)
{
    for (int i = 0; i < size; ++i)
    {
        buffer.put(static_cast<char>(data & 0xFF));
        data >>= 8;
    }
}

std::vector<char> createWavBuffer(const std::vector<float> &buffer, int sampleRate, int bitDepth, int channels)
{
    std::ostringstream wavBuffer;

    // Header chunk
    wavBuffer << "RIFF";
    wavBuffer << "----"; // Placeholder for file size
    wavBuffer << "WAVE";

    // Format chunk
    wavBuffer << "fmt ";
    writeToBuffer(wavBuffer, 16, 4);                                   // Size
    writeToBuffer(wavBuffer, 1, 2);                                    // Compression code
    writeToBuffer(wavBuffer, channels, 2);                             // Number of channels
    writeToBuffer(wavBuffer, sampleRate, 4);                           // Sample rate
    writeToBuffer(wavBuffer, sampleRate * channels * bitDepth / 8, 4); // Byte rate
    writeToBuffer(wavBuffer, channels * bitDepth / 8, 2);              // Block align
    writeToBuffer(wavBuffer, bitDepth, 2);                             // Bit depth

    // Data chunk
    wavBuffer << "data";
    wavBuffer << "----"; // Placeholder for data chunk size

    std::streampos preAudioPosition = wavBuffer.tellp();

    // Reading from the input buffer and writing to the wavBuffer
    for (size_t i = 0; i < buffer.size(); ++i)
    {
        writeToBuffer(wavBuffer, static_cast<int>(buffer[i] * pow(2, bitDepth - 1)), bitDepth / 8);
    }

    std::streampos postAudioPosition = wavBuffer.tellp();

    // Updating chunk sizes
    wavBuffer.seekp(preAudioPosition - static_cast<std::streampos>(4), std::ios::beg);
    writeToBuffer(wavBuffer, static_cast<int>(postAudioPosition - preAudioPosition), 4);
    wavBuffer.seekp(4, std::ios::beg);
    writeToBuffer(wavBuffer, static_cast<int>(postAudioPosition - static_cast<std::streampos>(8)), 4);

    // Convert the stringstream buffer to a vector of chars
    std::string str = wavBuffer.str();
    return std::vector<char>(str.begin(), str.end());
}

// Audio mic callback to populate queue with chunks
static int audioCallback(const void *inputBuffer, void *outputBuffer,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo *timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void *userData)
{
    std::vector<char> wavData = createWavBuffer(std::vector<float>(static_cast<const float *>(inputBuffer),
                                                                   static_cast<const float *>(inputBuffer) + framesPerBuffer),
                                                SAMPLE_RATE, 32, NUM_CHANNELS);

    // Save wav to queue
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        audioQueue.push(std::move(wavData));
        audioCond.notify_one();
    }

    return paContinue;
}

// Wait for new chunks in queue and send them to websocket
void wsThread(client &c)
{
    while (true)
    {
        std::vector<char> audioChunk;
        {
            std::unique_lock<std::mutex> lock(audioMutex);
            audioCond.wait(lock, []
                           { return !audioQueue.empty(); });
            audioChunk = std::move(audioQueue.front());
            audioQueue.pop();
        }

        try
        {

            // Convert the audio data to a base64 string
            std::string base64_audio = websocketpp::base64_encode(reinterpret_cast<const unsigned char *>(audioChunk.data()), audioChunk.size());
            // Create a JSON object and add the base64 string to it
            nlohmann::json j;
            j["audio"] = base64_audio;
            j["uid"] = "1234567890";

            // Convert the JSON object to a string
            std::string json_str = j.dump();

            // Send the stringified JSON
            c.send(global_hdl, json_str, websocketpp::frame::opcode::text);
        }
        catch (std::exception &e)
        {
        }
    }
}

int STTTranscription::startRecording(void (*updateText)(char *), const std::string &url, const std::string &language, bool manualPunctuation,
                                     const std::string &domain)
{

    PaError err = Pa_Initialize();
    if (err != paNoError)
    {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << '\n';
        return 1;
    }

    // Initialize WebSocket client
    client c;
    std::string uri = url.c_str();

    c.set_access_channels(websocketpp::log::alevel::none);
    c.clear_access_channels(websocketpp::log::alevel::all);

    c.set_tls_init_handler([&](websocketpp::connection_hdl)
                           {
        auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv12);
        ctx->set_options(boost::asio::ssl::context::default_workarounds |
                         boost::asio::ssl::context::no_sslv2 |
                         boost::asio::ssl::context::no_sslv3 |
                         boost::asio::ssl::context::single_dh_use);
        ctx->set_verify_mode(boost::asio::ssl::verify_none);
        return ctx; });

    // c.set_tls_init_handler([&](websocketpp::connection_hdl)
    //                        {
    //     auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(
    //             boost::asio::ssl::context::tlsv12);
    //     ctx->set_verify_mode(boost::asio::ssl::verify_none);
    //     return ctx; });

    c.init_asio();

    // Init config message with language on websocket connection and start mic stream
    c.set_open_handler([&](websocketpp::connection_hdl hdl)
                       {
        nlohmann::json initConfig;
        initConfig["language"] = language;
        initConfig["manual_punctuation"] = manualPunctuation;
        initConfig["domain"] = domain;
        std::string initMessage = initConfig.dump();
        global_hdl = hdl;
        c.send(hdl, initMessage, websocketpp::frame::opcode::text);

        PaStream *stream;
        Pa_OpenDefaultStream(&stream, NUM_CHANNELS, 0, paFloat32, SAMPLE_RATE,
                             SAMPLE_RATE * CHUNK_SECONDS, audioCallback, &c);
        Pa_StartStream(stream); });

    // Websocket transcript receiving handler
    c.set_message_handler([&](websocketpp::connection_hdl hdl, client::message_ptr msg)
                          {
        
        nlohmann::json data = nlohmann::json::parse(msg->get_payload());        

        if (data.contains("transcript") && data["transcript"].is_array()) {
            std::string concatenatedTranscript;

            for (const auto &transcript : data["transcript"]) {
                concatenatedTranscript += " ";
                concatenatedTranscript += transcript["transcript"];
            }

            char* modifiableCStr = new char[concatenatedTranscript.length() + 1]; // +1 pour le caractère nul
            std::strcpy(modifiableCStr, concatenatedTranscript.c_str());
            updateText(modifiableCStr);
        } });

    websocketpp::lib::error_code ec;
    client::connection_ptr con = c.get_connection(uri, ec);
    if (ec)
    {
        std::cerr << "Connection error: " << ec.message() << '\n';
        return 1;
    }
    else
    {
        std::cout << "Connection success" << std::endl;
    }

    c.connect(con);

    std::thread ws_thread(wsThread, std::ref(c));
    c.run();

    std::cout << "Running..." << std::endl;

    ws_thread.join();

    std::cout << "Finishing started running" << std::endl;

    Pa_Terminate();

    return 0;
}

int STTTranscription::stopRecording()
{
    shouldRecord = false;
    ws_thread.detach();
    return 0;
}
