#include "faucet-helpers.h"
#include "crow.h"
#include <iostream>
#include <sqlite3.h>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <fmt/core.h>

using ReturnType = std::tuple<crow::json::wvalue, bool, int>;
using RpcReturnType = std::tuple<crow::json::wvalue, int>;
namespace nl = nlohmann;

std::ofstream faucetHelper::logger("beldex-faucet.log", std::ios::app);

faucetHelper::faucetHelper() {
    try {
        const char* amountEnv = std::getenv("FAUCET_AMOUNT");
        if (!amountEnv) {
            logger << "[ERROR] FAUCET_AMOUNT environment variable is not set. Set it in your .env file." << std::endl;
        }
        AMOUNT = amountEnv ? std::stoll(amountEnv) : 0;

        const char* dbEnv = std::getenv("FAUCET_DATABASE");
        if (!dbEnv) {
            logger << "[ERROR] FAUCET_DATABASE environment variable is not set. Set it in your .env file." << std::endl;
        }
        DATABASE = dbEnv ? dbEnv : "";

        const char* walletUrlEnv = std::getenv("WALLET_URL");
        if (!walletUrlEnv) {
            logger << "[ERROR] WALLET_URL environment variable is not set. Set it in your .env file." << std::endl;
        }
        WALLET_URL = walletUrlEnv ? walletUrlEnv : "";

        if (!logger.is_open()) {
            std::cerr << "[ERROR] Cannot open log file." << std::endl;
        }

        int rc = sqlite3_open(DATABASE, &db);
        if (rc) {
            logger << "[ERROR] Failed to open database: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_close(db);
        }

        char* tableerr = nullptr;
        // Create user table
        const char* create_table = "CREATE TABLE IF NOT EXISTS users ("
                                   "Tx_Id INTEGER PRIMARY KEY, "
                                   "Tx_Address TEXT, "
                                   "IP TEXT, "
                                   "Tx_Amount INTEGER, "
                                   "Timestamp DATETIME );";
        
        if (sqlite3_exec(db, create_table, nullptr, nullptr, &tableerr) != SQLITE_OK) {
            logger << "[ERROR] Error creating table: "<< tableerr <<  std::endl;
            sqlite3_free(tableerr);
        }

    }
    catch (const std::exception& e) {
        logger << "[EXCEPTION] " << e.what() << std::endl;
    }
}

faucetHelper::~faucetHelper() {
    sqlite3_close(db);
}


// Validate client testnet address
bool faucetHelper::validateTestnetAddress(std::string tnAddr) {
    try {
        std::string payload = fmt::format(
            R"({{
                "jsonrpc":"2.0",
                "id":"0",
                "method":"validate_address",
                "params":{{
                    "address":"{}",
                    "any_net_type":true
                }}
            }})", 
            tnAddr
        );

        std::cout << "Wallet Url : " << WALLET_URL << std::endl;
        cpr::Header headers = cpr::Header{std::make_pair("Content-Type", "application/json")};
        cpr::Response res = cpr::Post(cpr::Url{WALLET_URL}, headers, cpr::Body{payload});

        std::cout << "Status Code: " << res.status_code << std::endl;
        std::cout << "Response Text: " << res.text << std::endl;
        std::cout << "Headers:" << std::endl;

        for (const auto& header : res.header) {
            std::cout << "  " << header.first << ": " << header.second << std::endl;
        }

        if (res.status_code == 200) {
            try {
                nl::json parsed = nl::json::parse(res.text);

                bool is_valid = parsed["result"]["valid"];
                std::string nettype = parsed["result"]["nettype"];
                std::cout << "Valid : " << is_valid << std::endl;
                std::cout << "Nettype : " << nettype << std::endl;

                if (is_valid && nettype == "testnet") {
                    logger << "[INFO] Given testnet address valid." << std::endl;
                    return true;
                } else {
                    logger << "[INFO] Given testnet address not valid." << std::endl;
                    return false;
                }
            } catch (const std::exception& e) {
                logger << "[ERROR] JSON parse or access error: " << e.what() << std::endl;
                return false;
            }
        } else {
            logger << "[ERROR] Wallet gave invalid response for validating address. Status code: " << res.status_code << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        logger << "[EXCEPTION] Unexpected error in validateTestnetAddress: " << e.what() << std::endl;
        return false;
    }
}


// Client IP
std::string faucetHelper::getClientIP(const crow::request& req) {
    try {
        std::string clientIP = req.get_header_value("X-Forwarded-For");

        if (clientIP.empty()) {
            clientIP = req.remote_ip_address;
        }

        return clientIP;
    } catch (const std::exception& e) {
        logger << "[ERROR] Exception while getting client IP: " << e.what() << std::endl;
        return "";
    }
}


// Get current Timestamp
std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}


