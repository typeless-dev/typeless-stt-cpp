#include <iostream>
#include <string>
#include "STTTranscription.h"

void clearConsole()
{
    system("clear");
}

void updateText(char *text)
{
    clearConsole();
    std::cout << text << std::endl;
    delete[] text;
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        std::cerr << "Usage: " << argv[0] << " <URL> <Language> <Manual Punctuation (0 or 1)>" << std::endl;
        return 1;
    }

    std::string url = argv[1];
    std::string language = argv[2];
    bool manual_punctuation = std::stoi(argv[3]);

    STTTranscription transcriber;

    std::cout << "Starting recording..." << std::endl;

    if (transcriber.startRecording(updateText, url, language, manual_punctuation) != 0)
    {
        std::cerr << "Failed to start recording." << std::endl;
        return 1;
    }

    std::cout << "Recording stopped." << std::endl;

    return 0;
}