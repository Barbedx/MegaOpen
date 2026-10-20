#pragma once
// src/secrets.h if there is one, else the committed defaults.
#if __has_include("../secrets.h")
#include "../secrets.h"
#else
#include "../secrets.example.h"
#endif
