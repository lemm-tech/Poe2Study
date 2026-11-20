#pragma once
#include "../Common/ComboSequence.h"

namespace combo {
std::wstring DefaultConfigPath();
bool LoadConfig(const std::wstring&, std::vector<Row>&, std::string& error);
bool SaveConfig(const std::wstring&, const std::vector<Row>&, std::string& error);
} // namespace combo
