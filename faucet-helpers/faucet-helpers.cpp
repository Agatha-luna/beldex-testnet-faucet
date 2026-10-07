#include "faucet-helpers.h"
#include "crow.h"
#include <iostream>
#include <sqlite3.h>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <fmt/core.h>

using ReturnType = std::tuple<crow::json::wvalue, bool, int>;
using RpcReturnType = std::tuple<crow::json::wvalue, int>;
namespace nl = nlohmann;

namespace {

std::optional<std::string> ipv4SubnetPattern(const std::string& ip) {
    std::size_t start = 0;
    std::size_t third_dot = std::string::npos;

    for (int octet = 0; octet < 4; ++octet) {
        const std::size_t end = ip.find('.', start);
        if ((octet < 3 && end == std::string::npos) ||
            (octet == 3 && end != std::string::npos)) {
            return std::nullopt;
        }

        const std::size_t part_end = end == std::string::npos ? ip.size() : end;
        if (part_end == start || part_end - start > 3) {
            return std::nullopt;
        }

        int value = 0;
        for (std::size_t i = start; i < part_end; ++i) {
            if (ip[i] < '0' || ip[i] > '9') {
                return std::nullopt;
            }
            value = value * 10 + (ip[i] - '0');
        }
        if (value > 255) {
            return std::nullopt;
        }

        if (octet == 2) {
            third_dot = end;
        }
        start = part_end + 1;
    }

    return ip.substr(0, third_dot + 1) + "%";
}

} // namespace

std::ofstream faucetHelper::logger("beldex-faucet.log", std::ios::app);

faucetHelper::faucetHelper() {
    try {
        const char* amountEnv = std::getenv("FAUCET_AMOUNT");
        if (!amountEnv) {
            logger << "[ERROR] FAUCET_AMOUNT environment variable is not set. Set it in your .env file." << std::endl;
            throw std::runtime_error("FAUCET_AMOUNT is not configured");
        }
        AMOUNT = std::stoll(amountEnv);

        const char* dbEnv = std::getenv("FAUCET_DATABASE");
        if (!dbEnv) {
            logger << "[ERROR] FAUCET_DATABASE environment variable is not set. Set it in your .env file." << std::endl;
            throw std::runtime_error("FAUCET_DATABASE is not configured");
        }
        DATABASE = dbEnv;

        const char* walletUrlEnv = std::getenv("WALLET_URL");
        if (!walletUrlEnv) {
            logger << "[ERROR] WALLET_URL environment variable is not set. Set it in your .env file." << std::endl;
            throw std::runtime_error("WALLET_URL is not configured");
        }
        WALLET_URL = walletUrlEnv;

        if (!logger.is_open()) {
            std::cerr << "[ERROR] Cannot open log file." << std::endl;
        }

        const int rc = sqlite3_open(DATABASE, &db);
        if (rc != SQLITE_OK) {
            logger << "[ERROR] Failed to open database: " << sqlite3_errmsg(db) << std::endl;
            sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error("Failed to open faucet database");
        }

        sqlite3_busy_timeout(db, 5000);

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
            throw std::runtime_error("Failed to create users table");
        }

        const char* create_address_index =
            "CREATE INDEX IF NOT EXISTS idx_users_address_tx "
            "ON users (Tx_Address, Tx_Id DESC);";
        if (sqlite3_exec(db, create_address_index, nullptr, nullptr, &tableerr) != SQLITE_OK) {
            logger << "[ERROR] Error creating address index: " << tableerr << std::endl;
            sqlite3_free(tableerr);
            throw std::runtime_error("Failed to create address restriction index");
        }

        const char* create_ip_index =
            "CREATE INDEX IF NOT EXISTS idx_users_ip_tx "
            "ON users (IP, Tx_Id DESC);";
        if (sqlite3_exec(db, create_ip_index, nullptr, nullptr, &tableerr) != SQLITE_OK) {
            logger << "[ERROR] Error creating IP index: " << tableerr << std::endl;
            sqlite3_free(tableerr);
            throw std::runtime_error("Failed to create IP restriction index");
        }

    }
    catch (const std::exception& e) {
        logger << "[EXCEPTION] " << e.what() << std::endl;
        if (db != nullptr) {
            sqlite3_close(db);
            db = nullptr;
        }
        throw;
    }
}

