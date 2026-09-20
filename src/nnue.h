#pragma once

// DannyNNUE NNUE evaluation interface.
//
// NNUE inference will be implemented in a later version.
// This file provides the initial interface so the search code
// can be separated from the evaluation implementation.

namespace DannyNNUE {

class NNUE {
public:
    NNUE() = default;

    int evaluate() const {
        return 0;
    }
};

}