// Parse Timestamp
std::chrono::system_clock::time_point parseTimestamp(const std::string& timestamp) {
    std::tm tm = {};
    std::istringstream ss(timestamp);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}


// IP Restrict
ReturnType faucetHelper::isIpRestrict(std::string clientIP) {
    crow::json::wvalue res;
    try {
        sqlite3_stmt* stmt;
        const char* sql = "SELECT TimeStamp FROM users WHERE IP = ? ORDER BY Tx_Id DESC LIMIT 1;";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            logger << "[ERROR] Failed to prepare SQL for IP restrict" << std::endl;
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, false, 500};
        }

        sqlite3_bind_text(stmt, 1, clientIP.c_str(), -1, SQLITE_STATIC);

        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            const unsigned char* ts_char = sqlite3_column_text(stmt, 0);
            std::string last_access_ts = ts_char ? reinterpret_cast<const char*>(ts_char) : "";
            logger << "[DEBUG] ts_char is: " << (ts_char ? reinterpret_cast<const char*>(ts_char) : "NULL") << std::endl;

            sqlite3_finalize(stmt);

            if (!last_access_ts.empty()) {
                try {
                    std::time_t last_time = std::stoll(last_access_ts);
                    auto last_dt = std::chrono::system_clock::from_time_t(last_time);
                    auto current_dt = std::chrono::system_clock::now();
                    auto diff = current_dt - last_dt;
                    auto hours = std::chrono::duration_cast<std::chrono::hours>(diff).count();

                    if (hours >= 24) {
                        res["message"] = "No restrict";
                        return {res, false, 200};
                    } else {
                        auto seconds_left = 86400 - std::chrono::duration_cast<std::chrono::seconds>(diff).count();
                        int h = seconds_left / 3600;
                        int m = (seconds_left % 3600) / 60;
                        int s = seconds_left % 60;

                        std::stringstream msg;
                        msg << "Access is temporarily restricted. Kindly try again in " << h << " hour(s) and " << m << " minute(s) and " << s << " seconds(s).";
                        res["error"] = msg.str();
                        res["status"] = false;
                        return {res, true, 429};
                    }
                } catch (const std::exception& e) {
                    logger << "[EXCEPTION] Unexpected error in IP Restrict: " << e.what() << std::endl;
                    res["tx-error"] = "Something went wrong.";
                    res["status"] = false;
                    return {res, true, 500};
                }
            } else {
                res["message"] = "No restrict";
                return {res, false, 200};
            }
        } else {
            sqlite3_finalize(stmt);
            res["message"] = "No restrict";
            return {res, false, 200};
        }
    } catch (const std::exception& e) {
        logger << "[ERROR] Exception while checking IP restriction: " << e.what() << std::endl;
        res["tx-error"] = "Something went wrong.";
        res["status"] = false;
        return {res, true, 500};
    }
}


// Address Restrict
ReturnType faucetHelper::isAddressRestrict(std::string tnAddr) {
    crow::json::wvalue res;
    try {
        sqlite3_stmt* stmt;
        const char* sql = "SELECT TimeStamp FROM users WHERE Tx_Address = ? ORDER BY Tx_Id DESC LIMIT 1";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            faucetHelper::logger << "[ERROR] Failed to prepare SQL for Address Restriction" << std::endl;
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
        }

        sqlite3_bind_text(stmt, 1, tnAddr.c_str(), -1, SQLITE_STATIC);

        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            const unsigned char* ts_char = sqlite3_column_text(stmt, 0);
            std::string last_access_ts = ts_char ? reinterpret_cast<const char*>(ts_char) : "";
            logger << "[DEBUG] ts_char is: " << (ts_char ? reinterpret_cast<const char*>(ts_char) : "NULL") << std::endl;

            sqlite3_finalize(stmt);

            if (!last_access_ts.empty()) {
                try {
                    std::time_t last_time = std::stoll(last_access_ts);
                    auto last_dt = std::chrono::system_clock::from_time_t(last_time);
                    auto current_dt = std::chrono::system_clock::now();
                    auto diff = current_dt - last_dt;
                    auto hours = std::chrono::duration_cast<std::chrono::hours>(diff).count();

                    if (hours >= 24) {
                        res["message"] = "No restrict";
                        return {res, false, 0};
                    } else {
                        auto seconds_left = 86400 - std::chrono::duration_cast<std::chrono::seconds>(diff).count();
                        int h = seconds_left / 3600;
                        int m = (seconds_left % 3600) / 60;
                        int s = seconds_left % 60;

                        std::stringstream msg;
                        msg << "Access is temporarily restricted. Kindly try again in "
                            << h << " hour(s) and " << m << " minute(s) and " << s << " seconds(s).";
                        res["error"] = msg.str();
                        res["status"] = false;
                        return {res, true, 429};
                    }
                } catch (const std::exception& e) {
                    logger << "[EXCEPTION] Unexpected error in Address Restrict: " << e.what() << std::endl;
                    res["tx-error"] = "Something went wrong.";
                    res["status"] = false;
                    return {res, true, 500};
                }
            } else {
                res["message"] = "No restrict";
                return {res, false, 0};
            }
        } else {
            sqlite3_finalize(stmt);
            res["message"] = "No restrict";
            return {res, false, 0};
        }
    } catch (const std::exception& e) {
        logger << "[ERROR] Exception while checking address restriction: " << e.what() << std::endl;
        res["tx-error"] = "Something went wrong.";
        res["status"] = false;
        return {res, true, 500};
    }
}


