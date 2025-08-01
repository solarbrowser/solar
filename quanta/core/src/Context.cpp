#include "Context.h"
#include "Engine.h"
#include "Error.h"
#include "JSON.h"
#include "ProxyReflect.h"
#include "WebAPI.h"
#include <iostream>
#include <sstream>
#include <limits>
#include <cstdlib>

namespace Quanta {

// Static member initialization
uint32_t Context::next_context_id_ = 1;

//=============================================================================
// Context Implementation
//=============================================================================

Context::Context(Engine* engine, Type type) 
    : type_(type), state_(State::Running), context_id_(next_context_id_++),
      lexical_environment_(nullptr), variable_environment_(nullptr), this_binding_(nullptr),
      execution_depth_(0), global_object_(nullptr), current_exception_(), has_exception_(false), 
      return_value_(), has_return_value_(false), engine_(engine) {
    
    if (type == Type::Global) {
        initialize_global_context();
    }
}

Context::Context(Engine* engine, Context* parent, Type type)
    : type_(type), state_(State::Running), context_id_(next_context_id_++),
      lexical_environment_(nullptr), variable_environment_(nullptr), this_binding_(nullptr),
      execution_depth_(0), global_object_(parent ? parent->global_object_ : nullptr),
      current_exception_(), has_exception_(false), return_value_(), has_return_value_(false), engine_(engine) {
    
    // Inherit built-ins from parent
    if (parent) {
        built_in_objects_ = parent->built_in_objects_;
        built_in_functions_ = parent->built_in_functions_;
    }
}

Context::~Context() {
    // Clear call stack
    call_stack_.clear();
}

void Context::set_global_object(Object* global) {
    global_object_ = global;
}

bool Context::has_binding(const std::string& name) const {
    if (lexical_environment_) {
        return lexical_environment_->has_binding(name);
    }
    return false;
}

Value Context::get_binding(const std::string& name) const {
    if (!check_execution_depth()) {
        // Prevent infinite recursion
        const_cast<Context*>(this)->throw_exception(Value("Maximum execution depth exceeded"));
        return Value();
    }
    
    increment_execution_depth();
    Value result;
    
    if (lexical_environment_) {
        result = lexical_environment_->get_binding(name);
    } else {
        result = Value(); // undefined
    }
    
    decrement_execution_depth();
    return result;
}

bool Context::set_binding(const std::string& name, const Value& value) {
    if (lexical_environment_) {
        return lexical_environment_->set_binding(name, value);
    }
    return false;
}

bool Context::create_binding(const std::string& name, const Value& value, bool mutable_binding) {
    if (variable_environment_) {
        return variable_environment_->create_binding(name, value, mutable_binding);
    }
    return false;
}

bool Context::delete_binding(const std::string& name) {
    if (lexical_environment_) {
        return lexical_environment_->delete_binding(name);
    }
    return false;
}

void Context::push_frame(std::unique_ptr<StackFrame> frame) {
    if (is_stack_overflow()) {
        throw_exception(Value("RangeError: Maximum call stack size exceeded"));
        return;
    }
    call_stack_.push_back(std::move(frame));
}

std::unique_ptr<StackFrame> Context::pop_frame() {
    if (call_stack_.empty()) {
        return nullptr;
    }
    
    auto frame = std::move(call_stack_.back());
    call_stack_.pop_back();
    return frame;
}

StackFrame* Context::current_frame() const {
    if (call_stack_.empty()) {
        return nullptr;
    }
    return call_stack_.back().get();
}

void Context::throw_exception(const Value& exception) {
    current_exception_ = exception;
    has_exception_ = true;
    state_ = State::Thrown;
}

void Context::clear_exception() {
    current_exception_ = Value();
    has_exception_ = false;
    if (state_ == State::Thrown) {
        state_ = State::Running;
    }
}

void Context::throw_error(const std::string& message) {
    auto error = Error::create_error(message);
    throw_exception(Value(error.release()));
}

void Context::throw_type_error(const std::string& message) {
    auto error = Error::create_type_error(message);
    throw_exception(Value(error.release()));
}

void Context::throw_reference_error(const std::string& message) {
    auto error = Error::create_reference_error(message);
    throw_exception(Value(error.release()));
}

void Context::throw_syntax_error(const std::string& message) {
    auto error = Error::create_syntax_error(message);
    throw_exception(Value(error.release()));
}

void Context::throw_range_error(const std::string& message) {
    auto error = Error::create_range_error(message);
    throw_exception(Value(error.release()));
}

void Context::register_built_in_object(const std::string& name, Object* object) {
    built_in_objects_[name] = object;
    
    // Also bind to global object if available
    if (global_object_) {
        global_object_->set_property(name, Value(object));
    }
}

void Context::register_built_in_function(const std::string& name, Function* function) {
    built_in_functions_[name] = function;
    
    // Also bind to global object if available
    if (global_object_) {
        global_object_->set_property(name, Value(function));
    }
}

Object* Context::get_built_in_object(const std::string& name) const {
    auto it = built_in_objects_.find(name);
    return (it != built_in_objects_.end()) ? it->second : nullptr;
}

Function* Context::get_built_in_function(const std::string& name) const {
    auto it = built_in_functions_.find(name);
    return (it != built_in_functions_.end()) ? it->second : nullptr;
}

std::string Context::get_stack_trace() const {
    std::ostringstream oss;
    oss << "Stack trace:\n";
    
    // Print frames in reverse order (most recent first)
    for (int i = static_cast<int>(call_stack_.size()) - 1; i >= 0; --i) {
        oss << "  at " << call_stack_[i]->to_string() << "\n";
    }
    
    return oss.str();
}

std::vector<std::string> Context::get_variable_names() const {
    std::vector<std::string> names;
    
    if (lexical_environment_) {
        auto env_names = lexical_environment_->get_binding_names();
        names.insert(names.end(), env_names.begin(), env_names.end());
    }
    
    return names;
}

std::string Context::debug_string() const {
    std::ostringstream oss;
    oss << "Context(id=" << context_id_ 
        << ", type=" << static_cast<int>(type_)
        << ", state=" << static_cast<int>(state_)
        << ", stack_depth=" << stack_depth()
        << ", has_exception=" << has_exception_ << ")";
    return oss.str();
}

bool Context::check_execution_depth() const {
    return execution_depth_ < max_execution_depth_;
}

void Context::initialize_global_context() {
    // Create global object
    global_object_ = ObjectFactory::create_object().release();
    this_binding_ = global_object_;
    
    // Create global environment
    auto global_env = std::make_unique<Environment>(Environment::Type::Global);
    lexical_environment_ = global_env.release();
    variable_environment_ = lexical_environment_;
    
    // Initialize built-ins
    initialize_built_ins();
    setup_global_bindings();
}

void Context::initialize_built_ins() {
    // Create built-in objects (placeholder implementations)
    
    // Object constructor - create as a proper native function
    auto object_constructor = ObjectFactory::create_native_function("Object",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Object constructor implementation - for now just create empty object
            if (args.size() == 0) {
                return Value(ObjectFactory::create_object().release());
            }
            // TODO: Handle Object(value) constructor calls
            return Value(ObjectFactory::create_object().release());
        });
    
