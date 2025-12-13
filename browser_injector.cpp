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
    std::atomic<int> message_id{1};
    std::string ws_url;
    
    bool EstablishConnection() {
        try {
            const char *host = "127.0.0.1";
            const char *port = "27245";
            
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
                std::cerr << "No targets found\n";
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
                std::cerr << "No page target with webSocketDebuggerUrl\n";
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
        std::cout << "[CDP] Establishing new connection...\n";
        return EstablishConnection();
    }
    
    bool SendCommand(const std::string& method, const json& params, std::string& response) {
        std::lock_guard<std::mutex> lock(ws_mutex);
        
        if (!connected || !ws) {
            std::cerr << "[CDP] Not connected\n";
            return false;
        }
        
        try {
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
            return false;
        }
    }
    
    void Disconnect() {
        std::lock_guard<std::mutex> lock(ws_mutex);
        
        if (ws && connected) {
            try {
                ws->close(websocket::close_code::normal);
            } catch (...) {}
        }
        
        ws.reset();
        connected = false;
        std::cout << "[CDP] Connection closed\n";
    }
    
    ~CDPConnection() {
        Disconnect();
    }
};

// Global persistent connection
static std::unique_ptr<CDPConnection> g_cdp_connection;
static std::mutex g_cdp_mutex;

extern "C" __declspec(dllexport) bool InitializeCDP() {
    std::lock_guard<std::mutex> lock(g_cdp_mutex);
    
    if (!g_cdp_connection) {
        g_cdp_connection = std::make_unique<CDPConnection>();
    }
    
    return g_cdp_connection->EnsureConnected();
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
    
    if (!g_cdp_connection->EnsureConnected()) {
        std::cerr << "[CDP] Failed to establish connection\n";
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
        
        // Send Runtime.evaluate command
        json params = {
            {"expression", jsCode},
            {"userGesture", true},
            {"awaitPromise", false}
        };
        
        std::string response;
        if (!g_cdp_connection->SendCommand("Runtime.evaluate", params, response)) {
            std::cerr << "[CDP] Failed to send command\n";
            return false;
        }
        
        std::cout << "[CDP] Response: " << response << "\n";
        return true;
        
    } catch (std::exception const &e) {
        std::cerr << "[CDP] Error: " << e.what() << "\n";
        return false;
    }
}
