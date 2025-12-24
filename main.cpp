#include <windows.h>
#include <psapi.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "overlay.h"

using json = nlohmann::json;

// CDP injector DLL functions
extern "C" __declspec(dllimport) bool InitializeCDP();
extern "C" __declspec(dllimport) void ShutdownCDP();
extern "C" __declspec(dllimport) bool InjectJavaScript(const char* filename);

HHOOK hKeyboardHook;
std::string apiResponse;
std::mutex responseMutex;
bool responseReady = false;
std::atomic<int> activeThreads(0);
std::atomic<bool> programRunning(true);
std::thread apiThread;
std::mutex threadMutex;
json chatHistory = json::array();
std::mutex historyMutex;
std::string API_URL;
std::string API_KEY;
std::string MODEL;
std::vector<std::string> FALLBACK_MODELS; // Array of fallback models for automatic failover
std::vector<std::string> AI_PROVIDERS; // Array of provider names for OpenRouter routing
std::string SYSTEM_PROMPT;
std::string FLASH_WINDOW = "Chrome";

// Persistent CURL handle for connection reuse (low latency optimization)
CURL *persistentCurl = nullptr;
std::mutex curlMutex;

BOOL WINAPI ConsoleHandler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT)
    {
        std::cout << "\n[DEBUG] Console close signal received, cleaning up..." << std::endl;
        programRunning = false;
        Sleep(100);
        UnhookWindowsHookEx(hKeyboardHook);
        
        // Clean up persistent CURL handle
        if (persistentCurl)
        {
            curl_easy_cleanup(persistentCurl);
            persistentCurl = nullptr;
        }
        curl_global_cleanup();
        return TRUE;
    }
    return FALSE;
}

void ExitHandler()
{
    std::cout << "[DEBUG] Program is terminating! This should not happen during normal operation" << std::endl;
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

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    // Pretend to use lParam to suppress unused parameter warning
    (void)lParam;

    // Skip invisible windows early
    if (!IsWindowVisible(hwnd))
        return TRUE;
    
    // Get process ID from window handle
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    
    if (processId == 0)
        return TRUE;
    
    // Open process with query permissions
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (hProcess == NULL)
        return TRUE;
    
    // Get executable path
    char exePath[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameA(hProcess, 0, exePath, &size) == 0)
    {
        CloseHandle(hProcess);
        return TRUE;
    }
    CloseHandle(hProcess);
    
    // Extract executable name from full path
    std::string fullPath = exePath;
    size_t lastSlash = fullPath.find_last_of("\\/");
    std::string exeName = (lastSlash != std::string::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
    
    // Convert to lowercase for case-insensitive comparison
    std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::tolower);
    
    std::string target = FLASH_WINDOW;
    std::transform(target.begin(), target.end(), target.begin(), ::tolower);
    
    bool shouldFlash = false;
    
    // Special cases
    if (target == "all")
    {
        shouldFlash = true;
    }
    else if (target == "none" || target.empty())
    {
        shouldFlash = false;
    }
    else
    {
        // Check if target matches executable name (with or without .exe extension)
        std::string targetWithExt = target;
        if (target.find(".exe") == std::string::npos)
            targetWithExt += ".exe";
        
        if (exeName == targetWithExt || exeName == target)
            shouldFlash = true;
        // Also support partial matching (e.g., "chrome" matches "chrome.exe")
        else if (exeName.find(target) != std::string::npos)
            shouldFlash = true;
    }
    
    if (shouldFlash)
    {
        char windowTitle[256];
        GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));
        std::cout << "[DEBUG] Found target window: " << windowTitle << " (Process: " << exeName << ")" << std::endl;
        FlashWindow(hwnd);
        return FALSE;
    }
    
    return TRUE;
}

void FlashConfiguredWindows()
{
    std::cout << "[DEBUG] Flashing '" << FLASH_WINDOW << "' windows..." << std::endl;
    EnumWindows(EnumWindowsCallback, 0);
}