    // Add Object static methods
    // Object.keys(obj) - returns array of own enumerable property names
    auto keys_fn = ObjectFactory::create_native_function("keys", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() == 0 || !args[0].is_object()) {
                ctx.throw_exception(Value("Object.keys called on non-object"));
                return Value();
            }
            
            Object* obj = args[0].as_object();
            auto keys = obj->get_enumerable_keys();
            
            auto result_array = ObjectFactory::create_array(keys.size());
            for (size_t i = 0; i < keys.size(); i++) {
                result_array->set_element(i, Value(keys[i]));
            }
            
            return Value(result_array.release());
        });
    object_constructor->set_property("keys", Value(keys_fn.release()));
    
    // Object.create(prototype) - creates new object with specified prototype
    auto create_fn = ObjectFactory::create_native_function("create",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            Object* prototype = nullptr;
            if (args.size() > 0 && args[0].is_object()) {
                prototype = args[0].as_object();
            }
            
            auto new_obj = ObjectFactory::create_object(prototype);
            return Value(new_obj.release());
        });
    object_constructor->set_property("create", Value(create_fn.release()));
    
    register_built_in_object("Object", object_constructor.release());
    
    // Array constructor
    auto array_constructor = ObjectFactory::create_function();
    register_built_in_object("Array", array_constructor.release());
    
    // Function constructor
    auto function_constructor = ObjectFactory::create_native_function("Function",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Function constructor implementation - basic placeholder
            return Value(ObjectFactory::create_function().release());
        });
    
    // Add Function.prototype methods that will be inherited by all functions
    // These will be added to the prototype, but for now add them as static methods
    
    // Function.prototype.call
    auto call_fn = ObjectFactory::create_native_function("call",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get the function that call was invoked on
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_exception(Value("Function.call called on non-function"));
                return Value();
            }
            
            Function* func = static_cast<Function*>(function_obj);
            Value this_arg = args.size() > 0 ? args[0] : Value();
            
            // Prepare arguments (skip the first 'this' argument)
            std::vector<Value> call_args;
            for (size_t i = 1; i < args.size(); i++) {
                call_args.push_back(args[i]);
            }
            
            return func->call(ctx, call_args, this_arg);
        });
    function_constructor->set_property("call", Value(call_fn.release()));
    
    // Function.prototype.apply
    auto apply_fn = ObjectFactory::create_native_function("apply",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            // Get the function that apply was invoked on
            Object* function_obj = ctx.get_this_binding();
            if (!function_obj || !function_obj->is_function()) {
                ctx.throw_exception(Value("Function.apply called on non-function"));
                return Value();
            }
            
            Function* func = static_cast<Function*>(function_obj);
            Value this_arg = args.size() > 0 ? args[0] : Value();
            
            // Prepare arguments from array
            std::vector<Value> call_args;
            if (args.size() > 1 && args[1].is_object()) {
                Object* args_array = args[1].as_object();
                if (args_array->is_array()) {
                    uint32_t length = args_array->get_length();
                    for (uint32_t i = 0; i < length; i++) {
                        call_args.push_back(args_array->get_element(i));
                    }
                }
            }
            
            return func->call(ctx, call_args, this_arg);
        });
    function_constructor->set_property("apply", Value(apply_fn.release()));
    
    register_built_in_object("Function", function_constructor.release());
    
    // String constructor - callable as function
    auto string_constructor = ObjectFactory::create_native_function("String",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value("");
            return Value(args[0].to_string());
        });
    register_built_in_object("String", string_constructor.release());
    
    // Number constructor - callable as function with ES5 constants
    auto number_constructor = ObjectFactory::create_native_function("Number",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(0.0);
            return Value(args[0].to_number());
        });
    number_constructor->set_property("MAX_VALUE", Value(1.7976931348623157e+308));
    number_constructor->set_property("MIN_VALUE", Value(5e-324));
    number_constructor->set_property("NaN", Value(std::numeric_limits<double>::quiet_NaN()));
    number_constructor->set_property("POSITIVE_INFINITY", Value(std::numeric_limits<double>::infinity()));
    number_constructor->set_property("NEGATIVE_INFINITY", Value(-std::numeric_limits<double>::infinity()));
    register_built_in_object("Number", number_constructor.release());
    
    // Boolean constructor - callable as function
    auto boolean_constructor = ObjectFactory::create_native_function("Boolean",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value(false);
            return Value(args[0].to_boolean());
        });
    register_built_in_object("Boolean", boolean_constructor.release());
    
    // Error constructor
    auto error_constructor = ObjectFactory::create_function();
    register_built_in_object("Error", error_constructor.release());
    
    // JSON object
    auto json_object = ObjectFactory::create_object();
    
    // JSON.parse
    auto json_parse = ObjectFactory::create_native_function("parse", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return JSON::js_parse(ctx, args);
        });
    json_object->set_property("parse", Value(json_parse.release()));
    
    // JSON.stringify  
    auto json_stringify = ObjectFactory::create_native_function("stringify",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            return JSON::js_stringify(ctx, args);
        });
    json_object->set_property("stringify", Value(json_stringify.release()));
    
    register_built_in_object("JSON", json_object.release());
    
    // Proxy constructor
    auto proxy_constructor = ObjectFactory::create_native_function("Proxy",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value("Proxy constructor requires target and handler arguments"));
                return Value();
            }
            
            if (!args[0].is_object() || !args[1].is_object()) {
                ctx.throw_exception(Value("Proxy constructor arguments must be objects"));
                return Value();
            }
            
            Object* target = args[0].as_object();
            Object* handler = args[1].as_object();
            
            // Create a new Proxy object
            auto proxy = std::make_unique<Proxy>(target, handler);
            return Value(proxy.release());
        });
    
    register_built_in_object("Proxy", proxy_constructor.release());
    
    // Reflect object with static methods
    auto reflect_object = ObjectFactory::create_object();
    
    // Reflect.get(target, propertyKey[, receiver])
    auto reflect_get = ObjectFactory::create_native_function("get",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value("Reflect.get requires target and propertyKey arguments"));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value("Reflect.get target must be an object"));
                return Value();
            }
            
            Object* target = args[0].as_object();
            std::string property = args[1].to_string();
            
            return target->get_property(property);
        });
    reflect_object->set_property("get", Value(reflect_get.release()));
    
    // Reflect.set(target, propertyKey, value[, receiver])
    auto reflect_set = ObjectFactory::create_native_function("set",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 3) {
                ctx.throw_exception(Value("Reflect.set requires target, propertyKey, and value arguments"));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value("Reflect.set target must be an object"));
                return Value();
            }
            
            Object* target = args[0].as_object();
            std::string property = args[1].to_string();
            Value value = args[2];
            
            bool success = target->set_property(property, value);
            return Value(success);
        });
    reflect_object->set_property("set", Value(reflect_set.release()));
    
    // Reflect.has(target, propertyKey)
    auto reflect_has = ObjectFactory::create_native_function("has",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.size() < 2) {
                ctx.throw_exception(Value("Reflect.has requires target and propertyKey arguments"));
                return Value();
            }
            
            if (!args[0].is_object()) {
                ctx.throw_exception(Value("Reflect.has target must be an object"));
                return Value();
            }
            
            Object* target = args[0].as_object();
            std::string property = args[1].to_string();
            
            bool has = target->has_property(property);
            return Value(has);
        });
    reflect_object->set_property("has", Value(reflect_has.release()));
    
    register_built_in_object("Reflect", reflect_object.release());
    
    // Web APIs
    setup_web_apis();
}

