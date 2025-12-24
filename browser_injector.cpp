#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/version.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
using json = nlohmann::json;

// Persistent WebSocket connection manager
class CDPConnection {
private:
    std::unique_ptr<boost::asio::io_context> ioc;
    std::unique_ptr<websocket::stream<tcp::socket>> ws;
    std::mutex ws_mutex;
    std::atomic<bool> connected{false};
    std::atomic<bool> should_reconnect{true};
    std::atomic<int> message_id{1};
    std::string ws_url;
    std::thread reconnect_thread;
    std::atomic<bool> reconnect_thread_running{false};
    
    void ReconnectThreadFunc() {
        reconnect_thread_running = true;
        const int RETRY_INTERVAL_MS = 1000; // Retry every 1 second
        const int HEARTBEAT_INTERVAL_MS = 500; // Check connection health every 5 seconds
        auto last_heartbeat = std::chrono::steady_clock::now();
        
        while (should_reconnect) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat).count();
            
            {
                std::lock_guard<std::mutex> lock(ws_mutex);
                
                if (!connected) {
                    std::cout << "[CDP] Attempting to connect to browser...\n";
                    if (EstablishConnection()) {
                        std::cout << "[CDP] Successfully connected to browser\n";
                        last_heartbeat = std::chrono::steady_clock::now();
                    }
                } else if (elapsed >= HEARTBEAT_INTERVAL_MS) {
                    // Perform periodic health check
                    if (!HealthCheck()) {
                        std::cout << "[CDP] Connection lost, will reconnect...\n";
                        connected = false;
                    }
                    last_heartbeat = std::chrono::steady_clock::now();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_INTERVAL_MS));
        }
        reconnect_thread_running = false;
    }
    
    bool HealthCheck() {
        // Check if socket is still open
        try {
            if (!ws || !ws->next_layer().is_open()) {
                return false;
            }
            // Try to send a simple command to verify connection is alive
            json ping_msg = {
                {"id", message_id++},
                {"method", "Browser.getVersion"},
                {"params", json::object()}
            };
            
            std::string ping_text = ping_msg.dump();
            ws->write(boost::asio::buffer(ping_text));
            
            boost::beast::flat_buffer response_buffer;
            ws->read(response_buffer);
            
            return true;
        } catch (...) {
            return false;
        }
    }
    
    bool EstablishConnection() {
        try {
            const char *host = "127.0.0.1";
            const char *port = "27245";
            
            // Safely close old connection before creating new io_context
            if (ws) {
                try {
                    ws->close(websocket::close_code::normal);
                } catch (...) {}
                ws.reset();
            }
            
            // Reset io_context and websocket
            ioc = std::make_unique<boost::asio::io_context>();
            
            // Query /json for WebSocket URL
            tcp::resolver resolver{*ioc};
            auto const results = resolver.resolve(host, port);
            
            boost::beast::tcp_stream http_stream{*ioc};
            http_stream.connect(results);
            
            http::request<http::string_body> req{http::verb::get, "/json", 11};
            req.set(http::field::host, host);
            req.set(http::field::user_agent, "Mozilla/5.0");
            http::write(http_stream, req);
            
            boost::beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(http_stream, buffer, res);
            http_stream.socket().shutdown(tcp::socket::shutdown_both);
            
            // Parse JSON to find WebSocket URL
            auto j = json::parse(res.body());
            if (!j.is_array() || j.empty()) {
                std::cerr << "[CDP] No targets found\n";
                return false;
            }
            
            ws_url.clear();
            for (auto &entry : j) {
                if (entry.contains("type") && entry["type"] == "page" &&
                    entry.contains("webSocketDebuggerUrl")) {
                    ws_url = entry["webSocketDebuggerUrl"].get<std::string>();
                    break;
                }
            }
            
            if (ws_url.empty()) {
                std::cerr << "[CDP] No page target with webSocketDebuggerUrl\n";
                return false;
            }
            
            std::cout << "[CDP] Using WebSocket URL: " << ws_url << "\n";
            
            // Parse WebSocket URL
            std::string ws_host = host;
            std::string ws_port = port;
            std::string ws_path;
            
            {
                auto pos = ws_url.find("://");
                auto rest = (pos == std::string::npos) ? ws_url : ws_url.substr(pos + 3);
                auto slash_pos = rest.find('/');
                auto hostport = rest.substr(0, slash_pos);
                ws_path = rest.substr(slash_pos);
                
                auto colon_pos = hostport.find(':');
                if (colon_pos != std::string::npos) {
                    ws_host = hostport.substr(0, colon_pos);
                    ws_port = hostport.substr(colon_pos + 1);
                } else {
                    ws_host = hostport;
                }
            }
            
            // Establish WebSocket connection
            tcp::resolver ws_resolver{*ioc};
            auto const ws_results = ws_resolver.resolve(ws_host, ws_port);
            
            ws = std::make_unique<websocket::stream<tcp::socket>>(*ioc);
            boost::asio::connect(ws->next_layer(), ws_results);
            
            std::string host_header = ws_host + ":" + ws_port;
            ws->handshake(host_header, ws_path);
            
            connected = true;
            std::cout << "[CDP] WebSocket connection established\n";
            
            // Enable Page domain (required for addScriptToEvaluateOnNewDocument)
            try {
                json enable_msg = {
                    {"id", message_id++},
                    {"method", "Page.enable"},
                    {"params", json::object()}
                };
                
                std::string enable_text = enable_msg.dump();
                ws->write(boost::asio::buffer(enable_text));
                
                boost::beast::flat_buffer enable_buffer;
                ws->read(enable_buffer);
                std::cout << "[CDP] Page domain enabled\n";
            } catch (...) {
                std::cerr << "[CDP] Warning: Could not enable Page domain\n";
            }
            
            return true;
            
        } catch (std::exception const &e) {
            std::cerr << "[CDP] Connection error: " << e.what() << "\n";
            connected = false;
            return false;
        }
    }
    