bool LoadConfig()
{
    std::ifstream promptFile("system_prompt.md");
    if (promptFile.is_open()) {
        std::stringstream buffer;
        buffer << promptFile.rdbuf();
        SYSTEM_PROMPT = buffer.str();
    } else {
        SYSTEM_PROMPT = "You are a helpful assistant.";
    }
    std::ifstream configFile("config.json");
    if (!configFile.is_open())
    {
        std::cout << "Creating default config.json..." << std::endl;
        json defaultConfig = {
            {"api_url", "http://localhost:8080/v1/chat/completions"},
            {"api_key", ""},
            {"ai", {
                {"model", "openai/gpt-3.5-turbo"},
                {"fallback_models", json::array()},
                {"providers", json::array()}
            }},
            {"flash_window", "Chrome"}
        };
        std::ofstream outFile("config.json");
        if (outFile.is_open())
            outFile << defaultConfig.dump(2) << std::endl;
        API_URL = defaultConfig["api_url"];
        API_KEY = defaultConfig["api_key"];
        MODEL = defaultConfig["ai"]["model"];
        FLASH_WINDOW = defaultConfig["flash_window"];
        return true;
    }
    try
    {
        json config;
        configFile >> config;
        API_URL = config.value("api_url", "");
        API_KEY = config.value("api_key", "");
        FLASH_WINDOW = config.value("flash_window", "Chrome");
        
        // Parse ai section
        if (!config.contains("ai"))
        {
            std::cerr << "Error: config.json missing 'ai' section" << std::endl;
            return false;
        }
        
        json aiConfig = config["ai"];
        if (!aiConfig.contains("model") || aiConfig["model"].get<std::string>().empty())
        {
            std::cerr << "Error: config.json missing 'ai.model'" << std::endl;
            return false;
        }
        MODEL = aiConfig["model"];
        
        // Parse fallback_models array (optional)
        if (aiConfig.contains("fallback_models") && aiConfig["fallback_models"].is_array())
        {
            for (const auto& fallbackModel : aiConfig["fallback_models"])
            {
                if (fallbackModel.is_string())
                    FALLBACK_MODELS.push_back(fallbackModel.get<std::string>());
            }
            if (!FALLBACK_MODELS.empty())
                std::cout << "[DEBUG] Loaded " << FALLBACK_MODELS.size() << " fallback model(s)" << std::endl;
        }
        
        // Parse providers array (optional)
        if (aiConfig.contains("providers") && aiConfig["providers"].is_array())
        {
            for (const auto& provider : aiConfig["providers"])
            {
                if (provider.is_string())
                    AI_PROVIDERS.push_back(provider.get<std::string>());
            }
            if (!AI_PROVIDERS.empty())
                std::cout << "[DEBUG] Loaded " << AI_PROVIDERS.size() << " provider routing preference(s)" << std::endl;
        }
        
        if (API_URL.empty())
        {
            std::cerr << "Error: config.json missing 'api_url'" << std::endl;
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

size_t WriteCallback(void *contents, size_t size, size_t nmemb, std::string *userp)
{
    size_t totalSize = size * nmemb;
    userp->append((char *)contents, totalSize);
    return totalSize;
}

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

bool SetClipboardText(const std::string &text)
{
    if (!OpenClipboard(nullptr))
    {
        std::cerr << "Failed to open clipboard" << std::endl;
        return false;
    }
    EmptyClipboard();
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

void InitializeCurl()
{
    std::lock_guard<std::mutex> curlLock(curlMutex);
    
    if (persistentCurl)
        return; // Already initialized
    
    persistentCurl = curl_easy_init();
    if (!persistentCurl)
    {
        std::cerr << "[ERROR] Failed to initialize CURL handle" << std::endl;
        return;
    }
    
    // LOW LATENCY OPTIMIZATIONS - Set once for the persistent handle
    
    // Enable HTTP/2 if available (multiplexing, better performance)
    curl_easy_setopt(persistentCurl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
    
    // Disable Nagle's algorithm for lower latency (send small packets immediately)
    curl_easy_setopt(persistentCurl, CURLOPT_TCP_NODELAY, 1L);
    
    // Enable connection reuse and pooling
    curl_easy_setopt(persistentCurl, CURLOPT_MAXCONNECTS, 5L);
    
    // Enable TCP keep-alive to maintain connections
    curl_easy_setopt(persistentCurl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(persistentCurl, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(persistentCurl, CURLOPT_TCP_KEEPINTVL, 60L);
    
    // Set reduced connection timeout (5 seconds instead of 30)
    curl_easy_setopt(persistentCurl, CURLOPT_CONNECTTIMEOUT, 5L);
    
    // Set reasonable total timeout
    curl_easy_setopt(persistentCurl, CURLOPT_TIMEOUT, 120L);
    
    // Required for multi-threaded applications
    curl_easy_setopt(persistentCurl, CURLOPT_NOSIGNAL, 1L);
    
    // Enable DNS caching
    curl_easy_setopt(persistentCurl, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
    
    // Use HTTP pipelining for better performance
    curl_easy_setopt(persistentCurl, CURLOPT_PIPEWAIT, 1L);

    /* Tells libcurl to use standard certificate store of operating system.
       Currently implemented under MS-Windows. */
    curl_easy_setopt(persistentCurl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);

    std::cout << "[DEBUG] Initialized persistent CURL handle with low-latency optimizations" << std::endl;
}

// Extract content between specified tags
std::string ExtractTag(const std::string& text, const std::string& tag) {
    std::string openTag = "<" + tag + ">";
    std::string closeTag = "</" + tag + ">";
    
    size_t startPos = text.find(openTag);
    size_t endPos = text.find(closeTag);

    // If both open and close tags are found, extract content between them
    if (startPos != std::string::npos && endPos != std::string::npos && endPos > startPos) {
        startPos += openTag.length();
        return text.substr(startPos, endPos - startPos);
    }
    
    // Fallback to return the whole text if tags are not found
    return text; 
}

// Send text to OpenAI API
std::string SendToAPI(const std::string &prompt)
{
    std::lock_guard<std::mutex> curlLock(curlMutex);
    
    if (!persistentCurl)
        return "Error: CURL not initialized";
    
    std::string responseString;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    // Enable HTTP keep-alive
    headers = curl_slist_append(headers, "Connection: keep-alive");
    
    if (!API_KEY.empty())
    {
        std::string authHeader = "Authorization: Bearer " + API_KEY;
        headers = curl_slist_append(headers, authHeader.c_str());
    }
    
    json messages = json::array();
    if (!SYSTEM_PROMPT.empty())
        messages.push_back({{"role", "system"}, {"content", SYSTEM_PROMPT}});
    {
        std::lock_guard<std::mutex> lock(historyMutex);
        size_t historySize = chatHistory.size();
        // Keep last 20 messages (10 conversation pairs: user + assistant)
        size_t start = (historySize > 20) ? (historySize - 20) : 0;
        std::cout << "[DEBUG] Chat history size: " << historySize << " messages, using last " << (historySize - start) << " messages" << std::endl;
        for (size_t i = start; i < historySize; ++i)
            messages.push_back(chatHistory[i]);
    }
    messages.push_back({{"role", "user"}, {"content", prompt}});
    
    // Build payload with optional provider routing and model fallbacks
    json payload = {
        {"model", MODEL}, 
        {"messages", messages}, 
        {"temperature", 0.7}
    };
    
    // Add model fallbacks if specified (for OpenRouter automatic failover)
    if (!FALLBACK_MODELS.empty())
    {
        // Build fallback models array
        json modelsArray = json::array();
        for (const auto& fallbackModel : FALLBACK_MODELS)
        {
            modelsArray.push_back(fallbackModel);
        }
        payload["models"] = modelsArray;
        std::cout << "[DEBUG] Including model fallbacks: " << modelsArray.dump() << std::endl;
    }
    
    // Add provider routing if providers are specified (for OpenRouter)
    if (!AI_PROVIDERS.empty())
    {
        payload["provider"] = {
            {"order", AI_PROVIDERS},
            {"allow_fallbacks", true}
        };
        std::cout << "[DEBUG] Including provider routing: " << json(AI_PROVIDERS).dump() << std::endl;
    }
    
    std::string jsonStr = payload.dump();
    
    // Set request-specific options
    curl_easy_setopt(persistentCurl, CURLOPT_URL, API_URL.c_str());
    curl_easy_setopt(persistentCurl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(persistentCurl, CURLOPT_POSTFIELDS, jsonStr.c_str());
    curl_easy_setopt(persistentCurl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(persistentCurl, CURLOPT_WRITEDATA, &responseString);
    
    CURLcode res = curl_easy_perform(persistentCurl);
    long response_code = 0;
    curl_easy_getinfo(persistentCurl, CURLINFO_RESPONSE_CODE, &response_code);
    
    // Clean up headers
    curl_slist_free_all(headers);
    
    // Don't cleanup the persistent handle - it will be reused
    
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
            std::string content = responseJson["choices"][0]["message"]["content"];
            // Assume the answer is wrapped in <answer> tags
            // User should specify this in the system prompt
            std::string finalAnswer = ExtractTag(content, "answer");
            {
                std::lock_guard<std::mutex> lock(historyMutex);
                chatHistory.push_back({{"role", "user"}, {"content", prompt}});
                chatHistory.push_back({{"role", "assistant"}, {"content", content}});
                std::cout << "[DEBUG] Added conversation to history. Total messages: " << chatHistory.size() << std::endl;
            }
            return finalAnswer;
        }
        else if (responseJson.contains("error"))
            return "API Error: " + responseJson["error"]["message"].get<std::string>();
    }
    catch (const std::exception &e)
    {
        std::cout << "[DEBUG] Full API response: " << responseString << std::endl;
        return "Error parsing response: " + std::string(e.what());
    }
    std::cout << "[DEBUG] Full API response: " << responseString << std::endl;
    return "Error: Unexpected response format";
}

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0 || wParam != WM_KEYDOWN)
        return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
    DWORD vk = ((KBDLLHOOKSTRUCT *)lParam)->vkCode;
    switch (vk)
    {
    case VK_F6:
    {
        std::lock_guard<std::mutex> lock(historyMutex);
        chatHistory.clear();
        std::cout << "Chat history cleared." << std::endl;
        break;
    }
    case VK_F7:
    {
        std::cout << "F7 pressed - Processing..." << std::endl;
        std::string clipboardText = GetClipboardText();
        if (clipboardText.empty())
        {
            std::cout << "Clipboard empty." << std::endl;
            break;
        }
        // Show green indicator when clipboard read succeeds and sending to API
        ShowOverlayIndicator(700, IndicatorColor::Green);
        
        std::lock_guard<std::mutex> lock(threadMutex);
        if (apiThread.joinable())
            apiThread.join();
        apiThread = std::thread([clipboardText]()
        {
            activeThreads++;
            try
            {
                std::string response = SendToAPI(clipboardText);
                std::lock_guard<std::mutex> lock(responseMutex);
                apiResponse = response;
                responseReady = true;
                std::cout << "Response received. Press F8 to copy." << std::endl;
                // Show green indicator with first character of response
                const char* firstChar = response.empty() ? nullptr : response.c_str();
                ShowOverlayIndicator(3000, IndicatorColor::Green, firstChar);
                FlashConfiguredWindows();
            }
            catch (...) { std::cerr << "Error in API thread." << std::endl; }
            activeThreads--;
        });
        break;
    }
    case VK_F8:
    {
        std::lock_guard<std::mutex> lock(responseMutex);
        if (responseReady && !apiResponse.empty())
        {
            if (SetClipboardText(apiResponse))
            {
                std::cout << "Clipboard updated." << std::endl;
                // Show green indicator on successful clipboard update
                const char* firstChar = apiResponse.empty() ? nullptr : apiResponse.c_str();
                ShowOverlayIndicator(1000, IndicatorColor::Green, firstChar);
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
            // Show red indicator when no response is available
            ShowOverlayIndicator(1000, IndicatorColor::Red);
        }
        break;
    }
    case VK_F9:
    {
        std::cout << "F9 pressed - Toggling Selection Highlighting..." << std::endl;
        if (InjectJavaScript("js/toggle_selection.js"))
        {
            std::cout << "[DEBUG] JavaScript injection successful!" << std::endl;
            ShowOverlayIndicator(1000, IndicatorColor::Green);
        }
        else
        {
            std::cout << "[DEBUG] JavaScript injection failed!" << std::endl;
            ShowOverlayIndicator(1000, IndicatorColor::Red);
        }
        break;
    }
    // Use F12 to quit the application to prevent accidental closure
    case VK_F12:
        programRunning = false;
        PostQuitMessage(0);
        break;
    }
    return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}

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
    bool debugMode = false;
    for (int i = 1; i < argc; i++)
    {
        if (std::string(argv[i]) == "--debug")
        {
            debugMode = true;
            break;
        }
    }
    if (debugMode)
    {
        if (AllocConsole())
        {
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);
            HANDLE hConOut = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hConOut != INVALID_HANDLE_VALUE)
            {
                static ConsoleStreamBuf consoleBuf(hConOut);
                std::cout.rdbuf(&consoleBuf);
                std::cerr.rdbuf(&consoleBuf);
                std::clog.rdbuf(&consoleBuf);
                if (!freopen("CONOUT$", "w", stdout))
                    freopen("/dev/console", "w", stdout);
                if (!freopen("CONOUT$", "w", stderr))
                    freopen("/dev/console", "w", stderr);
                if (!freopen("CONIN$", "r", stdin))
                    freopen("/dev/console", "r", stdin);
                setvbuf(stdout, NULL, _IONBF, 0);
                setvbuf(stderr, NULL, _IONBF, 0);
                std::cout << "[DEBUG] Console allocated and streams redirected via custom buffer." << std::endl;
            }
        }
    }
    std::atexit(ExitHandler);
    if (debugMode)
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
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
    if (!FALLBACK_MODELS.empty())
    {
        std::cout << "  Fallback Models: ";
        for (size_t i = 0; i < FALLBACK_MODELS.size(); ++i)
        {
            std::cout << FALLBACK_MODELS[i];
            if (i < FALLBACK_MODELS.size() - 1)
                std::cout << ", ";
        }
        std::cout << std::endl;
    }
    if (!AI_PROVIDERS.empty())
    {
        std::cout << "  Providers: ";
        for (size_t i = 0; i < AI_PROVIDERS.size(); ++i)
        {
            std::cout << AI_PROVIDERS[i];
            if (i < AI_PROVIDERS.size() - 1)
                std::cout << ", ";
        }
        std::cout << std::endl;
    }
    std::cout << "  API Key: " << (API_KEY.empty() ? "(not set)" : "********") << std::endl;
    std::cout << "  System Prompt: " << (SYSTEM_PROMPT.empty() ? "(default)" : SYSTEM_PROMPT.substr(0, 50) + (SYSTEM_PROMPT.length() > 50 ? "..." : "")) << std::endl;
    std::cout << "  Flash Window: " << FLASH_WINDOW << std::endl;
    std::cout << std::endl;
    std::cout << "Controls:" << std::endl;
    std::cout << "  F6 - Clear chat history" << std::endl;
    std::cout << "  F7 - Read clipboard and send to API" << std::endl;
    std::cout << "  F8 - Replace clipboard with API response" << std::endl;
    std::cout << "  F9 - Toggle text selection" << std::endl;
    std::cout << "  F12 - Quit application" << std::endl;
    std::cout << std::endl;
    
    // Initialize CURL before showing ready message
    curl_global_init(CURL_GLOBAL_DEFAULT);
    InitializeCurl();
    
    // Initialize CDP connection (will auto-connect and reconnect in background thread)
    std::cout << "Initializing Chrome DevTools connection (background)..." << std::endl;
    InitializeCDP();
    std::cout << "CDP will automatically connect when browser is available..." << std::endl;
    
    std::cout << "Waiting for key presses..." << std::endl;
    std::cout.flush();
    
    hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, nullptr, 0);
    if (hKeyboardHook == nullptr)
    {
        std::cerr << "Failed to install keyboard hook!" << std::endl;
        curl_global_cleanup();
        return 1;
    }
    std::cout << "[DEBUG] Keyboard hook installed successfully" << std::endl;
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
    
    // Clean up CDP connection
    ShutdownCDP();
    
    // Clean up persistent CURL handle
    if (persistentCurl)
    {
        curl_easy_cleanup(persistentCurl);
        persistentCurl = nullptr;
    }
    curl_global_cleanup();
    return 0;
}
