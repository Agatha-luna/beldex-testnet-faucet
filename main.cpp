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

        // Mint a short-lived, address-bound challenge for the client to sign
        // with its wallet (e.g. bdx_signMessage) before /transfer will accept it.
        CROW_ROUTE(app, "/challenge").methods("GET"_method)([](const crow::request& req){
            crow::json::wvalue res;

            try {
                faucetHelper helper;
                char* address = req.url_params.get("address");
                if (!address || std::string(address).empty()) {
                    res["error"] = "Query parameter \"address\" is required.";
                    res["status"] = false;
                    return crow::response(400, res);
                }

                res["challenge"] = helper.makeChallenge(address);
                res["status"] = true;
                return crow::response(200, res);

            } catch (const std::exception& e) {
                faucetHelper::logger << "[EXCEPTION] Exception in /challenge handler: " << e.what() << std::endl;
                res["tx-error"] = "Something went wrong.";
                res["status"] = false;
                return crow::response(500, res);
            }
        });

        CROW_ROUTE(app, "/transfer").methods("POST"_method)([](const crow::request& req){
            crow::json::wvalue res;

            try {
                crow::json::rvalue body = crow::json::load(req.body);
                if (!body || !body.has("address") || body["address"].t() != crow::json::type::String ||
                    !body.has("captcha_token") || body["captcha_token"].t() != crow::json::type::String) {
                    res["status"] = false;
                    res["error"] = "Provide an address and CAPTCHA token.";
                    return crow::response(400, res);
                }

                auto [captchaResponse, captchaStatus] = faucetHelper::verifyCaptcha(body["captcha_token"].s());
                if (captchaStatus != 200) {
                    return crow::response(captchaStatus, captchaResponse);
                }
                faucetHelper helper;

                std::string tnAddr = body["address"].s();
                if (tnAddr.empty()) {
                    res["error"] = "A non-empty address string is required.";
                    res["status"] = false;
                    return crow::response(400, res);
                }

                if (!body.has("challenge") || !body.has("signature")) {
                    res["error"] = "Wallet signature is required. Please connect your wallet and try again.";
                    res["status"] = false;
                    return crow::response(400, res);
                }
                std::string challenge = body["challenge"].s();
                std::string signature = body["signature"].s();

                // Get IP
                std::string clientIP = helper.getClientIP(req);
                std::cout << "User IP : " << clientIP << std::endl;

                if (clientIP.empty()) {
                    res["tx-error"] = "Unable to identify the requesting client.";
                    res["status"] = false;
                    return crow::response(500, res);
                }

                // Fast pre-check, before spending effort on address validation
                // or a signature-verify RPC call. transferRequest repeats this
                // check atomically inside the transaction that acquires the
                // IP and address locks.
                ReturnType ipResult = helper.isIpRestrict(clientIP);
                auto [ipResponse, isIpRestricted, ipStatusCode] = ipResult;
                std::cout << "IP Restricted : " << isIpRestricted << std::endl;

                if (isIpRestricted) {
                    return crow::response(ipStatusCode, ipResponse);
                }

                // validate client testnet address
                bool addressValid = helper.validateTestnetAddress(tnAddr);
                std::cout << "Address valid : " << addressValid << std::endl;

                if (!addressValid) {
                    res["error"] = "The address provided is invalid. Kindly ensure that you enter a valid testnet address and try again.";
                    res["status"] = false;
                    return crow::response(400, res);
                }

                // The challenge must be one we actually minted for this address,
                // and not expired.
                std::string challengeError;
                if (!helper.checkChallenge(challenge, tnAddr, challengeError)) {
                    res["error"] = challengeError;
                    res["status"] = false;
                    return crow::response(400, res);
                }

                // Prove the requester actually controls tnAddr's spend key —
                // closes the "spray addresses nobody owns across rotating IPs"
                // hole that the IP/address rate limits alone don't catch.
                if (!helper.verifySignature(tnAddr, challenge, signature)) {
                    res["error"] = "Could not verify wallet ownership. Please sign the message with the connected wallet and try again.";
                    res["status"] = false;
                    return crow::response(401, res);
                }

                // Transfer faucet. isIpRestrict/isAddressRestrict are re-checked
                // atomically under lock inside here before anything is sent.
                RpcReturnType rpcResult = helper.transferRequest(tnAddr, clientIP);
                auto [rpcResponse, rpcStatuscode] = rpcResult;
                return crow::response(rpcStatuscode, rpcResponse);

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

        app.bindaddr("127.0.0.1").port(5000).multithreaded().run();
    }
    catch (const std::exception& e) {
        faucetHelper::logger << "[EXCEPTION] Exception in main: " << e.what() << std::endl;
    }


    return 0;
}