public:
    bool EnsureConnected() {
        std::lock_guard<std::mutex> lock(ws_mutex);
        
        if (connected && ws) {
            // Test if connection is still alive
            try {
                // Check if socket is still open
                if (ws->next_layer().is_open()) {
                    return true;
                }
            } catch (...) {
                connected = false;
            }
        }
        
        // Reconnect if needed
        std::cout << "[CDP] Ensuring connection...\n";
        return EstablishConnection();
    }
    
    bool IsConnected() const {
        return connected;
    }
    
    bool SendCommand(const std::string& method, const json& params, std::string& response) {
        std::lock_guard<std::mutex> lock(ws_mutex);
        
        if (!connected || !ws) {
            std::cerr << "[CDP] Not connected\n";
            return false;
        }
        
        try {
            // Verify socket is still open before sending
            if (!ws->next_layer().is_open()) {
                std::cerr << "[CDP] Socket is closed\n";
                connected = false;
                return false;
            }
            
            json msg = {
                {"id", message_id++},
                {"method", method},
                {"params", params}
            };
            
            std::string msg_text = msg.dump();
            ws->write(boost::asio::buffer(msg_text));
            
            // Read response
            boost::beast::flat_buffer ws_buffer;
            ws->read(ws_buffer);
            response = boost::beast::buffers_to_string(ws_buffer.data());
            
            return true;
            
        } catch (std::exception const &e) {
            std::cerr << "[CDP] Send error: " << e.what() << "\n";
            connected = false;
            // Try to close the socket gracefully
            try {
                if (ws && ws->next_layer().is_open()) {
                    ws->next_layer().close();
                }
            } catch (...) {}
            return false;
        }
    }
    
    void StartAutoReconnect() {
        std::lock_guard<std::mutex> lock(ws_mutex);
        
        if (reconnect_thread_running) {
            return; // Already running
        }
        
        should_reconnect = true;
        reconnect_thread = std::thread(&CDPConnection::ReconnectThreadFunc, this);
        // Don't detach - let destructor handle cleanup
    }
    
    void StopAutoReconnect() {
        should_reconnect = false;
        // Wait for thread to finish gracefully
        if (reconnect_thread.joinable()) {
            reconnect_thread.join();
        }
    }
    
    void Disconnect() {
        std::lock_guard<std::mutex> lock(ws_mutex);
        
        if (ws) {
            try {
                if (ws->next_layer().is_open()) {
                    ws->close(websocket::close_code::normal);
                    ws->next_layer().close();
                }
            } catch (...) {}
            ws.reset();
        }
        
        connected = false;
        std::cout << "[CDP] Connection closed\n";
    }
    
    ~CDPConnection() {
        StopAutoReconnect();
        Disconnect();
    }
};

