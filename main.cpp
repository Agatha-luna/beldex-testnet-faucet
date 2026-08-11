#include "crow.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include "faucet-helpers.h"

using ReturnType = std::tuple<crow::json::wvalue, bool, int>;
using RpcReturnType = std::tuple<crow::json::wvalue, int>;

namespace nl = nlohmann;

// Loads KEY=VALUE pairs from a .env file into the process environment.
// Existing environment variables are not overwritten.
void loadDotenv(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        setenv(key.c_str(), value.c_str(), 0);
    }
}


struct CORS {
    struct context {};

    void before_handle(crow::request& req, crow::response& res, context&) {
        if (req.method == "OPTIONS"_method) {
            res.code = 204;
            setCorsHeaders(res);
            res.end();
        }
    }

    void after_handle(crow::request&, crow::response& res, context&, crow::detail::context<CORS>&) {
        setCorsHeaders(res);
    }

    void setCorsHeaders(crow::response& res) {
        res.add_header("Access-Control-Allow-Origin", "*");
        res.add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.add_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    }
};

  
int main() {

    loadDotenv();

    try {
        crow::App<CORS> app;

        CROW_ROUTE(app, "/transfer").methods("POST"_method)([](const crow::request& req){
            crow::json::wvalue res;

            try {
                faucetHelper helper;
                crow::json::rvalue body = crow::json::load(req.body);
                if (!body)
                    return crow::response(400, "Invalid JSON");

                std::string tnAddr = body["address"].s();

                // Get IP
                std::string clientIP = helper.getClientIP(req);
                std::cout << "User IP : " << clientIP << std::endl;

                
                // validate client testnet address
                bool addressValid = helper.validateTestnetAddress(tnAddr);
                std::cout << "Address valid : " << addressValid << std::endl;

                if (addressValid) {
                    // Check IP restrict
                    ReturnType result = helper.isIpRestrict(clientIP);

                    auto [response, is_restricted, statuscode] = result;
                    std::cout << "IP Statuscode : " << statuscode << std::endl;
                    std::cout << "IP Restricted : " << is_restricted << std::endl;


                    if (!is_restricted) {
                        // Transfer faucet
                        RpcReturnType rpcResult = helper.transferRequest(tnAddr, clientIP);
                        auto [rpcResponse, rpcStatuscode] = rpcResult;

                        return crow::response(rpcStatuscode, rpcResponse);
                    } else {
                        return crow::response(statuscode, response);
                    }
                    
                } else {
                    res["error"] = "The address provided is invalid. Kindly ensure that you enter a valid testnet address and try again.";
                    res["status"] = false;
                    return crow::response(400, res);
                }

            } catch (const std::exception& e) {
                faucetHelper::logger << "[EXCEPTION] Exception in /transfer handler: " << e.what() << std::endl;
                res["tx-error"] = "Something went wrong.";
                res["status"] = false;
                return crow::response(500, res);
            
            } catch (...) {
                faucetHelper::logger << "[EXCEPTION] Unknown exception in /transfer handler." << std::endl;
                res["tx-error"] = "Something went wrong.";
                res["status"] = false;
                return crow::response(500, res);
            }
        });

        app.port(5000).multithreaded().run();
    }
    catch (const std::exception& e) {
        faucetHelper::logger << "[EXCEPTION] Exception in main: " << e.what() << std::endl;
    }


    return 0;
}
