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
        
    public:
        faucetHelper();
        ~faucetHelper();

        static std::ofstream logger;
        bool validateTestnetAddress(std::string tnAddr);
        std::string getClientIP(const crow::request& req);
        ReturnType isIpRestrict(std::string clientIP);
        ReturnType isAddressRestrict(std::string tnAddr);
        RpcReturnType transferRequest(std::string tnAddr, std::string clientIP);
        
};
