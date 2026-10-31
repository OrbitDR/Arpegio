// Thin wrapper over one SQLite connection. All text is UTF-8. Failures throw std::runtime_error.
#pragma once

#include <concepts>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace arpegio {

class Statement {
public:
    Statement(sqlite3* database, std::string_view sql);
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&&) = delete;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    // Parameters count from 1, columns from 0, as in SQLite.
    Statement& bind(int index, std::string_view text);
    Statement& bind(int index, double value);
    Statement& bind_null(int index);
    template <std::integral T>
    Statement& bind(int index, T value) {
        return bind_integer(index, static_cast<int64_t>(value));
    }
    template <typename T>
    Statement& bind(int index, const std::optional<T>& value) {
        return value ? bind(index, *value) : bind_null(index);
    }

    bool step();       // true when a row is ready
    void run();        // steps to the end, for statements without rows
    int64_t single();  // first column of the first row
    void reset();

    bool is_null(int column) const;
    int64_t integer(int column) const;
    double real(int column) const;
    std::string text(int column) const;

private:
    Statement& bind_integer(int index, int64_t value);

    sqlite3* database_;
    sqlite3_stmt* statement_ = nullptr;
};

class Database {
public:
    explicit Database(const std::filesystem::path& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(const char* sql);
    Statement prepare(std::string_view sql) { return Statement(database_, sql); }

    // Prepares a statement and binds `values` to its parameters in order.
    template <typename... Values>
    Statement query(std::string_view sql, const Values&... values) {
        Statement statement = prepare(sql);
        int index = 0;
        (statement.bind(++index, values), ...);
        return statement;
    }

    int64_t last_insert_id() const;
    int user_version();
    void set_user_version(int version);

private:
    sqlite3* database_ = nullptr;
};

// Rolls back unless commit() was reached.
class Transaction {
public:
    explicit Transaction(Database& database);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit();

private:
    Database& database_;
    bool open_ = true;
};

}  // namespace arpegio