void Context::setup_web_apis() {
    // Timer APIs
    auto setTimeout_fn = ObjectFactory::create_native_function("setTimeout", WebAPI::setTimeout);
    lexical_environment_->create_binding("setTimeout", Value(setTimeout_fn.release()), false);
    
    auto setInterval_fn = ObjectFactory::create_native_function("setInterval", WebAPI::setInterval);
    lexical_environment_->create_binding("setInterval", Value(setInterval_fn.release()), false);
    
    auto clearTimeout_fn = ObjectFactory::create_native_function("clearTimeout", WebAPI::clearTimeout);
    lexical_environment_->create_binding("clearTimeout", Value(clearTimeout_fn.release()), false);
    
    auto clearInterval_fn = ObjectFactory::create_native_function("clearInterval", WebAPI::clearInterval);
    lexical_environment_->create_binding("clearInterval", Value(clearInterval_fn.release()), false);
    
    // Enhanced Console API
    auto console_obj = ObjectFactory::create_object();
    console_obj->set_property("error", Value(ObjectFactory::create_native_function("error", WebAPI::console_error).release()));
    console_obj->set_property("warn", Value(ObjectFactory::create_native_function("warn", WebAPI::console_warn).release()));
    console_obj->set_property("info", Value(ObjectFactory::create_native_function("info", WebAPI::console_info).release()));
    console_obj->set_property("debug", Value(ObjectFactory::create_native_function("debug", WebAPI::console_debug).release()));
    console_obj->set_property("trace", Value(ObjectFactory::create_native_function("trace", WebAPI::console_trace).release()));
    console_obj->set_property("time", Value(ObjectFactory::create_native_function("time", WebAPI::console_time).release()));
    console_obj->set_property("timeEnd", Value(ObjectFactory::create_native_function("timeEnd", WebAPI::console_timeEnd).release()));
    
    // Update existing console object if it exists
    if (has_binding("console")) {
        Value existing_console = get_binding("console");
        if (existing_console.is_object()) {
            Object* console_existing = existing_console.as_object();
            console_existing->set_property("error", Value(ObjectFactory::create_native_function("error", WebAPI::console_error).release()));
            console_existing->set_property("warn", Value(ObjectFactory::create_native_function("warn", WebAPI::console_warn).release()));
            console_existing->set_property("info", Value(ObjectFactory::create_native_function("info", WebAPI::console_info).release()));
            console_existing->set_property("debug", Value(ObjectFactory::create_native_function("debug", WebAPI::console_debug).release()));
            console_existing->set_property("trace", Value(ObjectFactory::create_native_function("trace", WebAPI::console_trace).release()));
            console_existing->set_property("time", Value(ObjectFactory::create_native_function("time", WebAPI::console_time).release()));
            console_existing->set_property("timeEnd", Value(ObjectFactory::create_native_function("timeEnd", WebAPI::console_timeEnd).release()));
        }
    }
    
    // Fetch API
    auto fetch_fn = ObjectFactory::create_native_function("fetch", WebAPI::fetch);
    lexical_environment_->create_binding("fetch", Value(fetch_fn.release()), false);
    
    // DOM API - Document object
    auto document_obj = ObjectFactory::create_object();
    document_obj->set_property("getElementById", Value(ObjectFactory::create_native_function("getElementById", WebAPI::document_getElementById).release()));
    document_obj->set_property("createElement", Value(ObjectFactory::create_native_function("createElement", WebAPI::document_createElement).release()));
    document_obj->set_property("querySelector", Value(ObjectFactory::create_native_function("querySelector", WebAPI::document_querySelector).release()));
    lexical_environment_->create_binding("document", Value(document_obj.release()), false);
    
    // Window API
    auto alert_fn = ObjectFactory::create_native_function("alert", WebAPI::window_alert);
    lexical_environment_->create_binding("alert", Value(alert_fn.release()), false);
    
    auto confirm_fn = ObjectFactory::create_native_function("confirm", WebAPI::window_confirm);
    lexical_environment_->create_binding("confirm", Value(confirm_fn.release()), false);
    
    auto prompt_fn = ObjectFactory::create_native_function("prompt", WebAPI::window_prompt);
    lexical_environment_->create_binding("prompt", Value(prompt_fn.release()), false);
    
    // Storage API - localStorage
    auto localStorage_obj = ObjectFactory::create_object();
    localStorage_obj->set_property("getItem", Value(ObjectFactory::create_native_function("getItem", WebAPI::localStorage_getItem).release()));
    localStorage_obj->set_property("setItem", Value(ObjectFactory::create_native_function("setItem", WebAPI::localStorage_setItem).release()));
    localStorage_obj->set_property("removeItem", Value(ObjectFactory::create_native_function("removeItem", WebAPI::localStorage_removeItem).release()));
    localStorage_obj->set_property("clear", Value(ObjectFactory::create_native_function("clear", WebAPI::localStorage_clear).release()));
    lexical_environment_->create_binding("localStorage", Value(localStorage_obj.release()), false);
    
    // Storage API - sessionStorage (same as localStorage for now)
    auto sessionStorage_obj = ObjectFactory::create_object();
    sessionStorage_obj->set_property("getItem", Value(ObjectFactory::create_native_function("getItem", WebAPI::localStorage_getItem).release()));
    sessionStorage_obj->set_property("setItem", Value(ObjectFactory::create_native_function("setItem", WebAPI::localStorage_setItem).release()));
    sessionStorage_obj->set_property("removeItem", Value(ObjectFactory::create_native_function("removeItem", WebAPI::localStorage_removeItem).release()));
    sessionStorage_obj->set_property("clear", Value(ObjectFactory::create_native_function("clear", WebAPI::localStorage_clear).release()));
    lexical_environment_->create_binding("sessionStorage", Value(sessionStorage_obj.release()), false);
    
    // URL API
    auto URL_constructor_fn = ObjectFactory::create_native_function("URL", WebAPI::URL_constructor);
    lexical_environment_->create_binding("URL", Value(URL_constructor_fn.release()), false);
    
    // Event system - Global event functions
    auto addEventListener_fn = ObjectFactory::create_native_function("addEventListener", WebAPI::addEventListener);
    lexical_environment_->create_binding("addEventListener", Value(addEventListener_fn.release()), false);
    
    auto removeEventListener_fn = ObjectFactory::create_native_function("removeEventListener", WebAPI::removeEventListener);
    lexical_environment_->create_binding("removeEventListener", Value(removeEventListener_fn.release()), false);
    
    auto dispatchEvent_fn = ObjectFactory::create_native_function("dispatchEvent", WebAPI::dispatchEvent);
    lexical_environment_->create_binding("dispatchEvent", Value(dispatchEvent_fn.release()), false);
    
    // Crypto API
    auto crypto_obj = ObjectFactory::create_object();
    auto crypto_randomUUID_fn = ObjectFactory::create_native_function("randomUUID",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            // Generate a simple UUID v4
            std::string uuid = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
            const char* chars = "0123456789abcdef";
            for (char& c : uuid) {
                if (c == 'x') {
                    c = chars[rand() % 16];
                } else if (c == 'y') {
                    c = chars[8 + (rand() % 4)];
                }
            }
            return Value(uuid);
        });
    crypto_obj->set_property("randomUUID", Value(crypto_randomUUID_fn.release()));
    
    auto crypto_getRandomValues_fn = ObjectFactory::create_native_function("getRandomValues",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            std::cout << "crypto.getRandomValues: Generating random values (simulated)" << std::endl;
            return Value("random-values-array");
        });
    crypto_obj->set_property("getRandomValues", Value(crypto_getRandomValues_fn.release()));
    
    lexical_environment_->create_binding("crypto", Value(crypto_obj.release()), false);
    
    // FormData API
    auto FormData_constructor_fn = ObjectFactory::create_native_function("FormData",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            auto formData = ObjectFactory::create_object();
            
            // FormData.append method
            auto append_fn = ObjectFactory::create_native_function("append",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.size() >= 2) {
                        std::string key = args[0].to_string();
                        std::string value = args[1].to_string();
                        std::cout << "FormData.append: '" << key << "' = '" << value << "'" << std::endl;
                    }
                    return Value();
                });
            formData->set_property("append", Value(append_fn.release()));
            
            // FormData.get method
            auto get_fn = ObjectFactory::create_native_function("get",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.size() >= 1) {
                        std::string key = args[0].to_string();
                        std::cout << "FormData.get: Getting '" << key << "'" << std::endl;
                        return Value("form-data-value-" + key);
                    }
                    return Value();
                });
            formData->set_property("get", Value(get_fn.release()));
            
            return Value(formData.release());
        });
    lexical_environment_->create_binding("FormData", Value(FormData_constructor_fn.release()), false);
    
    // Blob API
    auto Blob_constructor_fn = ObjectFactory::create_native_function("Blob",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            auto blob = ObjectFactory::create_object();
            
            // Blob.size property
            blob->set_property("size", Value(1024.0));
            
            // Blob.type property
            blob->set_property("type", Value("application/octet-stream"));
            
            // Blob.text method
            auto text_fn = ObjectFactory::create_native_function("text",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx; (void)args;
                    std::cout << "Blob.text: Reading blob as text (simulated)" << std::endl;
                    return Value("blob-text-content");
                });
            blob->set_property("text", Value(text_fn.release()));
            
            std::cout << "Blob: Created new blob object" << std::endl;
            return Value(blob.release());
        });
    lexical_environment_->create_binding("Blob", Value(Blob_constructor_fn.release()), false);
    
    // URLSearchParams API
    auto URLSearchParams_constructor_fn = ObjectFactory::create_native_function("URLSearchParams",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx; (void)args;
            auto urlParams = ObjectFactory::create_object();
            
            // URLSearchParams.append method
            auto append_fn = ObjectFactory::create_native_function("append",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.size() >= 2) {
                        std::string key = args[0].to_string();
                        std::string value = args[1].to_string();
                        std::cout << "URLSearchParams.append: '" << key << "' = '" << value << "'" << std::endl;
                    }
                    return Value();
                });
            urlParams->set_property("append", Value(append_fn.release()));
            
            // URLSearchParams.get method
            auto get_fn = ObjectFactory::create_native_function("get",
                [](Context& ctx, const std::vector<Value>& args) -> Value {
                    (void)ctx;
                    if (args.size() >= 1) {
                        std::string key = args[0].to_string();
                        std::cout << "URLSearchParams.get: Getting '" << key << "'" << std::endl;
                        return Value("param-value-" + key);
                    }
                    return Value();
                });
            urlParams->set_property("get", Value(get_fn.release()));
            
            std::cout << "URLSearchParams: Created new URLSearchParams object" << std::endl;
            return Value(urlParams.release());
        });
    lexical_environment_->create_binding("URLSearchParams", Value(URLSearchParams_constructor_fn.release()), false);
    
    // RequestAnimationFrame API
    auto requestAnimationFrame_fn = ObjectFactory::create_native_function("requestAnimationFrame",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() >= 1) {
                static int animation_frame_id = 1;
                std::cout << "requestAnimationFrame: Scheduled callback for next frame (simulated)" << std::endl;
                return Value(static_cast<double>(animation_frame_id++));
            }
            return Value();
        });
    lexical_environment_->create_binding("requestAnimationFrame", Value(requestAnimationFrame_fn.release()), false);
    
    auto cancelAnimationFrame_fn = ObjectFactory::create_native_function("cancelAnimationFrame",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)ctx;
            if (args.size() >= 1) {
                double id = args[0].to_number();
                std::cout << "cancelAnimationFrame: Cancelled animation frame " << id << " (simulated)" << std::endl;
            }
            return Value();
        });
    lexical_environment_->create_binding("cancelAnimationFrame", Value(cancelAnimationFrame_fn.release()), false);
}

