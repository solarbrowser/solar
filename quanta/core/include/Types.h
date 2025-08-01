#ifndef QUANTA_TYPES_H
#define QUANTA_TYPES_H

#include <cstdint>

namespace Quanta {

// Forward declarations
class Value;
class Object;
class Function;
class String;
class Context;
class Engine;

// Property attributes
enum PropertyAttributes : uint8_t {
    None = 0,
    Writable = 1 << 0,
    Enumerable = 1 << 1,
    Configurable = 1 << 2,
    Default = Writable | Enumerable | Configurable
};

} // namespace Quanta

#endif // QUANTA_TYPES_H