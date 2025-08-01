#include "Generator.h"
#include "Context.h"
#include "Symbol.h"
#include "../../parser/include/AST.h"
#include <iostream>

namespace Quanta {

//=============================================================================
// Generator Implementation
//=============================================================================

Generator::Generator(Function* gen_func, Context* ctx, std::unique_ptr<ASTNode> body)
    : Object(ObjectType::Custom), generator_function_(gen_func), generator_context_(ctx),
      body_(std::move(body)), state_(State::SuspendedStart), pc_(0) {
}

Generator::GeneratorResult Generator::next(const Value& value) {
    if (state_ == State::Completed) {
        return GeneratorResult(Value(), true);
    }
    
    if (state_ == State::SuspendedStart) {
        state_ = State::SuspendedYield;
        return execute_until_yield(Value());
    }
    
    return execute_until_yield(value);
}

Generator::GeneratorResult Generator::return_value(const Value& value) {
    if (state_ == State::Completed) {
        return GeneratorResult(value, true);
    }
    
    complete_generator(value);
    return GeneratorResult(value, true);
}

Generator::GeneratorResult Generator::throw_exception(const Value& exception) {
    if (state_ == State::Completed) {
        generator_context_->throw_exception(exception);
        return GeneratorResult(Value(), true);
    }
    
    // Throw exception in generator context
    generator_context_->throw_exception(exception);
    complete_generator(Value());
    return GeneratorResult(Value(), true);
}

Value Generator::get_iterator() {
    // Generator objects are their own iterators
    return Value(this);
}

Generator::GeneratorResult Generator::execute_until_yield(const Value& sent_value) {
    if (!body_) {
        complete_generator(Value());
        return GeneratorResult(Value(), true);
    }
    
    try {
        // Store sent value for yield expressions
        last_value_ = sent_value;
        
        // Execute the generator body
        Value result = body_->evaluate(*generator_context_);
        
        // If we reach here without yielding, the generator is done
        complete_generator(result);
        return GeneratorResult(result, true);
        
    } catch (const std::exception& e) {
        // Handle generator exceptions
        complete_generator(Value());
        generator_context_->throw_exception(Value(e.what()));
        return GeneratorResult(Value(), true);
    }
}

void Generator::complete_generator(const Value& value) {
    state_ = State::Completed;
    last_value_ = value;
}

// Generator built-in methods
Value Generator::generator_next(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value("Generator.prototype.next called on non-object"));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value("Generator.prototype.next called on non-generator"));
        return Value();
    }
    
    Generator* generator = static_cast<Generator*>(obj);
    Value sent_value = args.empty() ? Value() : args[0];
    
    auto result = generator->next(sent_value);
    
    // Create iterator result object
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));
    
    return Value(result_obj.release());
}

Value Generator::generator_return(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value("Generator.prototype.return called on non-object"));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value("Generator.prototype.return called on non-generator"));
        return Value();
    }
    
    Generator* generator = static_cast<Generator*>(obj);
    Value return_value = args.empty() ? Value() : args[0];
    
    auto result = generator->return_value(return_value);
    
    // Create iterator result object
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));
    
    return Value(result_obj.release());
}

Value Generator::generator_throw(Context& ctx, const std::vector<Value>& args) {
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value("Generator.prototype.throw called on non-object"));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value("Generator.prototype.throw called on non-generator"));
        return Value();
    }
    
    Generator* generator = static_cast<Generator*>(obj);
    Value exception = args.empty() ? Value() : args[0];
    
    auto result = generator->throw_exception(exception);
    
    // Create iterator result object
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", result.value);
    result_obj->set_property("done", Value(result.done));
    
    return Value(result_obj.release());
}

void Generator::setup_generator_prototype(Context& ctx) {
    // Create Generator.prototype
    auto gen_prototype = ObjectFactory::create_object();
    
    // Add next method
    auto next_fn = ObjectFactory::create_native_function("next", generator_next);
    gen_prototype->set_property("next", Value(next_fn.release()));
    
    // Add return method
    auto return_fn = ObjectFactory::create_native_function("return", generator_return);
    gen_prototype->set_property("return", Value(return_fn.release()));
    
    // Add throw method
    auto throw_fn = ObjectFactory::create_native_function("throw", generator_throw);
    gen_prototype->set_property("throw", Value(throw_fn.release()));
    
    // Add Symbol.iterator method (generators are iterable)
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto iterator_fn = ObjectFactory::create_native_function("@@iterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; // Unused parameter
                return ctx.get_binding("this");
            });
        gen_prototype->set_property(iterator_symbol->to_string(), Value(iterator_fn.release()));
    }
    
    ctx.create_binding("GeneratorPrototype", Value(gen_prototype.release()));
}

//=============================================================================
// GeneratorFunction Implementation
//=============================================================================

GeneratorFunction::GeneratorFunction(const std::string& name, 
                                   const std::vector<std::string>& params,
                                   std::unique_ptr<ASTNode> body,
                                   Context* closure_context)
    : Function(name, params, nullptr, closure_context), body_(std::move(body)) {
}

Value GeneratorFunction::call(Context& ctx, const std::vector<Value>& args, Value this_value) {
    (void)this_value; // Unused parameter
    
    // Create new generator instance
    auto generator = create_generator(ctx, args);
    return Value(generator.release());
}

std::unique_ptr<Generator> GeneratorFunction::create_generator(Context& ctx, const std::vector<Value>& args) {
    // Create new context for generator execution
    auto gen_context = std::make_unique<Context>(ctx.get_engine(), &ctx, Context::Type::Function);
    
    // Bind parameters
    const auto& params = get_parameters();
    for (size_t i = 0; i < params.size(); ++i) {
        Value arg = i < args.size() ? args[i] : Value();
        gen_context->create_binding(params[i], arg);
    }
    
    // Clone the body for the generator
    auto body_clone = body_->clone();
    
    return std::make_unique<Generator>(this, gen_context.release(), std::move(body_clone));
}

//=============================================================================
// YieldExpression Implementation
//=============================================================================

YieldExpression::YieldExpression(Type type, std::unique_ptr<ASTNode> argument)
    : type_(type), argument_(std::move(argument)) {
}

Value YieldExpression::evaluate(Context& ctx, Generator* generator) {
    Value yield_value = Value();
    
    if (argument_) {
        yield_value = argument_->evaluate(ctx);
    }
    
    if (type_ == Type::Yield) {
        // Simple yield - suspend generator and return value
        // This is a simplified implementation
        // In a full implementation, we'd need to integrate with the AST evaluator
        return yield_value;
    } else {
        // yield* - delegate to another iterable
        // This is a complex operation that requires iterator protocol
        // For now, just return the value
        return yield_value;
    }
}

} // namespace Quanta