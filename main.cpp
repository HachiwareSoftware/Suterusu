#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

HHOOK hKeyboardHook;
std::string apiResponse;
std::mutex responseMutex;
bool responseReady = false;
std::atomic<int> activeThreads(0);
std::atomic<bool> programRunning(true);
std::thread apiThread;
std::mutex threadMutex;

std::string API_URL;
std::string API_KEY;
std::string MODEL;
std::string SYSTEM_PROMPT;
std::string FLASH_WINDOW = "Chrome";

// Console control handler to handle Ctrl+C properly
BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT)
    {
        std::cout << "\n[DEBUG] Console close signal received, cleaning up..." << std::endl;
        programRunning = false;
        Sleep(100); // Give time for threads to finish
        UnhookWindowsHookEx(hKeyboardHook);
        curl_global_cleanup();
        return TRUE;
    }
    return FALSE;
}

// Exit handler to catch unexpected termination
void ExitHandler()
{
    std::cout << "[DEBUG] Program is terminating! Stack trace:" << std::endl;
    std::cout << "[DEBUG] This should not happen during normal operation" << std::endl;
    std::cout.flush();
}

void FlashWindow(HWND hwnd)
{
    FLASHWINFO fwi;
    fwi.cbSize = sizeof(FLASHWINFO);
    fwi.hwnd = hwnd;
    fwi.dwFlags = FLASHW_TRAY;
    fwi.uCount = 3;
    fwi.dwTimeout = 0;
    FlashWindowEx(&fwi);
    Sleep(1600);

    fwi.dwFlags = FLASHW_STOP;
    fwi.uCount = 0;
    FlashWindowEx(&fwi);
}

// Callback to find and flash the first matching window
BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    char windowTitle[256];
    char className[256];
    GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));
    GetClassNameA(hwnd, className, sizeof(className));

    std::string title = windowTitle;
    std::string classStr = className;
    std::string target = FLASH_WINDOW;

    auto contains = [](std::string str, std::string sub)
    {
        std::transform(str.begin(), str.end(), str.begin(), ::tolower);
        std::transform(sub.begin(), sub.end(), sub.begin(), ::tolower);
        return str.find(sub) != std::string::npos;
    };

    bool shouldFlash = false;
    if (contains(target, "chrome"))
    {
        if (contains(classStr, "chrome_widgetwin") && !contains(title, "visual studio code") && !contains(title, "microsoft edge"))
            shouldFlash = true;
    }
    else if (contains(target, "firefox"))
    {
        if (contains(classStr, "mozillawindowclass"))
            shouldFlash = true;
    }
    else if (contains(target, "edge"))
    {
        if (contains(classStr, "chrome_widgetwin") && contains(title, "microsoft edge"))
            shouldFlash = true;
    }
    else if (contains(target, "vscode") || contains(target, "code"))
    {
        if (contains(classStr, "chrome_widgetwin") && contains(title, "visual studio code"))
            shouldFlash = true;
    }
    else if (contains(target, "notepad++"))
    {
        if (contains(classStr, "notepad++"))
            shouldFlash = true;
    }
    else if (contains(target, "word"))
    {
        if (contains(title, "microsoft word") || contains(title, "word"))
            shouldFlash = true;
    }
    else if (contains(target, "excel"))
    {
        if (contains(title, "microsoft excel") || contains(title, "excel"))
            shouldFlash = true;
    }
    else if (contains(target, "all"))
    {
        shouldFlash = true;
    }
    else if (contains(target, "none") || target.empty())
    {
        shouldFlash = false;
    }
    else
    {
        if (contains(title, target) || contains(classStr, target))
            shouldFlash = true;
    }

    if (shouldFlash && IsWindowVisible(hwnd))
    {
        std::cout << "[DEBUG] Found target window: " << windowTitle << std::endl;
        FlashWindow(hwnd);
        return FALSE;
    }
    return TRUE;
}