// Global persistent connection
static std::unique_ptr<CDPConnection> g_cdp_connection;
static std::mutex g_cdp_mutex;
static std::string g_last_script_id; // Store script ID for removal

extern "C" __declspec(dllexport) bool InitializeCDP() {
    std::lock_guard<std::mutex> lock(g_cdp_mutex);
    
    if (!g_cdp_connection) {
        g_cdp_connection = std::make_unique<CDPConnection>();
    }
    
    // Start auto-reconnect thread
    g_cdp_connection->StartAutoReconnect();
    
    return true;
}

extern "C" __declspec(dllexport) void ShutdownCDP() {
    std::lock_guard<std::mutex> lock(g_cdp_mutex);
    
    if (g_cdp_connection) {
        g_cdp_connection->Disconnect();
        g_cdp_connection.reset();
    }
}

extern "C" __declspec(dllexport) bool InjectJavaScript(const char* filename)
{
    std::lock_guard<std::mutex> lock(g_cdp_mutex);
    
    // Ensure connection is established
    if (!g_cdp_connection) {
        g_cdp_connection = std::make_unique<CDPConnection>();
    }
    
    // Wait up to 10 seconds for connection with retries
    for (int i = 0; i < 50; ++i) {
        if (g_cdp_connection->IsConnected()) {
            break;
        }
        if (i == 0) {
            std::cout << "[CDP] Waiting for browser connection...\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    if (!g_cdp_connection->IsConnected()) {
        std::cerr << "[CDP] Failed to establish connection to browser\n";
        return false;
    }
    
    try {
        // Load JavaScript code from file
        std::ifstream jsFile(filename);
        if (!jsFile.is_open()) {
            std::cerr << "[CDP] Error: Could not open " << filename << "\n";
            return false;
        }
        
        std::string jsCode{std::istreambuf_iterator<char>(jsFile),
                          std::istreambuf_iterator<char>()};
        jsFile.close();
        
        // Remove previous script if exists
        if (!g_last_script_id.empty()) {
            json remove_params = {{"identifier", g_last_script_id}};
            std::string remove_response;
            g_cdp_connection->SendCommand("Page.removeScriptToEvaluateOnNewDocument", remove_params, remove_response);
            std::cout << "[CDP] Removed previous script\n";
            g_last_script_id.clear();
        }
        
        // Use Page.addScriptToEvaluateOnNewDocument for persistent injection
        json params = {{"source", jsCode}};
        
        std::string response;
        if (!g_cdp_connection->SendCommand("Page.addScriptToEvaluateOnNewDocument", params, response)) {
            std::cerr << "[CDP] Failed to add script\n";
            return false;
        }
        
        // Parse response to get script ID
        try {
            auto resp_json = json::parse(response);
            if (resp_json.contains("result") && resp_json["result"].contains("identifier")) {
                g_last_script_id = resp_json["result"]["identifier"].get<std::string>();
                std::cout << "[CDP] Script registered (ID: " << g_last_script_id << ")\n";
            }
        } catch (...) {
            std::cout << "[CDP] Script registered (ID unknown)\n";
        }
        
        // Also evaluate immediately on current page
        json eval_params = {
            {"expression", jsCode},
            {"userGesture", true},
            {"awaitPromise", false}
        };
        
        std::string eval_response;
        g_cdp_connection->SendCommand("Runtime.evaluate", eval_params, eval_response);
        
        std::cout << "[CDP] Script will auto-inject on page load/navigation\n";
        return true;
        
    } catch (std::exception const &e) {
        std::cerr << "[CDP] Error: " << e.what() << "\n";
        return false;
    }
}
