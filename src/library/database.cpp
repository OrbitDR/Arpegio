#include "library/database.h"

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

#include "library/paths.h"

namespace arpegio {

namespace {

constexpr int kBusyTimeoutMs = 5000;

[[noreturn]] void fail(sqlite3* database) {
    throw std::runtime_error(std::string("database: ") + sqlite3_errmsg(database));
}

}  // namespace

Statement::Statement(sqlite3* database, std::string_view sql) : database_(database) {
    if (sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr) != SQLITE_OK) {
        fail(database);
    }
}

Statement::~Statement() {
    sqlite3_finalize(statement_);
}

Statement::Statement(Statement&& other) noexcept
    : database_(other.database_), statement_(std::exchange(other.statement_, nullptr)) {}

Statement& Statement::bind(int index, std::string_view text) {
    if (sqlite3_bind_text(statement_, index, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        fail(database_);
    }
    return *this;
}

Statement& Statement::bind(int index, double value) {
    if (sqlite3_bind_double(statement_, index, value) != SQLITE_OK) fail(database_);
    return *this;
}

Statement& Statement::bind_null(int index) {
    if (sqlite3_bind_null(statement_, index) != SQLITE_OK) fail(database_);
    return *this;
}

Statement& Statement::bind_integer(int index, int64_t value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) fail(database_);
    return *this;
}

bool Statement::step() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) return true;
    if (result == SQLITE_DONE) return false;
    fail(database_);
}

void Statement::run() {
    while (step()) {
    }
    reset();
}

int64_t Statement::single() {
    if (!step()) throw std::runtime_error("database: query returned no rows");
    const int64_t value = integer(0);
    reset();
    return value;
}

void Statement::reset() {
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
}

bool Statement::is_null(int column) const {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

int64_t Statement::integer(int column) const {
    return sqlite3_column_int64(statement_, column);
}

double Statement::real(int column) const {
    return sqlite3_column_double(statement_, column);
}

std::string Statement::text(int column) const {
    const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(statement_, column));
    if (!data) return {};
    return {data, static_cast<size_t>(sqlite3_column_bytes(statement_, column))};
}

Database::Database(const std::filesystem::path& path) {
    const int result =
        sqlite3_open_v2(path_to_utf8(path).c_str(), &database_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (result != SQLITE_OK) {
        const std::string text = database_ ? sqlite3_errmsg(database_) : sqlite3_errstr(result);
        sqlite3_close_v2(database_);
        throw std::runtime_error("database: " + text);
    }
    sqlite3_busy_timeout(database_, kBusyTimeoutMs);
}

Database::~Database() {
    sqlite3_close_v2(database_);
}

void Database::exec(const char* sql) {
    if (sqlite3_exec(database_, sql, nullptr, nullptr, nullptr) != SQLITE_OK) fail(database_);
}

int64_t Database::last_insert_id() const {
    return sqlite3_last_insert_rowid(database_);
}

int Database::user_version() {
    return static_cast<int>(prepare("PRAGMA user_version").single());
}

void Database::set_user_version(int version) {
    exec(("PRAGMA user_version = " + std::to_string(version)).c_str());
}

Transaction::Transaction(Database& database) : database_(database) {
    database_.exec("BEGIN IMMEDIATE");
}

Transaction::~Transaction() {
    if (!open_) return;
    try {
        database_.exec("ROLLBACK");
    } catch (const std::exception&) {
        // SQLite already rolled back when the failure was an I/O error or a full disk.
    }
}

void Transaction::commit() {
    database_.exec("COMMIT");
    open_ = false;
}

}  // namespace arpegio