void Context::setup_global_bindings() {
    if (!lexical_environment_) return;
    
    // Global constants
    lexical_environment_->create_binding("undefined", Value(), false);
    lexical_environment_->create_binding("null", Value::null(), false);
    lexical_environment_->create_binding("true", Value(true), false);
    lexical_environment_->create_binding("false", Value(false), false);
    
    // Global values
    lexical_environment_->create_binding("NaN", Value(std::numeric_limits<double>::quiet_NaN()), false);
    lexical_environment_->create_binding("Infinity", Value(std::numeric_limits<double>::infinity()), false);
    
    // Missing global functions
    auto encode_uri_fn = ObjectFactory::create_native_function("encodeURI",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value("");
            std::string input = args[0].to_string();
            // Basic implementation - just return the input for now
            return Value(input);
        });
    lexical_environment_->create_binding("encodeURI", Value(encode_uri_fn.release()), false);
    
    auto decode_uri_fn = ObjectFactory::create_native_function("decodeURI",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value("");
            std::string input = args[0].to_string();
            // Basic implementation - just return the input for now
            return Value(input);
        });
    lexical_environment_->create_binding("decodeURI", Value(decode_uri_fn.release()), false);
    
    auto encode_uri_component_fn = ObjectFactory::create_native_function("encodeURIComponent",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value("");
            std::string input = args[0].to_string();
            // Basic implementation - just return the input for now
            return Value(input);
        });
    lexical_environment_->create_binding("encodeURIComponent", Value(encode_uri_component_fn.release()), false);
    
    auto decode_uri_component_fn = ObjectFactory::create_native_function("decodeURIComponent",
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            if (args.empty()) return Value("");
            std::string input = args[0].to_string();
            // Basic implementation - just return the input for now
            return Value(input);
        });
    lexical_environment_->create_binding("decodeURIComponent", Value(decode_uri_component_fn.release()), false);
    
    // Bind built-in objects to global environment
    for (const auto& pair : built_in_objects_) {
        bool bound = lexical_environment_->create_binding(pair.first, Value(pair.second), false);
        // Also ensure it's bound to global object for property access
        if (global_object_ && pair.second) {
            global_object_->set_property(pair.first, Value(pair.second));
        }
    }
}

