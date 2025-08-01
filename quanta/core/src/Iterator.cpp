#include "Iterator.h"
#include "Context.h"
#include "Symbol.h"
#include "MapSet.h"
#include "../../parser/include/AST.h"
#include <iostream>

namespace Quanta {

//=============================================================================
// Iterator Implementation
//=============================================================================

Iterator::Iterator(NextFunction next_fn) 
    : Object(ObjectType::Custom), next_fn_(next_fn), done_(false) {
}

Iterator::IteratorResult Iterator::next() {
    if (done_) {
        return IteratorResult(Value(), true);
    }
    
    auto result = next_fn_();
    if (result.done) {
        done_ = true;
    }
    
    return result;
}

Value Iterator::iterator_next(Context& ctx, const std::vector<Value>& args) {
    (void)args; // Unused parameter
    
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value("Iterator.prototype.next called on non-object"));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value("Iterator.prototype.next called on non-iterator"));
        return Value();
    }
    
    Iterator* iterator = static_cast<Iterator*>(obj);
    auto result = iterator->next();
    
    return create_iterator_result(result.value, result.done);
}

Value Iterator::iterator_return(Context& ctx, const std::vector<Value>& args) {
    Value return_value = args.empty() ? Value() : args[0];
    
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value("Iterator.prototype.return called on non-object"));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value("Iterator.prototype.return called on non-iterator"));
        return Value();
    }
    
    Iterator* iterator = static_cast<Iterator*>(obj);
    iterator->done_ = true;
    
    return create_iterator_result(return_value, true);
}

Value Iterator::iterator_throw(Context& ctx, const std::vector<Value>& args) {
    Value exception = args.empty() ? Value() : args[0];
    
    Value this_value = ctx.get_binding("this");
    if (!this_value.is_object()) {
        ctx.throw_exception(Value("Iterator.prototype.throw called on non-object"));
        return Value();
    }
    
    Object* obj = this_value.as_object();
    if (obj->get_type() != Object::ObjectType::Custom) {
        ctx.throw_exception(Value("Iterator.prototype.throw called on non-iterator"));
        return Value();
    }
    
    Iterator* iterator = static_cast<Iterator*>(obj);
    iterator->done_ = true;
    
    ctx.throw_exception(exception);
    return Value();
}

void Iterator::setup_iterator_prototype(Context& ctx) {
    // Create Iterator.prototype
    auto iterator_prototype = ObjectFactory::create_object();
    
    // Add next method
    auto next_fn = ObjectFactory::create_native_function("next", iterator_next);
    iterator_prototype->set_property("next", Value(next_fn.release()));
    
    // Add return method
    auto return_fn = ObjectFactory::create_native_function("return", iterator_return);
    iterator_prototype->set_property("return", Value(return_fn.release()));
    
    // Add throw method
    auto throw_fn = ObjectFactory::create_native_function("throw", iterator_throw);
    iterator_prototype->set_property("throw", Value(throw_fn.release()));
    
    // Add Symbol.iterator method (iterators are iterable)
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto self_iterator_fn = ObjectFactory::create_native_function("@@iterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; // Unused parameter
                return ctx.get_binding("this");
            });
        iterator_prototype->set_property(iterator_symbol->to_string(), Value(self_iterator_fn.release()));
    }
    
    ctx.create_binding("IteratorPrototype", Value(iterator_prototype.release()));
}

Value Iterator::create_iterator_result(const Value& value, bool done) {
    auto result_obj = ObjectFactory::create_object();
    result_obj->set_property("value", value);
    result_obj->set_property("done", Value(done));
    return Value(result_obj.release());
}

//=============================================================================
// ArrayIterator Implementation
//=============================================================================

ArrayIterator::ArrayIterator(Object* array, Kind kind) 
    : Iterator([this]() { return this->next_impl(); }), array_(array), kind_(kind), index_(0) {
}

std::unique_ptr<ArrayIterator> ArrayIterator::create_keys_iterator(Object* array) {
    return std::make_unique<ArrayIterator>(array, Kind::Keys);
}

std::unique_ptr<ArrayIterator> ArrayIterator::create_values_iterator(Object* array) {
    return std::make_unique<ArrayIterator>(array, Kind::Values);
}

std::unique_ptr<ArrayIterator> ArrayIterator::create_entries_iterator(Object* array) {
    return std::make_unique<ArrayIterator>(array, Kind::Entries);
}

