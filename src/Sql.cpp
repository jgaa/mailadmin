#include "Sql.h"
#include "logging.h"
#include <memory>

namespace mailadmin {
Connection::Connection(const Config& config) {
    if (mysql_thread_init()) throw ApiError{503, "database_unavailable", "Database worker initialization failed"};
    mysql_ = mysql_init(nullptr);
    if (!mysql_) { mysql_thread_end(); throw std::bad_alloc{}; }
    auto timeout = config.database_timeout_seconds;
    unsigned local_infile = 0;
    mysql_options(mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(mysql_, MYSQL_OPT_READ_TIMEOUT, &timeout);
    mysql_options(mysql_, MYSQL_OPT_WRITE_TIMEOUT, &timeout);
    mysql_options(mysql_, MYSQL_OPT_LOCAL_INFILE, &local_infile);
    if (!mysql_real_connect(mysql_, "localhost", config.database_user.c_str(), config.database_password.c_str(),
            config.database_name.c_str(), 0, config.database_socket.c_str(), 0)
        || mysql_set_character_set(mysql_, "utf8mb4")) {
        MA_ERROR << "Database connection failed; connector_code=" << mysql_errno(mysql_);
        close();
        throw ApiError{503, "database_unavailable", "Cannot connect to the mail database"};
    }
}
Connection::~Connection() { close(); }
void Connection::close() noexcept {
    if (mysql_) { mysql_close(mysql_); mysql_ = nullptr; mysql_thread_end(); }
}
Rows Connection::query(std::string_view sql, const Parameters& parameters) {
    std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> statement{mysql_stmt_init(mysql_), mysql_stmt_close};
    if (!statement) throw std::bad_alloc{};
    auto fail = [&]() -> void {
        const auto code = mysql_stmt_errno(statement.get());
        // Never log SQL, bound values or connector text (which can echo data).
        MA_ERROR << "Database operation failed; connector_code=" << code;
        if (code == 1062) throw ApiError{409, "already_exists", "An address, domain or organization already exists"};
        if (code == 1451 || code == 1452) throw ApiError{409, "ownership_conflict", "Resource ownership prevents this operation"};
        throw ApiError{503, "database_unavailable", "Mail database operation failed"};
    };
    if (mysql_stmt_prepare(statement.get(), sql.data(), sql.size())) fail();
    if (mysql_stmt_param_count(statement.get()) != parameters.size())
        throw std::logic_error{"SQL parameter count mismatch"};
    std::vector<MYSQL_BIND> inputs(parameters.size());
    std::vector<my_bool> nulls(parameters.size());
    for (std::size_t index = 0; index != parameters.size(); ++index) {
        nulls[index] = !parameters[index];
        inputs[index].buffer_type = MYSQL_TYPE_STRING;
        inputs[index].is_null = &nulls[index];
        if (parameters[index]) {
            inputs[index].buffer = const_cast<char*>(parameters[index]->data());
            inputs[index].buffer_length = parameters[index]->size();
        }
    }
    if (!inputs.empty() && mysql_stmt_bind_param(statement.get(), inputs.data())) fail();
    if (mysql_stmt_execute(statement.get())) fail();
    const auto count = mysql_stmt_field_count(statement.get());
    if (!count) return {};
    std::vector<std::vector<char>> buffers(count, std::vector<char>(65537));
    std::vector<MYSQL_BIND> outputs(count);
    std::vector<unsigned long> lengths(count);
    std::vector<my_bool> result_nulls(count);
    for (unsigned index = 0; index != count; ++index) {
        outputs[index].buffer_type = MYSQL_TYPE_STRING;
        outputs[index].buffer = buffers[index].data();
        outputs[index].buffer_length = buffers[index].size();
        outputs[index].length = &lengths[index];
        outputs[index].is_null = &result_nulls[index];
    }
    if (mysql_stmt_bind_result(statement.get(), outputs.data())) fail();
    Rows rows;
    for (;;) {
        const auto status = mysql_stmt_fetch(statement.get());
        if (status == MYSQL_NO_DATA) break;
        if (status == MYSQL_DATA_TRUNCATED)
            throw ApiError{409, "data_limit", "A stored field exceeds the supported limit"};
        if (status) fail();
        if (rows.size() == 10000) throw ApiError{503, "inventory_limit", "Inventory exceeds 10000 records"};
        std::vector<std::string> fields;
        for (unsigned index = 0; index != count; ++index)
            fields.emplace_back(result_nulls[index] ? std::string{} : std::string{buffers[index].data(), lengths[index]});
        rows.emplace_back(std::move(fields));
    }
    return rows;
}
}