faucetHelper::~faucetHelper() {
    if (db != nullptr) {
        sqlite3_close(db);
    }
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
                bool isSubaddress = parsed["result"]["subaddress"];
                std::cout << "Valid : " << is_valid << std::endl;
                std::cout << "Nettype : " << nettype << std::endl;

                if (is_valid && nettype == "testnet" && !isSubaddress) {
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
        auto trim = [](std::string value) {
            const auto first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                return std::string{};
            }

            const auto last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        };

        // Cloudflare sets this header to the original visitor's IP address.
        std::string clientIP = trim(req.get_header_value("CF-Connecting-IP"));

        if (clientIP.empty()) {
            // X-Forwarded-For is a comma-separated chain. The first entry is
            // the original client when the header is set by a trusted proxy.
            clientIP = req.get_header_value("X-Forwarded-For");
            const auto comma = clientIP.find(',');
            if (comma != std::string::npos) {
                clientIP = clientIP.substr(0, comma);
            }
            clientIP = trim(clientIP);
        }

        if (clientIP.empty()) {
            clientIP = trim(req.remote_ip_address);
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
        const auto subnet_pattern = ipv4SubnetPattern(clientIP);
        const std::string lookup_value = subnet_pattern.value_or(clientIP);
        const char* sql = subnet_pattern
            ? "SELECT TimeStamp FROM users WHERE IP LIKE ? ORDER BY Tx_Id DESC LIMIT 1;"
            : "SELECT TimeStamp FROM users WHERE IP = ? ORDER BY Tx_Id DESC LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            logger << "[ERROR] Failed to prepare SQL for IP restriction: "
                   << sqlite3_errmsg(db) << std::endl;
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
        }

        if (sqlite3_bind_text(stmt, 1, lookup_value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            logger << "[ERROR] Failed to bind IP restriction query: "
                   << sqlite3_errmsg(db) << std::endl;
            sqlite3_finalize(stmt);
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
        }

        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            res["message"] = "No restrict";
            return {res, false, 200};
        }

        if (rc != SQLITE_ROW) {
            logger << "[ERROR] Failed to execute IP restriction query: "
                   << sqlite3_errmsg(db) << std::endl;
            sqlite3_finalize(stmt);
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
        }

        const unsigned char* ts_char = sqlite3_column_text(stmt, 0);
        const std::string last_access_ts =
            ts_char ? reinterpret_cast<const char*>(ts_char) : "";
        sqlite3_finalize(stmt);

        if (last_access_ts.empty()) {
            logger << "[ERROR] IP restriction row has no timestamp." << std::endl;
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
        }

        try {
            std::size_t parsed_length = 0;
            const std::time_t last_time = std::stoll(last_access_ts, &parsed_length);
            if (parsed_length != last_access_ts.size()) {
                throw std::invalid_argument("timestamp contains non-numeric characters");
            }

            const auto last_dt = std::chrono::system_clock::from_time_t(last_time);
            const auto current_dt = std::chrono::system_clock::now();
            const auto elapsed_seconds =
                std::chrono::duration_cast<std::chrono::seconds>(current_dt - last_dt).count();

            constexpr long long restriction_seconds = 24LL * 60 * 60;
            if (elapsed_seconds >= restriction_seconds) {
                res["message"] = "No restrict";
                return {res, false, 200};
            }

            const long long effective_elapsed = elapsed_seconds < 0 ? 0 : elapsed_seconds;
            const long long seconds_left = restriction_seconds - effective_elapsed;
            const long long h = seconds_left / 3600;
            const long long m = (seconds_left % 3600) / 60;
            const long long s = seconds_left % 60;

            std::stringstream msg;
            msg << "Access is temporarily restricted. Kindly try again in "
                << h << " hour(s) and " << m << " minute(s) and " << s << " second(s).";
            res["error"] = msg.str();
            res["status"] = false;
            return {res, true, 429};
        } catch (const std::exception& e) {
            logger << "[EXCEPTION] Unexpected error in IP Restrict: " << e.what() << std::endl;
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
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
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT TimeStamp FROM users WHERE Tx_Address = ? ORDER BY Tx_Id DESC LIMIT 1;";

        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            logger << "[ERROR] Failed to prepare SQL for Address Restriction: "
                   << sqlite3_errmsg(db) << std::endl;
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
        }

        if (sqlite3_bind_text(stmt, 1, tnAddr.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            logger << "[ERROR] Failed to bind address restriction query: "
                   << sqlite3_errmsg(db) << std::endl;
            sqlite3_finalize(stmt);
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
        }

        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            res["message"] = "No restrict";
            return {res, false, 200};
        }

        if (rc != SQLITE_ROW) {
            logger << "[ERROR] Failed to execute address restriction query: "
                   << sqlite3_errmsg(db) << std::endl;
            sqlite3_finalize(stmt);
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
        }

        const unsigned char* ts_char = sqlite3_column_text(stmt, 0);
        const std::string last_access_ts =
            ts_char ? reinterpret_cast<const char*>(ts_char) : "";
        sqlite3_finalize(stmt);

        if (last_access_ts.empty()) {
            logger << "[ERROR] Address restriction row has no timestamp." << std::endl;
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
        }

        try {
            std::size_t parsed_length = 0;
            const std::time_t last_time = std::stoll(last_access_ts, &parsed_length);
            if (parsed_length != last_access_ts.size()) {
                throw std::invalid_argument("timestamp contains non-numeric characters");
            }

            const auto last_dt = std::chrono::system_clock::from_time_t(last_time);
            const auto current_dt = std::chrono::system_clock::now();
            const auto elapsed_seconds =
                std::chrono::duration_cast<std::chrono::seconds>(current_dt - last_dt).count();

            constexpr long long restriction_seconds = 24LL * 60 * 60;
            if (elapsed_seconds >= restriction_seconds) {
                res["message"] = "No restrict";
                return {res, false, 200};
            }

            // If the system clock moved backwards, enforce a fresh 24-hour wait
            // instead of displaying a duration longer than the configured limit.
            const long long effective_elapsed = elapsed_seconds < 0 ? 0 : elapsed_seconds;
            const long long seconds_left = restriction_seconds - effective_elapsed;
            const long long h = seconds_left / 3600;
            const long long m = (seconds_left % 3600) / 60;
            const long long s = seconds_left % 60;

            std::stringstream msg;
            msg << "Access is temporarily restricted. Kindly try again in "
                << h << " hour(s) and " << m << " minute(s) and " << s << " second(s).";
            res["error"] = msg.str();
            res["status"] = false;
            return {res, true, 429};
        } catch (const std::exception& e) {
            logger << "[EXCEPTION] Unexpected error in Address Restrict: " << e.what() << std::endl;
            res["tx-error"] = "Something went wrong.";
            res["status"] = false;
            return {res, true, 500};
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


        char* transaction_error = nullptr;
        if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, &transaction_error) != SQLITE_OK) {
            logger << "[ERROR] Failed to begin address reservation transaction: "
                   << (transaction_error ? transaction_error : sqlite3_errmsg(db)) << std::endl;
            sqlite3_free(transaction_error);
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        auto rollback_reservation = [this]() {
            char* rollback_error = nullptr;
            if (sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &rollback_error) != SQLITE_OK) {
                logger << "[ERROR] Failed to roll back address reservation: "
                       << (rollback_error ? rollback_error : sqlite3_errmsg(db)) << std::endl;
                sqlite3_free(rollback_error);
            }
        };

        ReturnType result = faucetHelper::isAddressRestrict(tnAddr);
        auto [response, is_restricted, statuscode] = result;

        if (is_restricted) {
            rollback_reservation();
            return {response, statuscode};
        }

        // Insert the restriction before sending funds. BEGIN IMMEDIATE makes
        // the check-and-insert atomic across threads and service processes.
        const std::string timestamp = std::to_string(std::time(nullptr));
        const char* insert_user =
            "INSERT INTO users (Tx_Address, Tx_Amount, IP, Timestamp) VALUES (?, ?, ?, ?);";
        sqlite3_stmt* reservation_stmt = nullptr;

        if (sqlite3_prepare_v2(db, insert_user, -1, &reservation_stmt, nullptr) != SQLITE_OK) {
            logger << "[ERROR] Failed to prepare address reservation: "
                   << sqlite3_errmsg(db) << std::endl;
            rollback_reservation();
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        const bool bindings_ok =
            sqlite3_bind_text(reservation_stmt, 1, tnAddr.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_int64(reservation_stmt, 2, AMOUNT / 1000000000) == SQLITE_OK &&
            sqlite3_bind_text(reservation_stmt, 3, clientIP.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_text(reservation_stmt, 4, timestamp.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;

        const int reservation_rc = bindings_ok ? sqlite3_step(reservation_stmt) : SQLITE_ERROR;
        sqlite3_finalize(reservation_stmt);

        if (!bindings_ok || reservation_rc != SQLITE_DONE) {
            logger << "[ERROR] Failed to save address reservation: "
                   << sqlite3_errmsg(db) << std::endl;
            rollback_reservation();
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        const sqlite3_int64 reservation_id = sqlite3_last_insert_rowid(db);
        transaction_error = nullptr;
        if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &transaction_error) != SQLITE_OK) {
            logger << "[ERROR] Failed to commit address reservation: "
                   << (transaction_error ? transaction_error : sqlite3_errmsg(db)) << std::endl;
            sqlite3_free(transaction_error);
            rollback_reservation();
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        auto release_reservation = [this, reservation_id]() {
            sqlite3_stmt* delete_stmt = nullptr;
            const char* delete_sql = "DELETE FROM users WHERE Tx_Id = ?;";
            if (sqlite3_prepare_v2(db, delete_sql, -1, &delete_stmt, nullptr) != SQLITE_OK) {
                logger << "[ERROR] Failed to prepare reservation cleanup: "
                       << sqlite3_errmsg(db) << std::endl;
                return false;
            }

            const bool bind_ok =
                sqlite3_bind_int64(delete_stmt, 1, reservation_id) == SQLITE_OK;
            const int delete_rc = bind_ok ? sqlite3_step(delete_stmt) : SQLITE_ERROR;
            sqlite3_finalize(delete_stmt);

            if (!bind_ok || delete_rc != SQLITE_DONE) {
                logger << "[ERROR] Failed to clean up address reservation: "
                       << sqlite3_errmsg(db) << std::endl;
                return false;
            }
            return true;
        };

        for (int attempt = 0; attempt < 6; ++attempt) {
            try {
                std::cout << "Wallet Url : " << WALLET_URL << std::endl;
                cpr::Header headers = cpr::Header{std::make_pair("Content-Type", "application/json")};
                cpr::Response res = cpr::Post(cpr::Url{WALLET_URL}, headers, cpr::Body{payload});

                if (res.error) {
                    logger << "[ERROR] HTTP request failed While Transfer: " << res.error.message << std::endl;
                    logger << "[WARN] Address reservation retained because the transfer result is ambiguous." << std::endl;
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
                        release_reservation();
                        transRes["message"] = "Transaction failed.";
                        transRes["status"] = false;
                        return {transRes, 500};
                    }

                    if (attempt < 5) {
                        std::this_thread::sleep_for(std::chrono::seconds(10));
                        continue;
                    }

                    release_reservation();
                    transRes["message"] = "Transaction failed.";
                    transRes["status"] = false;
                    return {transRes, 500};
                }

                std::string tx_hash = rpc_result["result"]["tx_hash"];

                transRes["tx_hash"] = tx_hash;
                transRes["amount"] = AMOUNT / 1000000000;
                transRes["status"] = true;

                return {transRes, 200};
            } catch (const std::exception& e) {
                logger << "[EXCEPTION] Exception while processing transfer response: "
                       << e.what() << std::endl;
                logger << "[WARN] Address reservation retained because the transfer result is ambiguous." << std::endl;
                transRes["message"] = "The transfer result could not be confirmed.";
                transRes["status"] = false;
                return {transRes, 500};
            }
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