Iterator::IteratorResult ArrayIterator::next_impl() {
    if (!array_ || index_ >= array_->get_length()) {
        return IteratorResult(Value(), true);
    }
    
    Value element = array_->get_element(index_);
    
    switch (kind_) {
        case Kind::Keys:
            return IteratorResult(Value(static_cast<double>(index_++)), false);
            
        case Kind::Values:
            index_++;
            return IteratorResult(element, false);
            
        case Kind::Entries: {
            auto entry_array = ObjectFactory::create_array(2);
            entry_array->set_element(0, Value(static_cast<double>(index_)));
            entry_array->set_element(1, element);
            index_++;
            return IteratorResult(Value(entry_array.release()), false);
        }
    }
    
    return IteratorResult(Value(), true);
}

//=============================================================================
// StringIterator Implementation
//=============================================================================

StringIterator::StringIterator(const std::string& str) 
    : Iterator([this]() { return this->next_impl(); }), string_(str), position_(0) {
}

Iterator::IteratorResult StringIterator::next_impl() {
    if (position_ >= string_.length()) {
        return IteratorResult(Value(), true);
    }
    
    // Simple character iteration (not Unicode-aware for now)
    std::string character(1, string_[position_++]);
    return IteratorResult(Value(character), false);
}

//=============================================================================
// MapIterator Implementation
//=============================================================================

MapIterator::MapIterator(Map* map, Kind kind) 
    : Iterator([this]() { return this->next_impl(); }), map_(map), kind_(kind), index_(0) {
}

Iterator::IteratorResult MapIterator::next_impl() {
    if (!map_ || index_ >= map_->size()) {
        return IteratorResult(Value(), true);
    }
    
    auto entries = map_->entries();
    if (index_ >= entries.size()) {
        return IteratorResult(Value(), true);
    }
    
    auto& entry = entries[index_++];
    
    switch (kind_) {
        case Kind::Keys:
            return IteratorResult(entry.first, false);
            
        case Kind::Values:
            return IteratorResult(entry.second, false);
            
        case Kind::Entries: {
            auto entry_array = ObjectFactory::create_array(2);
            entry_array->set_element(0, entry.first);
            entry_array->set_element(1, entry.second);
            return IteratorResult(Value(entry_array.release()), false);
        }
    }
    
    return IteratorResult(Value(), true);
}

//=============================================================================
// SetIterator Implementation
//=============================================================================

SetIterator::SetIterator(Set* set, Kind kind) 
    : Iterator([this]() { return this->next_impl(); }), set_(set), kind_(kind), index_(0) {
}

Iterator::IteratorResult SetIterator::next_impl() {
    if (!set_ || index_ >= set_->size()) {
        return IteratorResult(Value(), true);
    }
    
    auto values = set_->values();
    if (index_ >= values.size()) {
        return IteratorResult(Value(), true);
    }
    
    Value value = values[index_++];
    
    switch (kind_) {
        case Kind::Values:
            return IteratorResult(value, false);
            
        case Kind::Entries: {
            auto entry_array = ObjectFactory::create_array(2);
            entry_array->set_element(0, value);
            entry_array->set_element(1, value);
            return IteratorResult(Value(entry_array.release()), false);
        }
    }
    
    return IteratorResult(Value(), true);
}

//=============================================================================
// IterableUtils Implementation
//=============================================================================

namespace IterableUtils {

bool is_iterable(const Value& value) {
    if (!value.is_object()) {
        return false;
    }
    
    Object* obj = value.as_object();
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    
    if (!iterator_symbol) {
        return false;
    }
    
    return obj->has_property(iterator_symbol->to_string());
}

std::unique_ptr<Iterator> get_iterator(const Value& value, Context& ctx) {
    if (!value.is_object()) {
        return nullptr;
    }
    
    Object* obj = value.as_object();
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    
    if (!iterator_symbol) {
        return nullptr;
    }
    
    Value iterator_method = obj->get_property(iterator_symbol->to_string());
    if (!iterator_method.is_function()) {
        return nullptr;
    }
    
    // Call the iterator method
    Function* iterator_fn = iterator_method.as_function();
    Value iterator_result = iterator_fn->call(ctx, {}, value);
    
    if (!iterator_result.is_object()) {
        return nullptr;
    }
    
    Object* iterator_obj = iterator_result.as_object();
    if (iterator_obj->get_type() != Object::ObjectType::Custom) {
        return nullptr;
    }
    
    return std::unique_ptr<Iterator>(static_cast<Iterator*>(iterator_obj));
}

std::vector<Value> to_array(const Value& iterable, Context& ctx) {
    std::vector<Value> result;
    
    auto iterator = get_iterator(iterable, ctx);
    if (!iterator) {
        return result;
    }
    
    while (true) {
        auto iter_result = iterator->next();
        if (iter_result.done) {
            break;
        }
        result.push_back(iter_result.value);
    }
    
    return result;
}

void for_of_loop(const Value& iterable, 
                 std::function<void(const Value&)> callback, 
                 Context& ctx) {
    auto iterator = get_iterator(iterable, ctx);
    if (!iterator) {
        ctx.throw_exception(Value("Value is not iterable"));
        return;
    }
    
    while (true) {
        auto iter_result = iterator->next();
        if (iter_result.done) {
            break;
        }
        callback(iter_result.value);
    }
}

void setup_array_iterator_methods(Context& ctx) {
    // Get Array.prototype
    Value array_constructor = ctx.get_binding("Array");
    if (!array_constructor.is_function()) {
        return;
    }
    
    Function* array_fn = array_constructor.as_function();
    Value array_prototype = array_fn->get_property("prototype");
    if (!array_prototype.is_object()) {
        return;
    }
    
    Object* array_proto = array_prototype.as_object();
    
    // Add keys() method
    auto keys_fn = ObjectFactory::create_native_function("keys", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; // Unused parameter
            Value this_value = ctx.get_binding("this");
            if (!this_value.is_object()) {
                ctx.throw_exception(Value("Array.prototype.keys called on non-object"));
                return Value();
            }
            
            Object* array = this_value.as_object();
            auto iterator = ArrayIterator::create_keys_iterator(array);
            return Value(iterator.release());
        });
    
