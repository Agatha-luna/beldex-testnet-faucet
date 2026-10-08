#include "faucet-helpers.h"
#include "crow.h"
#include <iostream>
#include <sqlite3.h>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <cstdint>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <fmt/core.h>

using ReturnType = std::tuple<crow::json::wvalue, bool, int>;
using RpcReturnType = std::tuple<crow::json::wvalue, int>;
namespace nl = nlohmann;

namespace {

constexpr std::int32_t wallet_connect_timeout_ms = 5000;
constexpr std::int32_t wallet_request_timeout_ms = 30000;

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

std::string ipRestrictionKey(const std::string& ip) {
    const auto pattern = ipv4SubnetPattern(ip);
    return pattern.value_or(ip);
}

} // namespace

std::ofstream faucetHelper::logger("beldex-faucet.log", std::ios::app);

faucetHelper::faucetHelper() {
    try {
        const char* amountEnv = std::getenv("FAUCET_AMOUNT");
        if (!amountEnv || amountEnv[0] == '\0') {
            logger << "[ERROR] FAUCET_AMOUNT environment variable is not set. Set it in your .env file." << std::endl;
            throw std::runtime_error("FAUCET_AMOUNT is not configured");
        }
        std::size_t amount_length = 0;
        AMOUNT = std::stoll(amountEnv, &amount_length);
        if (amount_length != std::string(amountEnv).size() || AMOUNT <= 0) {
            throw std::runtime_error("FAUCET_AMOUNT must be a positive integer");
        }

        const char* dbEnv = std::getenv("FAUCET_DATABASE");
        if (!dbEnv || dbEnv[0] == '\0') {
            logger << "[ERROR] FAUCET_DATABASE environment variable is not set. Set it in your .env file." << std::endl;
            throw std::runtime_error("FAUCET_DATABASE is not configured");
        }
        DATABASE = dbEnv;

        const char* walletUrlEnv = std::getenv("WALLET_URL");
        if (!walletUrlEnv || walletUrlEnv[0] == '\0') {
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

        // This table prevents concurrent payouts without writing incomplete
        // or failed transfers to the users transaction history.
        const char* create_address_locks =
            "CREATE TABLE IF NOT EXISTS faucet_address_locks ("
            "Tx_Address TEXT PRIMARY KEY, "
            "ReservedAt INTEGER NOT NULL);";
        if (sqlite3_exec(db, create_address_locks, nullptr, nullptr, &tableerr) != SQLITE_OK) {
            logger << "[ERROR] Error creating address lock table: " << tableerr << std::endl;
            sqlite3_free(tableerr);
            throw std::runtime_error("Failed to create address lock table");
        }

        const char* create_ip_locks =
            "CREATE TABLE IF NOT EXISTS faucet_ip_locks ("
            "IP_Key TEXT PRIMARY KEY, "
            "ReservedAt INTEGER NOT NULL);";
        if (sqlite3_exec(db, create_ip_locks, nullptr, nullptr, &tableerr) != SQLITE_OK) {
            logger << "[ERROR] Error creating IP lock table: " << tableerr << std::endl;
            sqlite3_free(tableerr);
            throw std::runtime_error("Failed to create IP lock table");
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
        const nl::json request_payload = {
            {"jsonrpc", "2.0"},
            {"id", "0"},
            {"method", "validate_address"},
            {"params", {
                {"address", tnAddr},
                {"any_net_type", true}
            }}
        };
        const std::string payload = request_payload.dump();

        std::cout << "Wallet Url : " << WALLET_URL << std::endl;
        cpr::Header headers = cpr::Header{std::make_pair("Content-Type", "application/json")};
        cpr::Response res = cpr::Post(
            cpr::Url{WALLET_URL},
            headers,
            cpr::Body{payload},
            cpr::ConnectTimeout{wallet_connect_timeout_ms},
            cpr::Timeout{wallet_request_timeout_ms});

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

        const std::string remoteIP = trim(req.remote_ip_address);
        const bool fromTrustedLocalProxy =
            remoteIP == "127.0.0.1" || remoteIP == "::1";

        // Only trust proxy-provided headers when the direct peer is the local
        // reverse proxy. Otherwise a client could forge its rate-limit key.
        std::string clientIP;
        if (fromTrustedLocalProxy) {
            clientIP = trim(req.get_header_value("CF-Connecting-IP"));
        }

        if (fromTrustedLocalProxy && clientIP.empty()) {
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
            clientIP = remoteIP;
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
        const nl::json request_payload = {
            {"jsonrpc", "2.0"},
            {"id", "0"},
            {"method", "transfer"},
            {"params", {
                {"destinations", {{{"amount", AMOUNT}, {"address", tnAddr}}}},
                {"account_index", 0},
                {"priority", 1},
                {"get_tx_key", true}
            }}
        };
        const std::string payload = request_payload.dump();


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

        ReturnType ip_result = faucetHelper::isIpRestrict(clientIP);
        auto [ip_response, is_ip_restricted, ip_statuscode] = ip_result;
        if (is_ip_restricted) {
            rollback_reservation();
            return {ip_response, ip_statuscode};
        }

        ReturnType address_result = faucetHelper::isAddressRestrict(tnAddr);
        auto [address_response, is_address_restricted, address_statuscode] = address_result;
        if (is_address_restricted) {
            rollback_reservation();
            return {address_response, address_statuscode};
        }

        // Remove a stale lock for this address. A retained lock protects
        // against retrying an ambiguous wallet response for 24 hours.
        constexpr sqlite3_int64 lock_lifetime_seconds = 24LL * 60 * 60;
        const sqlite3_int64 reserved_at = static_cast<sqlite3_int64>(std::time(nullptr));
        const std::string ip_key = ipRestrictionKey(clientIP);
        sqlite3_stmt* stale_lock_stmt = nullptr;
        const char* delete_stale_lock =
            "DELETE FROM faucet_address_locks "
            "WHERE Tx_Address = ? AND ReservedAt <= ?;";
        if (sqlite3_prepare_v2(db, delete_stale_lock, -1, &stale_lock_stmt, nullptr) != SQLITE_OK) {
            logger << "[ERROR] Failed to prepare stale address lock cleanup: "
                   << sqlite3_errmsg(db) << std::endl;
            rollback_reservation();
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        const bool stale_lock_bindings_ok =
            sqlite3_bind_text(stale_lock_stmt, 1, tnAddr.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_int64(stale_lock_stmt, 2, reserved_at - lock_lifetime_seconds) == SQLITE_OK;
        const int stale_lock_rc = stale_lock_bindings_ok
            ? sqlite3_step(stale_lock_stmt)
            : SQLITE_ERROR;
        sqlite3_finalize(stale_lock_stmt);

        if (!stale_lock_bindings_ok || stale_lock_rc != SQLITE_DONE) {
            logger << "[ERROR] Failed to remove stale address lock: "
                   << sqlite3_errmsg(db) << std::endl;
            rollback_reservation();
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        sqlite3_stmt* stale_ip_lock_stmt = nullptr;
        const char* delete_stale_ip_lock =
            "DELETE FROM faucet_ip_locks "
            "WHERE IP_Key = ? AND ReservedAt <= ?;";
        if (sqlite3_prepare_v2(db, delete_stale_ip_lock, -1,
                              &stale_ip_lock_stmt, nullptr) != SQLITE_OK) {
            logger << "[ERROR] Failed to prepare stale IP lock cleanup: "
                   << sqlite3_errmsg(db) << std::endl;
            rollback_reservation();
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        const bool stale_ip_lock_bindings_ok =
            sqlite3_bind_text(stale_ip_lock_stmt, 1, ip_key.c_str(), -1,
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_int64(stale_ip_lock_stmt, 2,
                               reserved_at - lock_lifetime_seconds) == SQLITE_OK;
        const int stale_ip_lock_rc = stale_ip_lock_bindings_ok
            ? sqlite3_step(stale_ip_lock_stmt)
            : SQLITE_ERROR;
        sqlite3_finalize(stale_ip_lock_stmt);

        if (!stale_ip_lock_bindings_ok || stale_ip_lock_rc != SQLITE_DONE) {
            logger << "[ERROR] Failed to remove stale IP lock: "
                   << sqlite3_errmsg(db) << std::endl;
            rollback_reservation();
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        // The primary key makes this an atomic cross-thread/process lock.
        sqlite3_stmt* reservation_stmt = nullptr;
        const char* insert_lock =
            "INSERT INTO faucet_address_locks (Tx_Address, ReservedAt) VALUES (?, ?);";
        if (sqlite3_prepare_v2(db, insert_lock, -1, &reservation_stmt, nullptr) != SQLITE_OK) {
            logger << "[ERROR] Failed to prepare address lock: "
                   << sqlite3_errmsg(db) << std::endl;
            rollback_reservation();
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        const bool reservation_bindings_ok =
            sqlite3_bind_text(reservation_stmt, 1, tnAddr.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_int64(reservation_stmt, 2, reserved_at) == SQLITE_OK;
        const int reservation_rc = reservation_bindings_ok
            ? sqlite3_step(reservation_stmt)
            : SQLITE_ERROR;
        sqlite3_finalize(reservation_stmt);

        if (!reservation_bindings_ok || reservation_rc != SQLITE_DONE) {
            const bool already_processing = reservation_rc == SQLITE_CONSTRAINT;
            if (!already_processing) {
                logger << "[ERROR] Failed to acquire address lock: "
                       << sqlite3_errmsg(db) << std::endl;
            }
            rollback_reservation();
            transRes[already_processing ? "error" : "tx-error"] = already_processing
                ? "A transaction for this address is already being processed. Please try again later."
                : "Something went wrong.";
            transRes["status"] = false;
            return {transRes, already_processing ? 429 : 500};
        }

        sqlite3_stmt* ip_reservation_stmt = nullptr;
        const char* insert_ip_lock =
            "INSERT INTO faucet_ip_locks (IP_Key, ReservedAt) VALUES (?, ?);";
        if (sqlite3_prepare_v2(db, insert_ip_lock, -1,
                              &ip_reservation_stmt, nullptr) != SQLITE_OK) {
            logger << "[ERROR] Failed to prepare IP lock: "
                   << sqlite3_errmsg(db) << std::endl;
            rollback_reservation();
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        const bool ip_reservation_bindings_ok =
            sqlite3_bind_text(ip_reservation_stmt, 1, ip_key.c_str(), -1,
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_int64(ip_reservation_stmt, 2, reserved_at) == SQLITE_OK;
        const int ip_reservation_rc = ip_reservation_bindings_ok
            ? sqlite3_step(ip_reservation_stmt)
            : SQLITE_ERROR;
        sqlite3_finalize(ip_reservation_stmt);

        if (!ip_reservation_bindings_ok || ip_reservation_rc != SQLITE_DONE) {
            const bool ip_already_processing = ip_reservation_rc == SQLITE_CONSTRAINT;
            if (!ip_already_processing) {
                logger << "[ERROR] Failed to acquire IP lock: "
                       << sqlite3_errmsg(db) << std::endl;
            }
            rollback_reservation();
            transRes[ip_already_processing ? "error" : "tx-error"] =
                ip_already_processing
                    ? "A transaction from this network is already being processed. Please try again later."
                    : "Something went wrong.";
            transRes["status"] = false;
            return {transRes, ip_already_processing ? 429 : 500};
        }

        transaction_error = nullptr;
        if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &transaction_error) != SQLITE_OK) {
            logger << "[ERROR] Failed to commit address lock: "
                   << (transaction_error ? transaction_error : sqlite3_errmsg(db)) << std::endl;
            sqlite3_free(transaction_error);
            rollback_reservation();
            transRes["tx-error"] = "Something went wrong.";
            transRes["status"] = false;
            return {transRes, 500};
        }

        auto release_reservation = [this, &tnAddr, &ip_key]() {
            char* cleanup_error = nullptr;
            if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr,
                             &cleanup_error) != SQLITE_OK) {
                logger << "[ERROR] Failed to begin lock cleanup: "
                       << (cleanup_error ? cleanup_error : sqlite3_errmsg(db))
                       << std::endl;
                sqlite3_free(cleanup_error);
                return false;
            }

            auto rollback_cleanup = [this]() {
                sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
            };

            auto delete_lock = [this](const char* sql, const std::string& value) {
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                    return false;
                }
                const bool bind_ok =
                    sqlite3_bind_text(stmt, 1, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
                const int step_rc = bind_ok ? sqlite3_step(stmt) : SQLITE_ERROR;
                sqlite3_finalize(stmt);
                return bind_ok && step_rc == SQLITE_DONE;
            };

            if (!delete_lock(
                    "DELETE FROM faucet_address_locks WHERE Tx_Address = ?;", tnAddr) ||
                !delete_lock(
                    "DELETE FROM faucet_ip_locks WHERE IP_Key = ?;", ip_key)) {
                logger << "[ERROR] Failed to clean up faucet locks: "
                       << sqlite3_errmsg(db) << std::endl;
                rollback_cleanup();
                return false;
            }

            cleanup_error = nullptr;
            if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &cleanup_error) != SQLITE_OK) {
                logger << "[ERROR] Failed to commit faucet lock cleanup: "
                       << (cleanup_error ? cleanup_error : sqlite3_errmsg(db))
                       << std::endl;
                sqlite3_free(cleanup_error);
                rollback_cleanup();
                return false;
            }
            return true;
        };

        auto record_successful_transfer = [this, &tnAddr, &clientIP, &ip_key]() {
            char* success_transaction_error = nullptr;
            if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr,
                             &success_transaction_error) != SQLITE_OK) {
                logger << "[ERROR] Failed to begin successful transfer transaction: "
                       << (success_transaction_error
                               ? success_transaction_error
                               : sqlite3_errmsg(db))
                       << std::endl;
                sqlite3_free(success_transaction_error);
                return false;
            }

            auto rollback_success = [this]() {
                char* rollback_error = nullptr;
                if (sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &rollback_error) != SQLITE_OK) {
                    logger << "[ERROR] Failed to roll back successful transfer record: "
                           << (rollback_error ? rollback_error : sqlite3_errmsg(db))
                           << std::endl;
                    sqlite3_free(rollback_error);
                }
            };

            const std::string completed_at = std::to_string(std::time(nullptr));
            const char* insert_user =
                "INSERT INTO users (Tx_Address, Tx_Amount, IP, Timestamp) "
                "VALUES (?, ?, ?, ?);";
            sqlite3_stmt* insert_stmt = nullptr;
            if (sqlite3_prepare_v2(db, insert_user, -1, &insert_stmt, nullptr) != SQLITE_OK) {
                logger << "[ERROR] Failed to prepare successful transfer record: "
                       << sqlite3_errmsg(db) << std::endl;
                rollback_success();
                return false;
            }

            const bool insert_bindings_ok =
                sqlite3_bind_text(insert_stmt, 1, tnAddr.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                sqlite3_bind_int64(insert_stmt, 2, AMOUNT / 1000000000) == SQLITE_OK &&
                sqlite3_bind_text(insert_stmt, 3, clientIP.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                sqlite3_bind_text(insert_stmt, 4, completed_at.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
            const int insert_rc = insert_bindings_ok
                ? sqlite3_step(insert_stmt)
                : SQLITE_ERROR;
            sqlite3_finalize(insert_stmt);

            if (!insert_bindings_ok || insert_rc != SQLITE_DONE) {
                logger << "[ERROR] Failed to save successful transfer: "
                       << sqlite3_errmsg(db) << std::endl;
                rollback_success();
                return false;
            }

            const char* delete_lock =
                "DELETE FROM faucet_address_locks WHERE Tx_Address = ?;";
            sqlite3_stmt* delete_stmt = nullptr;
            if (sqlite3_prepare_v2(db, delete_lock, -1, &delete_stmt, nullptr) != SQLITE_OK) {
                logger << "[ERROR] Failed to prepare successful address lock cleanup: "
                       << sqlite3_errmsg(db) << std::endl;
                rollback_success();
                return false;
            }

            const bool delete_binding_ok =
                sqlite3_bind_text(delete_stmt, 1, tnAddr.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
            const int delete_rc = delete_binding_ok
                ? sqlite3_step(delete_stmt)
                : SQLITE_ERROR;
            sqlite3_finalize(delete_stmt);

            if (!delete_binding_ok || delete_rc != SQLITE_DONE) {
                logger << "[ERROR] Failed to remove successful address lock: "
                       << sqlite3_errmsg(db) << std::endl;
                rollback_success();
                return false;
            }

            const char* delete_ip_lock =
                "DELETE FROM faucet_ip_locks WHERE IP_Key = ?;";
            sqlite3_stmt* delete_ip_stmt = nullptr;
            if (sqlite3_prepare_v2(db, delete_ip_lock, -1,
                                  &delete_ip_stmt, nullptr) != SQLITE_OK) {
                logger << "[ERROR] Failed to prepare successful IP lock cleanup: "
                       << sqlite3_errmsg(db) << std::endl;
                rollback_success();
                return false;
            }

            const bool delete_ip_binding_ok =
                sqlite3_bind_text(delete_ip_stmt, 1, ip_key.c_str(), -1,
                                  SQLITE_TRANSIENT) == SQLITE_OK;
            const int delete_ip_rc = delete_ip_binding_ok
                ? sqlite3_step(delete_ip_stmt)
                : SQLITE_ERROR;
            sqlite3_finalize(delete_ip_stmt);

            if (!delete_ip_binding_ok || delete_ip_rc != SQLITE_DONE) {
                logger << "[ERROR] Failed to remove successful IP lock: "
                       << sqlite3_errmsg(db) << std::endl;
                rollback_success();
                return false;
            }

            success_transaction_error = nullptr;
            if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr,
                             &success_transaction_error) != SQLITE_OK) {
                logger << "[ERROR] Failed to commit successful transfer record: "
                       << (success_transaction_error
                               ? success_transaction_error
                               : sqlite3_errmsg(db))
                       << std::endl;
                sqlite3_free(success_transaction_error);
                rollback_success();
                return false;
            }

            return true;
        };

        for (int attempt = 0; attempt < 6; ++attempt) {
            try {
                std::cout << "Wallet Url : " << WALLET_URL << std::endl;
                cpr::Header headers = cpr::Header{std::make_pair("Content-Type", "application/json")};
                cpr::Response res = cpr::Post(
                    cpr::Url{WALLET_URL},
                    headers,
                    cpr::Body{payload},
                    cpr::ConnectTimeout{wallet_connect_timeout_ms});

                if (res.error) {
                    logger << "[ERROR] HTTP request failed While Transfer: " << res.error.message << std::endl;
                    logger << "[WARN] Faucet locks retained because the transfer result is ambiguous." << std::endl;
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
                if (tx_hash.empty()) {
                    throw std::runtime_error("Wallet returned an empty transaction hash");
                }

                bool transfer_recorded = false;
                for (int database_attempt = 0; database_attempt < 3; ++database_attempt) {
                    if (record_successful_transfer()) {
                        transfer_recorded = true;
                        break;
                    }
                    if (database_attempt < 2) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }

                if (!transfer_recorded) {
                    // Funds were sent, so return the confirmed transaction to
                    // the client. Retained locks prevent an unsafe retry.
                    logger << "[CRITICAL] Transfer " << tx_hash
                           << " succeeded but could not be written to users; "
                           << "the faucet locks were retained." << std::endl;
                    transRes["warning"] =
                        "Funds were sent, but transaction history recording requires administrator attention.";
                }

                transRes["tx_hash"] = tx_hash;
                transRes["amount"] = AMOUNT / 1000000000;
                transRes["recorded"] = transfer_recorded;
                transRes["status"] = true;

                return {transRes, 200};
            } catch (const std::exception& e) {
                logger << "[EXCEPTION] Exception while processing transfer response: "
                       << e.what() << std::endl;
                logger << "[WARN] Faucet locks retained because the transfer result is ambiguous." << std::endl;
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