// Flash all windows matching the configured target
void FlashConfiguredWindows()
{
    std::cout << "[DEBUG] Flashing '" << FLASH_WINDOW << "' windows..." << std::endl;
    EnumWindows(EnumWindowsCallback, 0);
}

// Load configuration from config.json
bool LoadConfig()
{
    std::ifstream configFile("config.json");
    if (!configFile.is_open())
    {
        std::cout << "Creating default config.json..." << std::endl;
        json defaultConfig = {
            {"api_url", "http://localhost:8080/v1/chat/completions"},
            {"api_key", ""},
            {"model", "gpt-3.5-turbo"},
            {"system_prompt", "You are a helpful assistant."},
            {"flash_window", "Chrome"}};

        std::ofstream outFile("config.json");
        if (outFile.is_open())
        {
            outFile << defaultConfig.dump(2) << std::endl;
        }

        API_URL = defaultConfig["api_url"];
        API_KEY = defaultConfig["api_key"];
        MODEL = defaultConfig["model"];
        SYSTEM_PROMPT = defaultConfig["system_prompt"];
        FLASH_WINDOW = defaultConfig["flash_window"];
        return true;
    }

    try
    {
        json config;
        configFile >> config;
        API_URL = config.value("api_url", "");
        MODEL = config.value("model", "");
        API_KEY = config.value("api_key", "");
        SYSTEM_PROMPT = config.value("system_prompt", "You are a helpful assistant.");
        FLASH_WINDOW = config.value("flash_window", "Chrome");

        if (API_URL.empty() || MODEL.empty())
        {
            std::cerr << "Error: config.json missing 'api_url' or 'model'" << std::endl;
            return false;
        }
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Config error: " << e.what() << std::endl;
        return false;
    }
}

// Callback for curl to write response data
size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *userp)
{
    size_t totalSize = size * nmemb;
    userp->append((char *)contents, totalSize);
    return totalSize;
}

// Get clipboard text
std::string GetClipboardText()
{
    if (!OpenClipboard(nullptr))
    {
        std::cerr << "Failed to open clipboard" << std::endl;
        return "";
    }

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData == nullptr)
    {
        CloseClipboard();
        return "";
    }

    wchar_t *pszText = static_cast<wchar_t *>(GlobalLock(hData));
    if (pszText == nullptr)
    {
        CloseClipboard();
        return "";
    }

    // Convert wide string to UTF-8 string (properly handles Vietnamese)
    int size = WideCharToMultiByte(CP_UTF8, 0, pszText, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        std::cerr << "[DEBUG] Failed to get clipboard text size" << std::endl;
        GlobalUnlock(hData);
        CloseClipboard();
        return "";
    }

    std::string text(size - 1, 0);
    int result = WideCharToMultiByte(CP_UTF8, 0, pszText, -1, &text[0], size, nullptr, nullptr);

    GlobalUnlock(hData);
    CloseClipboard();

    return result == 0 ? "" : text;
}

// Set clipboard text
bool SetClipboardText(const std::string &text)
{
    if (!OpenClipboard(nullptr))
    {
        std::cerr << "Failed to open clipboard" << std::endl;
        return false;
    }

    EmptyClipboard();

    // Convert to wide string
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, size * sizeof(wchar_t));
    if (hGlob == nullptr)
    {
        CloseClipboard();
        return false;
    }

    wchar_t *pszText = static_cast<wchar_t *>(GlobalLock(hGlob));
    if (pszText == nullptr)
    {
        GlobalFree(hGlob);
        CloseClipboard();
        return false;
    }

    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pszText, size);
    GlobalUnlock(hGlob);

    SetClipboardData(CF_UNICODETEXT, hGlob);
    CloseClipboard();

    return true;
}

