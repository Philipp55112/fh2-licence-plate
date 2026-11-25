#pragma once
#include <string>
// Erzeugt die drei Spieler-Kennzeichen-Texturen (Diffuse-Atlas, Normal, Glanz) als FH2-swatchbin.
// data: PlateEditor_eu.bin / _us.bin, text: bis 8 Zeichen (A-Z, 0-9, Leerzeichen), out[3]: Zieldateien
bool pled_generate(const std::string& data, const std::string& text, const std::string out[3], std::string& err);
