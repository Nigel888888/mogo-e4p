#pragma once
#include <stdbool.h>

/// USB、重挂、关机等移交前等待书库句柄归还；普通导航不等。
/// Wait for mini-library handles at USB, remount and shutdown handoff, never during ordinary navigation.
bool home_library_quiesce(unsigned timeout_ms);