// Send text to OpenAI API
std::string SendToAPI(const std::string &prompt)
{
    CURL *curl = curl_easy_init();
    if (!curl)
        return "Error: Failed to initialize CURL";

    std::string responseString;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (!API_KEY.empty())
    {
        std::string authHeader = "Authorization: Bearer " + API_KEY;
        headers = curl_slist_append(headers, authHeader.c_str());
    }

    json messages = json::array();
    if (!SYSTEM_PROMPT.empty())
        messages.push_back({{"role", "system"}, {"content", SYSTEM_PROMPT}});
    messages.push_back({{"role", "user"}, {"content", prompt}});

    json payload = {{"model", MODEL}, {"messages", messages}, {"temperature", 0.7}};
    std::string jsonStr = payload.dump();

    curl_easy_setopt(curl, CURLOPT_URL, API_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        if (res == CURLE_OPERATION_TIMEDOUT)
            return "Error: Request timed out.";
        if (res == CURLE_COULDNT_CONNECT)
            return "Error: Could not connect to server.";
        if (res == CURLE_COULDNT_RESOLVE_HOST)
            return "Error: Could not resolve host.";
        return std::string("Error: ") + curl_easy_strerror(res);
    }

    try
    {
        json responseJson = json::parse(responseString);
        if (responseJson.contains("choices") && !responseJson["choices"].empty())
        {
            return responseJson["choices"][0]["message"]["content"];
        }
        else if (responseJson.contains("error"))
        {
            return "API Error: " + responseJson["error"]["message"].get<std::string>();
        }
    }
    catch (const std::exception &e)
    {
        return "Error parsing response: " + std::string(e.what());
    }
    return "Error: Unexpected response format";
}

// Keyboard hook callback
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && wParam == WM_KEYDOWN)
    {
        KBDLLHOOKSTRUCT *pKeyboard = (KBDLLHOOKSTRUCT *)lParam;

        if (pKeyboard->vkCode == VK_F7)
        {
            std::cout << "F7 pressed - Processing..." << std::endl;
            std::string clipboardText = GetClipboardText();

            if (clipboardText.empty())
            {
                std::cout << "Clipboard empty." << std::endl;
            }
            else
            {
                {
                    std::lock_guard<std::mutex> lock(threadMutex);
                    if (apiThread.joinable())
                        apiThread.join();
                }

                std::lock_guard<std::mutex> lock(threadMutex);
                apiThread = std::thread([clipboardText]()
                                        {
                    activeThreads++;
                    try {
                        std::string response = SendToAPI(clipboardText);
                        std::lock_guard<std::mutex> lock(responseMutex);
                        apiResponse = response;
                        responseReady = true;
                        std::cout << "Response received. Press F8 to copy." << std::endl;
                        FlashConfiguredWindows();
                    } catch (...) {
                        std::cerr << "Error in API thread." << std::endl;
                    }
                    activeThreads--; });
            }
        }
        else if (pKeyboard->vkCode == VK_F8)
        {
            std::lock_guard<std::mutex> lock(responseMutex);
            if (responseReady && !apiResponse.empty())
            {
                if (SetClipboardText(apiResponse))
                {
                    std::cout << "Clipboard updated." << std::endl;
                }
                else
                {
                    std::cout << "Failed to update clipboard." << std::endl;
                }
                responseReady = false;
            }
            else
            {
                std::cout << "No response available." << std::endl;
            }
        }
        else if (pKeyboard->vkCode == VK_F9)
        {
            programRunning = false;
            PostQuitMessage(0);
        }
    }
    return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}

// Custom streambuf to write directly to the console
// This bypasses issues with C runtime redirection in some MinGW/MSYS2 environments
class ConsoleStreamBuf : public std::streambuf
{
    HANDLE hConsole;

public:
    ConsoleStreamBuf(HANDLE h) : hConsole(h) {}

protected:
    virtual int_type overflow(int_type c) override
    {
        if (c != EOF)
        {
            char ch = static_cast<char>(c);
            DWORD written;
            WriteConsoleA(hConsole, &ch, 1, &written, NULL);
        }
        return c;
    }
    virtual std::streamsize xsputn(const char *s, std::streamsize n) override
    {
        DWORD written;
        WriteConsoleA(hConsole, s, static_cast<DWORD>(n), &written, NULL);
        return n;
    }
};

