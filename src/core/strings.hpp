#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cpm::str {

std::vector<std::string> split(std::string_view text, char sep);
std::vector<std::string> split_lines(std::string_view text);  // handles \n and \r\n
std::string_view trim(std::string_view text);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string to_lower(std::string_view text);
bool iequals(std::string_view a, std::string_view b);
std::string replace_all(std::string text, std::string_view from, std::string_view to);

// Number of Unicode code points in a UTF-8 string (invalid bytes count as one each).
std::size_t utf8_length(std::string_view text);

#ifdef _WIN32
std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
#endif

}  // namespace cpm::str
