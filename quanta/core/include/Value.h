#ifndef QUANTA_VALUE_H
#define QUANTA_VALUE_H

#include "Types.h"
#include <string>
#include <memory>
#include <cstdint>
#include <cmath>
#include <limits>

namespace Quanta {

// Forward declarations
class Object;
class String;
class Function;

/**
 * High-performance JavaScript value representation using NaN-boxing
 * Optimized for 64-bit systems with pointer tagging
 */
class Value {
public:
    // JavaScript value types
    enum class Type : uint8_t {
        Undefined = 0,
        Null,
        Boolean,
        Number,
        String,
        Symbol,
        BigInt,
        Object,
        Function
    };

private:
    // NaN-boxing implementation for optimal memory usage
    // Uses IEEE 754 double precision format with tagged pointers
    static constexpr uint64_t SIGN_MASK     = 0x8000000000000000ULL;
    static constexpr uint64_t EXPONENT_MASK = 0x7FF0000000000000ULL;
    static constexpr uint64_t MANTISSA_MASK = 0x000FFFFFFFFFFFFFULL;
    static constexpr uint64_t QUIET_NAN     = 0x7FF8000000000000ULL;
    static constexpr uint64_t TAG_MASK      = 0x000F000000000000ULL;
    static constexpr uint64_t PAYLOAD_MASK  = 0x0000FFFFFFFFFFFFULL;
    
    // Type tags for NaN-boxed values
    static constexpr uint64_t TAG_UNDEFINED = 0x0001000000000000ULL;
    static constexpr uint64_t TAG_NULL      = 0x0002000000000000ULL;
    static constexpr uint64_t TAG_FALSE     = 0x0003000000000000ULL;
    static constexpr uint64_t TAG_TRUE      = 0x0004000000000000ULL;
    static constexpr uint64_t TAG_STRING    = 0x0005000000000000ULL;
    static constexpr uint64_t TAG_SYMBOL    = 0x0006000000000000ULL;
    static constexpr uint64_t TAG_BIGINT    = 0x0007000000000000ULL;
    static constexpr uint64_t TAG_OBJECT    = 0x0008000000000000ULL;
    static constexpr uint64_t TAG_FUNCTION  = 0x0009000000000000ULL;

    union {
        uint64_t bits_;
        double number_;
    };

public:
    // Constructors
    Value() : bits_(QUIET_NAN | TAG_UNDEFINED) {}
    
    // Null constructor
    static Value null() {
        Value v;
        v.bits_ = QUIET_NAN | TAG_NULL;
        return v;
    }
    
    // Boolean constructors
    explicit Value(bool b) : bits_(QUIET_NAN | (b ? TAG_TRUE : TAG_FALSE)) {}
    
    // Number constructor - direct double storage
    explicit Value(double d) : number_(d) {}
    explicit Value(int32_t i) : number_(static_cast<double>(i)) {}
    explicit Value(uint32_t i) : number_(static_cast<double>(i)) {}
    explicit Value(int64_t i) : number_(static_cast<double>(i)) {}
    
    // String constructor
    explicit Value(String* str) : bits_(QUIET_NAN | TAG_STRING | reinterpret_cast<uint64_t>(str)) {}
    explicit Value(const std::string& str);
    
    // Symbol constructor
    explicit Value(class Symbol* sym) : bits_(QUIET_NAN | TAG_SYMBOL | reinterpret_cast<uint64_t>(sym)) {}
    
    // Object constructor
    explicit Value(Object* obj);
    
    // Function constructor
    explicit Value(Function* func) : bits_(QUIET_NAN | TAG_FUNCTION | reinterpret_cast<uint64_t>(func)) {}

    // Copy and move semantics
    Value(const Value& other) = default;
    Value(Value&& other) noexcept = default;
    Value& operator=(const Value& other) = default;
    Value& operator=(Value&& other) noexcept = default;

    // Type checking (highly optimized)
    inline bool is_undefined() const { return bits_ == (QUIET_NAN | TAG_UNDEFINED); }
    inline bool is_null() const { return bits_ == (QUIET_NAN | TAG_NULL); }
    inline bool is_boolean() const { 
        return (bits_ & (QUIET_NAN | TAG_MASK)) == (QUIET_NAN | TAG_FALSE) ||
               (bits_ & (QUIET_NAN | TAG_MASK)) == (QUIET_NAN | TAG_TRUE);
    }
    inline bool is_number() const { 
        return (bits_ & EXPONENT_MASK) != EXPONENT_MASK || (bits_ & TAG_MASK) == 0;
    }
    inline bool is_string() const { return (bits_ & (QUIET_NAN | TAG_MASK)) == (QUIET_NAN | TAG_STRING); }
    inline bool is_symbol() const { return (bits_ & (QUIET_NAN | TAG_MASK)) == (QUIET_NAN | TAG_SYMBOL); }
    inline bool is_bigint() const { return (bits_ & (QUIET_NAN | TAG_MASK)) == (QUIET_NAN | TAG_BIGINT); }
    inline bool is_object() const { return (bits_ & (QUIET_NAN | TAG_MASK)) == (QUIET_NAN | TAG_OBJECT); }
    inline bool is_function() const { return (bits_ & (QUIET_NAN | TAG_MASK)) == (QUIET_NAN | TAG_FUNCTION); }
    