int main(int argc, char *argv[])
{
    // Check for --debug flag
    bool debugMode = false;
    for (int i = 1; i < argc; i++)
    {
        if (std::string(argv[i]) == "--debug")
        {
            debugMode = true;
            break;
        }
    }

    // Allocate console if debug mode is enabled
    if (debugMode)
    {
        if (AllocConsole())
        {
            // Set console to UTF-8 to properly display Vietnamese characters
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);

            // Open a handle to the console output
            // We use CreateFile("CONOUT$") to ensure we get the real console handle
            HANDLE hConOut = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

            if (hConOut != INVALID_HANDLE_VALUE)
            {
                // Redirect C++ streams to use our custom buffer wrapping the console handle
                // This ensures std::cout works even if freopen fails
                static ConsoleStreamBuf consoleBuf(hConOut);
                std::cout.rdbuf(&consoleBuf);
                std::cerr.rdbuf(&consoleBuf);
                std::clog.rdbuf(&consoleBuf);

                // Also try to fix C stdio for libraries that might use printf
                // We try both standard Windows and MSYS2 paths
                if (!freopen("CONOUT$", "w", stdout))
                {
                    freopen("/dev/console", "w", stdout);
                }
                if (!freopen("CONOUT$", "w", stderr))
                {
                    freopen("/dev/console", "w", stderr);
                }
                if (!freopen("CONIN$", "r", stdin))
                {
                    freopen("/dev/console", "r", stdin);
                }

                // Disable buffering
                setvbuf(stdout, NULL, _IONBF, 0);
                setvbuf(stderr, NULL, _IONBF, 0);

                std::cout << "[DEBUG] Console allocated and streams redirected via custom buffer." << std::endl;
            }
        }
    }

    // Register exit handler to catch unexpected termination
    std::atexit(ExitHandler);

    // Set console control handler to properly handle closing (only in debug mode)
    if (debugMode)
    {
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    }

    std::cout << "=== Suterusu ===" << std::endl;
    std::cout << "Loading configuration from config.json..." << std::endl;

    if (!LoadConfig())
    {
        std::cerr << "Failed to load configuration. Exiting." << std::endl;
        return 1;
    }

    printf("Debug mode: %s\n", debugMode ? "Enabled" : "Disabled");
    std::cout << std::endl;
    std::cout << "Configuration loaded successfully:" << std::endl;
    std::cout << "  API URL: " << API_URL << std::endl;
    std::cout << "  Model: " << MODEL << std::endl;
    std::cout << "  API Key: " << (API_KEY.empty() ? "(not set)" : "********") << std::endl;
    std::cout << "  System Prompt: " << (SYSTEM_PROMPT.empty() ? "(default)" : SYSTEM_PROMPT.substr(0, 50) + (SYSTEM_PROMPT.length() > 50 ? "..." : "")) << std::endl;
    std::cout << "  Flash Window: " << FLASH_WINDOW << std::endl;
    std::cout << std::endl;
    std::cout << "Controls:" << std::endl;
    std::cout << "  F7 - Read clipboard and send to API" << std::endl;
    std::cout << "  F8 - Replace clipboard with API response" << std::endl;
    std::cout << "  F9 - Quit application" << std::endl;
    std::cout << std::endl;
    std::cout << "Waiting for key presses..." << std::endl;
    std::cout.flush();

    // Initialize curl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Install keyboard hook
    hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, nullptr, 0);
    if (hKeyboardHook == nullptr)
    {
        std::cerr << "Failed to install keyboard hook!" << std::endl;
        curl_global_cleanup();
        return 1;
    }

    std::cout << "[DEBUG] Keyboard hook installed successfully" << std::endl;

    // Message loop to keep the program running
    MSG msg;
    while (programRunning)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                programRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        Sleep(10);
    }

    {
        std::lock_guard<std::mutex> lock(threadMutex);
        if (apiThread.joinable())
            apiThread.join();
    }

    while (activeThreads > 0)
        Sleep(100);

    UnhookWindowsHookEx(hKeyboardHook);
    curl_global_cleanup();
    return 0;
}