    // Add values() method
    auto values_fn = ObjectFactory::create_native_function("values", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; // Unused parameter
            Value this_value = ctx.get_binding("this");
            if (!this_value.is_object()) {
                ctx.throw_exception(Value("Array.prototype.values called on non-object"));
                return Value();
            }
            
            Object* array = this_value.as_object();
            auto iterator = ArrayIterator::create_values_iterator(array);
            return Value(iterator.release());
        });
    
    // Add entries() method
    auto entries_fn = ObjectFactory::create_native_function("entries", 
        [](Context& ctx, const std::vector<Value>& args) -> Value {
            (void)args; // Unused parameter
            Value this_value = ctx.get_binding("this");
            if (!this_value.is_object()) {
                ctx.throw_exception(Value("Array.prototype.entries called on non-object"));
                return Value();
            }
            
            Object* array = this_value.as_object();
            auto iterator = ArrayIterator::create_entries_iterator(array);
            return Value(iterator.release());
        });
    
    array_proto->set_property("keys", Value(keys_fn.release()));
    array_proto->set_property("values", Value(values_fn.release()));
    array_proto->set_property("entries", Value(entries_fn.release()));
    
    // Add Symbol.iterator method (default to values)
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto default_iterator_fn = ObjectFactory::create_native_function("@@iterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; // Unused parameter
                Value this_value = ctx.get_binding("this");
                if (!this_value.is_object()) {
                    ctx.throw_exception(Value("Array.prototype[Symbol.iterator] called on non-object"));
                    return Value();
                }
                
                Object* array = this_value.as_object();
                auto iterator = ArrayIterator::create_values_iterator(array);
                return Value(iterator.release());
            });
        
        array_proto->set_property(iterator_symbol->to_string(), Value(default_iterator_fn.release()));
    }
}

void setup_string_iterator_methods(Context& ctx) {
    // Get String.prototype
    Value string_constructor = ctx.get_binding("String");
    if (!string_constructor.is_function()) {
        return;
    }
    
    Function* string_fn = string_constructor.as_function();
    Value string_prototype = string_fn->get_property("prototype");
    if (!string_prototype.is_object()) {
        return;
    }
    
    Object* string_proto = string_prototype.as_object();
    
    // Add Symbol.iterator method
    Symbol* iterator_symbol = Symbol::get_well_known(Symbol::ITERATOR);
    if (iterator_symbol) {
        auto string_iterator_fn = ObjectFactory::create_native_function("@@iterator", 
            [](Context& ctx, const std::vector<Value>& args) -> Value {
                (void)args; // Unused parameter
                Value this_value = ctx.get_binding("this");
                std::string str = this_value.to_string();
                
                auto iterator = std::make_unique<StringIterator>(str);
                return Value(iterator.release());
            });
        
        string_proto->set_property(iterator_symbol->to_string(), Value(string_iterator_fn.release()));
    }
}

void setup_map_iterator_methods(Context& ctx) {
    // Implementation would be similar to array iterator methods
    // but for Map objects
}

void setup_set_iterator_methods(Context& ctx) {
    // Implementation would be similar to array iterator methods
    // but for Set objects
}

} // namespace IterableUtils

} // namespace Quanta