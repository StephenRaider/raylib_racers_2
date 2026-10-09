#pragma once
#include <string>

// Three-letter driver codes for the timing tower and minimap (JFO, SPB ...), read from
// assets/short_names.json so other grids (team codes, a GC edition) can bring their own.
void loadShortNames(const std::string& assetsDir);
// The code for a car: its name without the leading race number, else its robot, else the
// first three letters of the name's last word.
std::string shortName(const std::string& name, const std::string& robot = "");
// The race number at the start of a car's name ("44 Dave" -> 44), 0 if none.
int carNumber(const std::string& name);
