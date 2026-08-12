#pragma once
#include <string>

// Arduino .ino sketch preprocessing.
//
// A .ino file is not valid C++ on its own. Before handing a sketch to the
// compiler the Arduino IDE performs two transformations, and ardio does the
// same here:
//
//   1. It prepends #include <Arduino.h>, which the sketch is allowed to rely
//      on without ever mentioning.
//   2. It inserts a forward declaration for every function the sketch defines
//      at file scope, so that a function may be *called* before it is
//      *defined*. C++ requires a declaration first; .ino does not.
//
// Nothing else about the sketch is rewritten. Line ordering is preserved so
// that compiler diagnostics still point roughly where the user expects.

namespace ardio {

struct SketchResult {
    bool ok = false;
    std::string error;
    std::string source;   // the generated C++ translation unit
};

// Converts .ino text into valid C++. On success `source` holds the result.
SketchResult preprocess_sketch(const std::string& ino_source);

} // namespace ardio