//=============================================================================
// Return Value Handling
//=============================================================================

void Context::set_return_value(const Value& value) {
    return_value_ = value;
    has_return_value_ = true;
}

void Context::clear_return_value() {
    return_value_ = Value();
    has_return_value_ = false;
}

//=============================================================================
// StackFrame Implementation
//=============================================================================

StackFrame::StackFrame(Type type, Function* function, Object* this_binding)
    : type_(type), function_(function), this_binding_(this_binding),
      environment_(nullptr), program_counter_(0), line_number_(0), column_number_(0) {
}

Value StackFrame::get_argument(size_t index) const {
    if (index < arguments_.size()) {
        return arguments_[index];
    }
    return Value(); // undefined
}

bool StackFrame::has_local(const std::string& name) const {
    return local_variables_.find(name) != local_variables_.end();
}

Value StackFrame::get_local(const std::string& name) const {
    auto it = local_variables_.find(name);
    if (it != local_variables_.end()) {
        return it->second;
    }
    return Value(); // undefined
}

void StackFrame::set_local(const std::string& name, const Value& value) {
    local_variables_[name] = value;
}

void StackFrame::set_source_location(const std::string& location, uint32_t line, uint32_t column) {
    source_location_ = location;
    line_number_ = line;
    column_number_ = column;
}