// Faucet transfer
RpcReturnType faucetHelper::transferRequest(std::string tnAddr, std::string clientIP) {
    crow::json::wvalue transRes;

    try {
        std::string payload = fmt::format(R"({{
            "jsonrpc": "2.0",
            "id": "0",
            "method": "transfer",
            "params": {{
                "destinations": [
                    {{
                        "amount": {},
                        "address": "{}"
                    }}
                ],
                "account_index": 0,
                "priority": 0,
                "get_tx_key": true
            }}
        }})", AMOUNT, tnAddr);


        ReturnType result = faucetHelper::isAddressRestrict(tnAddr);

        auto [response, is_restricted, statuscode] = result;

        if (!is_restricted) {
            for (int attempt = 0; attempt < 6; ++attempt) {
                try {
                    std::cout << "Wallet Url : " << WALLET_URL << std::endl;
                    cpr::Header headers = cpr::Header{std::make_pair("Content-Type", "application/json")};
                    cpr::Response res = cpr::Post(cpr::Url{WALLET_URL}, headers, cpr::Body{payload});

                    if (res.error) {
                        logger << "[ERROR] HTTP request failed While Transfer: " << res.error.message << std::endl;
                        transRes["error"] = "Something went wrong.";
                        transRes["status"] = false;
                        return {transRes, 500};
                    }

                    std::cout << "Status Code: " << res.status_code << std::endl;
                    std::cout << "Response Text: " << res.text << std::endl;
                    std::cout << "Headers:" << std::endl;

                    for (const auto& header : res.header) {
                        std::cout << "  " << header.first << ": " << header.second << std::endl;
                    }

                    nl::json rpc_result = nl::json::parse(res.text);

                    if (rpc_result.contains("error")) {
                        int error_code = rpc_result["error"].value("code", -1);

                        logger << "[RPC ERROR] Code: " << error_code << ", Message: " << rpc_result["error"].dump() << std::endl;

                        if (error_code == -37) {
                            transRes["message"] = "Transaction failed.";
                            transRes["status"] = false;
                            return {transRes, 500};
                        }

                        std::this_thread::sleep_for(std::chrono::seconds(10));
                        continue;
                    }

                    std::string tx_hash = rpc_result["result"]["tx_hash"];
                    std::string timestamp = std::to_string(std::time(nullptr));

                    const char* insert_user = "INSERT INTO users (Tx_Address, Tx_Amount,IP, Timestamp) VALUES (?, ?, ?, ?);";
                    sqlite3_stmt* stmt;

                    if (sqlite3_prepare_v2(db, insert_user, -1, &stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(stmt, 1, tnAddr.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_int64(stmt, 2, AMOUNT / 1000000000);
                        sqlite3_bind_text(stmt, 3, clientIP.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_text(stmt, 4, timestamp.c_str(), -1, SQLITE_STATIC);
                        sqlite3_step(stmt);
                    } else {
                        logger << "[ERROR] Failed to prepare insert statement for users: " << sqlite3_errmsg(db) << std::endl;
                    }

                    sqlite3_finalize(stmt);

                    transRes["tx_hash"] = tx_hash;
                    transRes["amount"] = AMOUNT / 1000000000;
                    transRes["status"] = true;

                    return {transRes, 200};
                } catch (const std::exception& e) {
                    logger << "[EXCEPTION] Exeception Occur While Transfer: " << e.what() << std::endl;
                }

                if (attempt < 5) {
                    std::this_thread::sleep_for(std::chrono::seconds(10));
                } else {
                    transRes["message"] = "Transaction failed.";
                    transRes["status"] = false;
                    return {transRes, 500};
                }
            }
        } else {
            return {response, statuscode};
        }

    }catch (const std::exception& e) {
        logger << "[ERROR] Exception while Transfer: " << e.what() << std::endl;
        transRes["message"] = "Transaction failed.";
        transRes["status"] = false;
        return {transRes, 500};
    }
    transRes["message"] = "Transaction failed.";
    transRes["status"] = false;
    return {transRes, 500};

}