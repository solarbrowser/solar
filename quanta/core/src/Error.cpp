#include "../include/Error.h"
#include <sstream>
#include <iostream>

namespace Quanta {

//=============================================================================
// Error Implementation
//=============================================================================

Error::Error(Type type, const std::string& message) 
    : Object(Object::ObjectType::Error), error_type_(type), message_(message), 
      line_number_(0), column_number_(0) {
    set_error_name();
    initialize_properties();
}

Error::Error(Type type, const std::string& message, const std::string& filename, int line, int column)
    : Object(Object::ObjectType::Error), error_type_(type), message_(message), 
      line_number_(line), column_number_(column), filename_(filename) {
    set_error_name();
    initialize_properties();
}

void Error::set_error_name() {
    name_ = type_to_name(error_type_);
}

void Error::initialize_properties() {
    // Set standard Error properties
    set_property("name", Value(name_));
    set_property("message", Value(message_));
    
    if (!stack_trace_.empty()) {
        set_property("stack", Value(stack_trace_));
    }
    
    if (line_number_ > 0) {
        set_property("lineNumber", Value(static_cast<double>(line_number_)));
    }
    
    if (column_number_ > 0) {
        set_property("columnNumber", Value(static_cast<double>(column_number_)));
    }
    
    if (!filename_.empty()) {
        set_property("fileName", Value(filename_));
    }
}

void Error::set_location(const std::string& filename, int line, int column) {
    filename_ = filename;
    line_number_ = line;
    column_number_ = column;
    initialize_properties();
}

std::string Error::to_string() const {
    if (message_.empty()) {
        return name_;
    }
    return name_ + ": " + message_;
}

void Error::generate_stack_trace() {
    std::ostringstream oss;
    oss << name_;
    if (!message_.empty()) {
        oss << ": " << message_;
    }
    
    if (!filename_.empty()) {
        oss << "\n    at " << filename_;
        if (line_number_ > 0) {
            oss << ":" << line_number_;
            if (column_number_ > 0) {
                oss << ":" << column_number_;
            }
        }
    }
    
    stack_trace_ = oss.str();
    set_property("stack", Value(stack_trace_));
}

std::string Error::type_to_name(Type type) {
    switch (type) {
        case Type::Error:           return "Error";
        case Type::TypeError:       return "TypeError";
        case Type::ReferenceError:  return "ReferenceError";
        case Type::SyntaxError:     return "SyntaxError";
        case Type::RangeError:      return "RangeError";
        case Type::URIError:        return "URIError";
        case Type::EvalError:       return "EvalError";
        case Type::AggregateError:  return "AggregateError";
        default:                    return "Error";
    }
}

//=============================================================================
// Static Factory Methods
//=============================================================================

std::unique_ptr<Error> Error::create_error(const std::string& message) {
    return std::make_unique<Error>(Type::Error, message);
}

std::unique_ptr<Error> Error::create_type_error(const std::string& message) {
    return std::make_unique<Error>(Type::TypeError, message);
}

std::unique_ptr<Error> Error::create_reference_error(const std::string& message) {
    return std::make_unique<Error>(Type::ReferenceError, message);
}

std::unique_ptr<Error> Error::create_syntax_error(const std::string& message) {
    return std::make_unique<Error>(Type::SyntaxError, message);
}

std::unique_ptr<Error> Error::create_range_error(const std::string& message) {
    return std::make_unique<Error>(Type::RangeError, message);
}

std::unique_ptr<Error> Error::create_uri_error(const std::string& message) {
    return std::make_unique<Error>(Type::URIError, message);
}

std::unique_ptr<Error> Error::create_eval_error(const std::string& message) {
    return std::make_unique<Error>(Type::EvalError, message);
}

//=============================================================================
// Exception Throwing Methods
//=============================================================================

void Error::throw_error(const std::string& message) {
    throw JavaScriptException(create_error(message));
}

void Error::throw_type_error(const std::string& message) {
    throw JavaScriptException(create_type_error(message));
}

void Error::throw_reference_error(const std::string& message) {
    throw JavaScriptException(create_reference_error(message));
}

void Error::throw_syntax_error(const std::string& message) {
    throw JavaScriptException(create_syntax_error(message));
}

void Error::throw_range_error(const std::string& message) {
    throw JavaScriptException(create_range_error(message));
}

//=============================================================================
// JavaScriptException Implementation
//=============================================================================

JavaScriptException::JavaScriptException(std::unique_ptr<Error> error) 
    : error_(std::move(error)) {
    what_message_ = error_->to_string();
}

const char* JavaScriptException::what() const noexcept {
    return what_message_.c_str();
}

} // namespace Quanta