std::string StackFrame::to_string() const {
    std::ostringstream oss;
    
    if (function_) {
        oss << "function";
    } else {
        oss << "anonymous";
    }
    
    if (!source_location_.empty()) {
        oss << " (" << source_location_;
        if (line_number_ > 0) {
            oss << ":" << line_number_;
            if (column_number_ > 0) {
                oss << ":" << column_number_;
            }
        }
        oss << ")";
    }
    
    return oss.str();
}

//=============================================================================
// Environment Implementation
//=============================================================================

Environment::Environment(Type type, Environment* outer)
    : type_(type), outer_environment_(outer), binding_object_(nullptr) {
}

Environment::Environment(Object* binding_object, Environment* outer)
    : type_(Type::Object), outer_environment_(outer), binding_object_(binding_object) {
}

bool Environment::has_binding(const std::string& name) const {
    if (has_own_binding(name)) {
        return true;
    }
    
    if (outer_environment_) {
        return outer_environment_->has_binding(name);
    }
    
    return false;
}

Value Environment::get_binding(const std::string& name) const {
    return get_binding_with_depth(name, 0);
}

Value Environment::get_binding_with_depth(const std::string& name, int depth) const {
    // Prevent infinite recursion
    if (depth > 100) {
        return Value(); // undefined
    }
    
    if (has_own_binding(name)) {
        if (type_ == Type::Object && binding_object_) {
            return binding_object_->get_property(name);
        } else {
            auto it = bindings_.find(name);
            if (it != bindings_.end()) {
                return it->second;
            }
        }
    }
    
    if (outer_environment_) {
        return outer_environment_->get_binding_with_depth(name, depth + 1);
    }
    
    return Value(); // undefined
}