    // Convenience type checks
    inline bool is_primitive() const { 
        return is_undefined() || is_null() || is_boolean() || is_number() || is_string() || is_symbol() || is_bigint();
    }
    inline bool is_nullish() const { return is_undefined() || is_null(); }
    inline bool is_numeric() const { return is_number() || is_bigint(); }

    // Type getter
    Type get_type() const;

    // Value extraction (optimized)
    inline bool as_boolean() const {
        return (bits_ & (QUIET_NAN | TAG_MASK)) == (QUIET_NAN | TAG_TRUE);
    }
    
    inline double as_number() const {
        return number_;
    }
    
    inline String* as_string() const {
        return reinterpret_cast<String*>(bits_ & PAYLOAD_MASK);
    }
    
    inline class Symbol* as_symbol() const {
        return reinterpret_cast<class Symbol*>(bits_ & PAYLOAD_MASK);
    }
    
    inline Object* as_object() const {
        return reinterpret_cast<Object*>(bits_ & PAYLOAD_MASK);
    }
    
    inline Function* as_function() const {
        return reinterpret_cast<Function*>(bits_ & PAYLOAD_MASK);
    }

    // JavaScript operations
    bool to_boolean() const;
    double to_number() const;
    std::string to_string() const;
    Object* to_object() const;
    
    // Comparison operations
    bool strict_equals(const Value& other) const;
    bool loose_equals(const Value& other) const;
    int compare(const Value& other) const;
    
    // Arithmetic operations
    Value add(const Value& other) const;
    Value subtract(const Value& other) const;
    Value multiply(const Value& other) const;
    Value divide(const Value& other) const;
    Value modulo(const Value& other) const;
    Value power(const Value& other) const;
    
    // Bitwise operations
    Value bitwise_and(const Value& other) const;
    Value bitwise_or(const Value& other) const;
    Value bitwise_xor(const Value& other) const;
    Value bitwise_not() const;
    Value left_shift(const Value& other) const;
    Value right_shift(const Value& other) const;
    Value unsigned_right_shift(const Value& other) const;
    
    // Unary operations
    Value unary_plus() const;
    Value unary_minus() const;
    Value logical_not() const;
    Value typeof_op() const;
    
    // Operators
    bool operator==(const Value& other) const { return strict_equals(other); }
    bool operator!=(const Value& other) const { return !strict_equals(other); }
    bool operator<(const Value& other) const { return compare(other) < 0; }
    bool operator<=(const Value& other) const { return compare(other) <= 0; }
    bool operator>(const Value& other) const { return compare(other) > 0; }
    bool operator>=(const Value& other) const { return compare(other) >= 0; }
    
    Value operator+(const Value& other) const { return add(other); }
    Value operator-(const Value& other) const { return subtract(other); }
    Value operator*(const Value& other) const { return multiply(other); }
    Value operator/(const Value& other) const { return divide(other); }
    Value operator%(const Value& other) const { return modulo(other); }
    
    Value operator&(const Value& other) const { return bitwise_and(other); }
    Value operator|(const Value& other) const { return bitwise_or(other); }
    Value operator^(const Value& other) const { return bitwise_xor(other); }
    Value operator~() const { return bitwise_not(); }
    Value operator<<(const Value& other) const { return left_shift(other); }
    Value operator>>(const Value& other) const { return right_shift(other); }
    
    Value operator+() const { return unary_plus(); }
    Value operator-() const { return unary_minus(); }
    Value operator!() const { return logical_not(); }

    // Utility methods
    std::string debug_string() const;
    size_t hash() const;
    
    // Memory management helpers
    void mark_referenced_objects() const;
    
    // Constants
    static const Value UNDEFINED;
    static const Value NULL_VALUE;
    static const Value TRUE_VALUE;
    static const Value FALSE_VALUE;
    static const Value ZERO;
    static const Value ONE;
    static const Value NAN_VALUE;
    static const Value INFINITY_VALUE;
    static const Value NEGATIVE_INFINITY_VALUE;

private:
    // Internal helpers
    static bool is_canonical_nan(uint64_t bits);
    static double number_from_string(const std::string& str);
    static std::string number_to_string(double num);
};

// Type-safe factory functions
namespace ValueFactory {
    inline Value undefined() { return Value(); }
    inline Value null() { return Value::null(); }
    inline Value boolean(bool b) { return Value(b); }
    inline Value number(double d) { return Value(d); }
    inline Value string(const std::string& s) { return Value(s); }
    Value create_function(std::unique_ptr<class Function> function_obj);
    inline Value function_placeholder(const std::string& name) { 
        // For now, create a function as a special string value
        return Value("[Function: " + name + "]"); 
    }
    
    // Common values
    inline Value zero() { return Value(0.0); }
    inline Value one() { return Value(1.0); }
    inline Value nan() { return Value(std::numeric_limits<double>::quiet_NaN()); }
    inline Value infinity() { return Value(std::numeric_limits<double>::infinity()); }
    inline Value negative_infinity() { return Value(-std::numeric_limits<double>::infinity()); }
}

} // namespace Quanta

// Hash function for std::unordered_map
namespace std {
    template<>
    struct hash<Quanta::Value> {
        size_t operator()(const Quanta::Value& v) const {
            return v.hash();
        }
    };
}

#endif // QUANTA_VALUE_H