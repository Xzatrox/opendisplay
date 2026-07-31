// OpenDisplay Windows Sender — Connection Picker Implementation
// The ConnectionPicker class is fully implemented inline in ConnectionPicker.h
// This file exists to satisfy the CMake source list requirement.
//
// Validates: Requirements 10.1, 10.2, 5.4

#include "ConnectionPicker.h"

// All methods are implemented inline in the header.
// This translation unit ensures the header is compiled and any implicit
// template instantiations are generated.

namespace OpenDisplay {
    // Force instantiation of the class by referencing it
    static_assert(sizeof(ConnectionPicker) > 0, "ConnectionPicker must be a complete type");
}