bool Environment::set_binding(const std::string& name, const Value& value) {
    if (has_own_binding(name)) {
        if (type_ == Type::Object && binding_object_) {
            return binding_object_->set_property(name, value);
        } else {
            // Check if mutable
            if (is_mutable_binding(name)) {
                bindings_[name] = value;
                return true;
            }
            return false; // Immutable binding
        }
    }
    
    if (outer_environment_) {
        return outer_environment_->set_binding(name, value);
    }
    
    return false;
}

bool Environment::create_binding(const std::string& name, const Value& value, bool mutable_binding) {
    if (has_own_binding(name)) {
        return false; // Binding already exists
    }
    
    if (type_ == Type::Object && binding_object_) {
        return binding_object_->set_property(name, value);
    } else {
        bindings_[name] = value;
        mutable_flags_[name] = mutable_binding;
        initialized_flags_[name] = true;
        return true;
    }
}

bool Environment::delete_binding(const std::string& name) {
    if (has_own_binding(name)) {
        if (type_ == Type::Object && binding_object_) {
            return binding_object_->delete_property(name);
        } else {
            bindings_.erase(name);
            mutable_flags_.erase(name);
            initialized_flags_.erase(name);
            return true;
        }
    }
    
    return false;
}

bool Environment::is_mutable_binding(const std::string& name) const {
    auto it = mutable_flags_.find(name);
    return (it != mutable_flags_.end()) ? it->second : true; // Default to mutable
}

