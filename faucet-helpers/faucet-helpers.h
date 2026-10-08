#pragma once

#include "crow.h"
#include <sqlite3.h>
#include <fstream>
#include <fmt/core.h>
#include <tuple>
#include <optional>


using ReturnType = std::tuple<crow::json::wvalue, bool, int>;
using RpcReturnType = std::tuple<crow::json::wvalue, int>;

class faucetHelper {
    private:
        sqlite3* db = nullptr;
        int64_t AMOUNT = 0;
        const char* DATABASE = nullptr;
        std::string WALLET_URL;
        std::string SIGN_SECRET;

    public:
        faucetHelper();
        ~faucetHelper();

        static std::ofstream logger;
        bool validateTestnetAddress(std::string tnAddr);
        std::string getClientIP(const crow::request& req);
        ReturnType isIpRestrict(std::string clientIP);
        ReturnType isAddressRestrict(std::string tnAddr);
        RpcReturnType transferRequest(std::string tnAddr, std::string clientIP);

        // Proof-of-ownership: the client must sign a server-issued, address-bound
        // challenge with the requesting wallet before any funds are considered.
        // This doesn't stop someone willing to script real wallet creation, but it
        // closes the much cheaper hole of spraying addresses nobody actually holds
        // the keys for (e.g. harvested from the explorer) across rotating IPs.
        std::string makeChallenge(const std::string& address);
        bool checkChallenge(const std::string& challenge, const std::string& address, std::string& error);
        bool verifySignature(const std::string& address, const std::string& data, const std::string& signature);

};