bool Environment::is_initialized_binding(const std::string& name) const {
    auto it = initialized_flags_.find(name);
    return (it != initialized_flags_.end()) ? it->second : false;
}

void Environment::initialize_binding(const std::string& name, const Value& value) {
    bindings_[name] = value;
    initialized_flags_[name] = true;
}

std::vector<std::string> Environment::get_binding_names() const {
    std::vector<std::string> names;
    
    if (type_ == Type::Object && binding_object_) {
        auto keys = binding_object_->get_own_property_keys();
        names.insert(names.end(), keys.begin(), keys.end());
    } else {
        for (const auto& pair : bindings_) {
            names.push_back(pair.first);
        }
    }
    
    return names;
}

std::string Environment::debug_string() const {
    std::ostringstream oss;
    oss << "Environment(type=" << static_cast<int>(type_)
        << ", bindings=" << bindings_.size() << ")";
    return oss.str();
}

bool Environment::has_own_binding(const std::string& name) const {
    if (type_ == Type::Object && binding_object_) {
        return binding_object_->has_own_property(name);
    } else {
        return bindings_.find(name) != bindings_.end();
    }
}

//=============================================================================
// ContextFactory Implementation
//=============================================================================

namespace ContextFactory {

std::unique_ptr<Context> create_global_context(Engine* engine) {
    return std::make_unique<Context>(engine, Context::Type::Global);
}

std::unique_ptr<Context> create_function_context(Engine* engine, Context* parent, Function* function) {
    auto context = std::make_unique<Context>(engine, parent, Context::Type::Function);
    
    // Create function environment
    auto func_env = std::make_unique<Environment>(Environment::Type::Function, parent->get_lexical_environment());
    context->set_lexical_environment(func_env.release());
    context->set_variable_environment(context->get_lexical_environment());
    
    return context;
}

std::unique_ptr<Context> create_eval_context(Engine* engine, Context* parent) {
    auto context = std::make_unique<Context>(engine, parent, Context::Type::Eval);
    
    // Eval context shares the parent's environment
    context->set_lexical_environment(parent->get_lexical_environment());
    context->set_variable_environment(parent->get_variable_environment());
    
    return context;
}

std::unique_ptr<Context> create_module_context(Engine* engine) {
    auto context = std::make_unique<Context>(engine, Context::Type::Module);
    
    // Create module environment
    auto module_env = std::make_unique<Environment>(Environment::Type::Module);
    context->set_lexical_environment(module_env.release());
    context->set_variable_environment(context->get_lexical_environment());
    
    return context;
}

} // namespace ContextFactory

} // namespace Quanta